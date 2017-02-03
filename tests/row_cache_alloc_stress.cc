/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <core/distributed.hh>
#include <core/app-template.hh>
#include <core/sstring.hh>
#include <core/thread.hh>

#include "utils/managed_bytes.hh"
#include "utils/logalloc.hh"
#include "row_cache.hh"
#include "log.hh"
#include "schema_builder.hh"
#include "memtable.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

static
partition_key new_key(schema_ptr s) {
    static thread_local int next = 0;
    return partition_key::from_single_value(*s, to_bytes(sprint("key%d", next++)));
}

static
clustering_key new_ckey(schema_ptr s) {
    static thread_local int next = 0;
    return clustering_key::from_single_value(*s, to_bytes(sprint("ckey%d", next++)));
}

int main(int argc, char** argv) {
    namespace bpo = boost::program_options;
    app_template app;
    app.add_options()
        ("debug", "enable debug logging");

    return app.run(argc, argv, [&app] {
        if (app.configuration().count("debug")) {
            logging::logger_registry().set_all_loggers_level(logging::log_level::debug);
        }

        // This test is supposed to verify that when we're low on memory but
        // we still have plenty of evictable memory in cache, we should be
        // able to populate cache with large mutations This test works only
        // with seastar's allocator.
        return seastar::async([] {
            auto s = schema_builder("ks", "cf")
                .with_column("pk", bytes_type, column_kind::partition_key)
                .with_column("ck", bytes_type, column_kind::clustering_key)
                .with_column("v", bytes_type, column_kind::regular_column)
                .build();

            auto mt0 = make_lw_shared<memtable>(s);

            cache_tracker tracker;
            row_cache cache(s, mt0->as_data_source(), tracker);

            auto mt = make_lw_shared<memtable>(s);
            std::vector<dht::decorated_key> keys;

            size_t cell_size = 1024;
            size_t row_count = 40 * 1024; // 40M mutations
            size_t large_cell_size = cell_size * row_count;

            auto make_small_mutation = [&] {
                mutation m(new_key(s), s);
                m.set_clustered_cell(new_ckey(s), "v", data_value(bytes(bytes::initialized_later(), cell_size)), 1);
                return m;
            };

            auto make_large_mutation = [&] {
                mutation m(new_key(s), s);
                m.set_clustered_cell(new_ckey(s), "v", data_value(bytes(bytes::initialized_later(), large_cell_size)), 2);
                return m;
            };

            for (int i = 0; i < 10; i++) {
                auto key = dht::global_partitioner().decorate_key(*s, new_key(s));

                mutation m1(key, s);
                m1.set_clustered_cell(new_ckey(s), "v", data_value(bytes(bytes::initialized_later(), cell_size)), 1);
                cache.populate(m1);

                // Putting large mutations into the memtable. Should take about row_count*cell_size each.
                mutation m2(key, s);
                for (size_t j = 0; j < row_count; j++) {
                    m2.set_clustered_cell(new_ckey(s), "v", data_value(bytes(bytes::initialized_later(), cell_size)), 2);
                }

                mt->apply(m2);
                keys.push_back(key);
            }

            auto reclaimable_memory = [] {
                return memory::stats().free_memory() + logalloc::shard_tracker().occupancy().free_space();
            };

            std::cout << "memtable occupancy: " << mt->occupancy() << "\n";
            std::cout << "Cache occupancy: " << tracker.region().occupancy() << "\n";
            std::cout << "Reclaimable memory: " << reclaimable_memory() << "\n";

            // We need to have enough Free memory to copy memtable into cache
            // When this assertion fails, increase amount of memory
            assert(mt->occupancy().used_space() < reclaimable_memory());

            auto checker = [](auto) {
                return partition_presence_checker_result::maybe_exists;
            };

            std::deque<dht::decorated_key> cache_stuffing;
            auto fill_cache_to_the_top = [&] {
                std::cout << "Filling up memory with evictable data\n";
                while (true) {
                    // Ensure that entries matching memtable partitions are evicted
                    // last, we want to hit the merge path in row_cache::update()
                    for (auto&& key : keys) {
                        cache.touch(key);
                    }
                    auto occupancy_before = tracker.region().occupancy().used_space();
                    auto m = make_small_mutation();
                    cache_stuffing.push_back(m.decorated_key());
                    cache.populate(m);
                    if (tracker.region().occupancy().used_space() <= occupancy_before) {
                        break;
                    }
                }
                std::cout << "Shuffling..\n";
                // Evict in random order to create fragmentation.
                std::random_shuffle(cache_stuffing.begin(), cache_stuffing.end());
                for (auto&& key : cache_stuffing) {
                    cache.touch(key);
                }
                // Ensure that entries matching memtable partitions are evicted
                // last, we want to hit the merge path in row_cache::update()
                for (auto&& key : keys) {
                    cache.touch(key);
                }
                std::cout << "Reclaimable memory: " << reclaimable_memory() << "\n";
                std::cout << "Cache occupancy: " << tracker.region().occupancy() << "\n";
            };

            std::deque<std::unique_ptr<char[]>> stuffing;
            auto fragment_free_space = [&] {
                stuffing.clear();
                std::cout << "Reclaimable memory: " << reclaimable_memory() << "\n";
                std::cout << "Free memory: " << memory::stats().free_memory() << "\n";
                std::cout << "Cache occupancy: " << tracker.region().occupancy() << "\n";

                // Induce memory fragmentation by taking down cache segments,
                // which should be evicted in random order, and inducing high
                // waste level in them. Should leave around up to 100M free,
                // but no LSA segment should fit.
                for (unsigned i = 0; i < 100 * 1024 * 1024 / (logalloc::segment_size / 2); ++i) {
                    stuffing.emplace_back(std::make_unique<char[]>(logalloc::segment_size / 2 + 1));
                }

                std::cout << "After fragmenting:\n";
                std::cout << "Reclaimable memory: " << reclaimable_memory() << "\n";
                std::cout << "Free memory: " << memory::stats().free_memory() << "\n";
                std::cout << "Cache occupancy: " << tracker.region().occupancy() << "\n";
            };

            fill_cache_to_the_top();

            fragment_free_space();

            cache.update(*mt, checker).get();

            stuffing.clear();
            cache_stuffing.clear();

            // Verify that all mutations from memtable went through
            for (auto&& key : keys) {
                auto range = dht::partition_range::make_singular(key);
                auto reader = cache.make_reader(s, range);
                auto mo = mutation_from_streamed_mutation(reader().get0()).get0();
                assert(mo);
                assert(mo->partition().live_row_count(*s) ==
                       row_count + 1 /* one row was already in cache before update()*/);
            }

            std::cout << "Testing reading from cache.\n";

            fill_cache_to_the_top();

            for (auto&& key : keys) {
                cache.touch(key);
            }

            for (auto&& key : keys) {
                auto range = dht::partition_range::make_singular(key);
                auto reader = cache.make_reader(s, range);
                auto mo = reader().get0();
                assert(mo);
            }

            std::cout << "Testing reading when memory can't be reclaimed.\n";
            // We want to check that when we really can't reserve memory, allocating_section
            // throws rather than enter infinite loop.
            {
                stuffing.clear();
                cache_stuffing.clear();
                tracker.clear();

                // eviction victims
                for (unsigned i = 0; i < logalloc::segment_size / cell_size; ++i) {
                    cache.populate(make_small_mutation());
                }

                const mutation& m = make_large_mutation();
                auto range = dht::partition_range::make_singular(m.decorated_key());

                cache.populate(m);

                logalloc::shard_tracker().reclaim_all_free_segments();

                {
                    logalloc::reclaim_lock _(tracker.region());
                    try {
                        while (true) {
                            stuffing.emplace_back(std::make_unique<char[]>(logalloc::segment_size));
                        }
                    } catch (const std::bad_alloc&) {
                        //expected
                    }
                }

                try {
                    auto reader = cache.make_reader(s, range);
                    assert(!reader().get0());
                    auto evicted_from_cache = logalloc::segment_size + large_cell_size;
                    new char[evicted_from_cache + logalloc::segment_size];
                    assert(false); // The test is not invoking the case which it's supposed to test
                } catch (const std::bad_alloc&) {
                    // expected
                }
            }
        });
    });
}
