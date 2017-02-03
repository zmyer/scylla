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


#include <random>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>
#include "mutation_query.hh"
#include "md5_hasher.hh"

#include "core/sstring.hh"
#include "core/do_with.hh"
#include "core/thread.hh"

#include "database.hh"
#include "utils/UUID_gen.hh"
#include "mutation_reader.hh"
#include "schema_builder.hh"
#include "query-result-set.hh"
#include "query-result-reader.hh"
#include "partition_slice_builder.hh"
#include "tmpdir.hh"

#include "tests/test-utils.hh"
#include "tests/mutation_assertions.hh"
#include "tests/mutation_reader_assertions.hh"
#include "tests/result_set_assertions.hh"
#include "mutation_source_test.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

using namespace std::chrono_literals;

static sstring some_keyspace("ks");
static sstring some_column_family("cf");

static atomic_cell make_atomic_cell(bytes value) {
    return atomic_cell::make_live(0, std::move(value));
};

static mutation_partition get_partition(memtable& mt, const partition_key& key) {
    auto dk = dht::global_partitioner().decorate_key(*mt.schema(), key);
    auto reader = mt.make_reader(mt.schema(), dht::partition_range::make_singular(dk));
    auto mo = mutation_from_streamed_mutation(reader().get0()).get0();
    BOOST_REQUIRE(bool(mo));
    return std::move(mo->partition());
}

template <typename Func>
future<>
with_column_family(schema_ptr s, column_family::config cfg, Func func) {
    auto dir = make_lw_shared<tmpdir>();
    cfg.datadir = { dir->path };
    auto cm = make_lw_shared<compaction_manager>();
    auto cf = make_lw_shared<column_family>(s, cfg, column_family::no_commitlog(), *cm);
    cf->mark_ready_for_writes();
    return func(*cf).then([cf, cm] {
        return cf->stop();
    }).finally([cf, cm, dir] {});
}

SEASTAR_TEST_CASE(test_mutation_is_applied) {
    return seastar::async([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", int32_type}}, {{"r1", int32_type}}, {}, utf8_type));

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(2)});

        mutation m(key, s);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(3)));
        mt->apply(std::move(m));

        auto p = get_partition(*mt, key);
        row& r = p.clustered_row(*s, c_key).cells();
        auto i = r.find_cell(r1_col.id);
        BOOST_REQUIRE(i);
        auto cell = i->as_atomic_cell();
        BOOST_REQUIRE(cell.is_live());
        BOOST_REQUIRE(int32_type->equal(cell.value(), int32_type->decompose(3)));
    });
}

SEASTAR_TEST_CASE(test_multi_level_row_tombstones) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}},
        {{"c1", int32_type}, {"c2", int32_type}, {"c3", int32_type}},
        {{"r1", int32_type}}, {}, utf8_type));

    auto ttl = gc_clock::now() + std::chrono::seconds(1);

    mutation m(partition_key::from_exploded(*s, {to_bytes("key1")}), s);

    auto make_prefix = [s] (const std::vector<data_value>& v) {
        return clustering_key_prefix::from_deeply_exploded(*s, v);
    };
    auto make_key = [s] (const std::vector<data_value>& v) {
        return clustering_key::from_deeply_exploded(*s, v);
    };

    m.partition().apply_row_tombstone(*s, make_prefix({1, 2}), tombstone(9, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, make_key({1, 2, 3})), tombstone(9, ttl));

    m.partition().apply_row_tombstone(*s, make_prefix({1, 3}), tombstone(8, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, make_key({1, 2, 0})), tombstone(9, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, make_key({1, 3, 0})), tombstone(8, ttl));

    m.partition().apply_row_tombstone(*s, make_prefix({1}), tombstone(11, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, make_key({1, 2, 0})), tombstone(11, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, make_key({1, 3, 0})), tombstone(11, ttl));

    m.partition().apply_row_tombstone(*s, make_prefix({1, 4}), tombstone(6, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, make_key({1, 2, 0})), tombstone(11, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, make_key({1, 3, 0})), tombstone(11, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, make_key({1, 4, 0})), tombstone(11, ttl));
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_row_tombstone_updates) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {{"c1", int32_type}, {"c2", int32_type}}, {{"r1", int32_type}}, {}, utf8_type));

    auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
    auto c_key1 = clustering_key::from_deeply_exploded(*s, {1, 0});
    auto c_key1_prefix = clustering_key_prefix::from_deeply_exploded(*s, {1});
    auto c_key2 = clustering_key::from_deeply_exploded(*s, {2, 0});
    auto c_key2_prefix = clustering_key_prefix::from_deeply_exploded(*s, {2});

    auto ttl = gc_clock::now() + std::chrono::seconds(1);

    mutation m(key, s);
    m.partition().apply_row_tombstone(*s, c_key1_prefix, tombstone(1, ttl));
    m.partition().apply_row_tombstone(*s, c_key2_prefix, tombstone(0, ttl));

    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, c_key1), tombstone(1, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, c_key2), tombstone(0, ttl));

    m.partition().apply_row_tombstone(*s, c_key2_prefix, tombstone(1, ttl));
    BOOST_REQUIRE_EQUAL(m.partition().tombstone_for_row(*s, c_key2), tombstone(1, ttl));
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_map_mutations) {
    return seastar::async([] {
        auto my_map_type = map_type_impl::get_instance(int32_type, utf8_type, true);
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", int32_type}}, {}, {{"s1", my_map_type}}, utf8_type));
        auto mt = make_lw_shared<memtable>(s);
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto& column = *s->get_column_definition("s1");
        map_type_impl::mutation mmut1{{}, {{int32_type->decompose(101), make_atomic_cell(utf8_type->decompose(sstring("101")))}}};
        mutation m1(key, s);
        m1.set_static_cell(column, my_map_type->serialize_mutation_form(mmut1));
        mt->apply(m1);
        map_type_impl::mutation mmut2{{}, {{int32_type->decompose(102), make_atomic_cell(utf8_type->decompose(sstring("102")))}}};
        mutation m2(key, s);
        m2.set_static_cell(column, my_map_type->serialize_mutation_form(mmut2));
        mt->apply(m2);
        map_type_impl::mutation mmut3{{}, {{int32_type->decompose(103), make_atomic_cell(utf8_type->decompose(sstring("103")))}}};
        mutation m3(key, s);
        m3.set_static_cell(column, my_map_type->serialize_mutation_form(mmut3));
        mt->apply(m3);
        map_type_impl::mutation mmut2o{{}, {{int32_type->decompose(102), make_atomic_cell(utf8_type->decompose(sstring("102 override")))}}};
        mutation m2o(key, s);
        m2o.set_static_cell(column, my_map_type->serialize_mutation_form(mmut2o));
        mt->apply(m2o);

        auto p = get_partition(*mt, key);
        row& r = p.static_row();
        auto i = r.find_cell(column.id);
        BOOST_REQUIRE(i);
        auto cell = i->as_collection_mutation();
        auto muts = my_map_type->deserialize_mutation_form(cell);
        BOOST_REQUIRE(muts.cells.size() == 3);
        // FIXME: more strict tests
    });
}

SEASTAR_TEST_CASE(test_set_mutations) {
    return seastar::async([] {
        auto my_set_type = set_type_impl::get_instance(int32_type, true);
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", int32_type}}, {}, {{"s1", my_set_type}}, utf8_type));
        auto mt = make_lw_shared<memtable>(s);
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto& column = *s->get_column_definition("s1");
        map_type_impl::mutation mmut1{{}, {{int32_type->decompose(101), make_atomic_cell({})}}};
        mutation m1(key, s);
        m1.set_static_cell(column, my_set_type->serialize_mutation_form(mmut1));
        mt->apply(m1);
        map_type_impl::mutation mmut2{{}, {{int32_type->decompose(102), make_atomic_cell({})}}};
        mutation m2(key, s);
        m2.set_static_cell(column, my_set_type->serialize_mutation_form(mmut2));
        mt->apply(m2);
        map_type_impl::mutation mmut3{{}, {{int32_type->decompose(103), make_atomic_cell({})}}};
        mutation m3(key, s);
        m3.set_static_cell(column, my_set_type->serialize_mutation_form(mmut3));
        mt->apply(m3);
        map_type_impl::mutation mmut2o{{}, {{int32_type->decompose(102), make_atomic_cell({})}}};
        mutation m2o(key, s);
        m2o.set_static_cell(column, my_set_type->serialize_mutation_form(mmut2o));
        mt->apply(m2o);

        auto p = get_partition(*mt, key);
        row& r = p.static_row();
        auto i = r.find_cell(column.id);
        BOOST_REQUIRE(i);
        auto cell = i->as_collection_mutation();
        auto muts = my_set_type->deserialize_mutation_form(cell);
        BOOST_REQUIRE(muts.cells.size() == 3);
        // FIXME: more strict tests
    });
}

SEASTAR_TEST_CASE(test_list_mutations) {
    return seastar::async([] {
        auto my_list_type = list_type_impl::get_instance(int32_type, true);
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", int32_type}}, {}, {{"s1", my_list_type}}, utf8_type));
        auto mt = make_lw_shared<memtable>(s);
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto& column = *s->get_column_definition("s1");
        auto make_key = [] { return timeuuid_type->decompose(utils::UUID_gen::get_time_UUID()); };
        collection_type_impl::mutation mmut1{{}, {{make_key(), make_atomic_cell(int32_type->decompose(101))}}};
        mutation m1(key, s);
        m1.set_static_cell(column, my_list_type->serialize_mutation_form(mmut1));
        mt->apply(m1);
        collection_type_impl::mutation mmut2{{}, {{make_key(), make_atomic_cell(int32_type->decompose(102))}}};
        mutation m2(key, s);
        m2.set_static_cell(column, my_list_type->serialize_mutation_form(mmut2));
        mt->apply(m2);
        collection_type_impl::mutation mmut3{{}, {{make_key(), make_atomic_cell(int32_type->decompose(103))}}};
        mutation m3(key, s);
        m3.set_static_cell(column, my_list_type->serialize_mutation_form(mmut3));
        mt->apply(m3);
        collection_type_impl::mutation mmut2o{{}, {{make_key(), make_atomic_cell(int32_type->decompose(102))}}};
        mutation m2o(key, s);
        m2o.set_static_cell(column, my_list_type->serialize_mutation_form(mmut2o));
        mt->apply(m2o);

        auto p = get_partition(*mt, key);
        row& r = p.static_row();
        auto i = r.find_cell(column.id);
        BOOST_REQUIRE(i);
        auto cell = i->as_collection_mutation();
        auto muts = my_list_type->deserialize_mutation_form(cell);
        BOOST_REQUIRE(muts.cells.size() == 4);
        // FIXME: more strict tests
    });
}

SEASTAR_TEST_CASE(test_multiple_memtables_one_partition) {
    return seastar::async([] {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {{"c1", int32_type}}, {{"r1", int32_type}}, {}, utf8_type));

    auto cf_stats = make_lw_shared<::cf_stats>();
    column_family::config cfg;
    cfg.enable_disk_reads = false;
    cfg.enable_disk_writes = false;
    cfg.enable_incremental_backups = false;
    cfg.cf_stats = &*cf_stats;

    with_column_family(s, cfg, [s] (column_family& cf) {
        const column_definition& r1_col = *s->get_column_definition("r1");
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});

        auto insert_row = [&] (int32_t c1, int32_t r1) {
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(c1)});
            mutation m(key, s);
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(r1)));
            cf.apply(std::move(m));
            return cf.flush();
        };
        insert_row(1001, 2001).get();
        insert_row(1002, 2002).get();
        insert_row(1003, 2003).get();
        {
            auto verify_row = [&] (int32_t c1, int32_t r1) {
                auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(c1)});
                auto p_key = dht::global_partitioner().decorate_key(*s, key);
                auto r = cf.find_row(cf.schema(), p_key, c_key).get0();
                {
                    BOOST_REQUIRE(r);
                    auto i = r->find_cell(r1_col.id);
                    BOOST_REQUIRE(i);
                    auto cell = i->as_atomic_cell();
                    BOOST_REQUIRE(cell.is_live());
                    BOOST_REQUIRE(int32_type->equal(cell.value(), int32_type->decompose(r1)));
                }
            };
            verify_row(1001, 2001);
            verify_row(1002, 2002);
            verify_row(1003, 2003);
        }
        return make_ready_future<>();
    }).get();
    });
}

SEASTAR_TEST_CASE(test_flush_in_the_middle_of_a_scan) {
    auto s = schema_builder("ks", "cf")
        .with_column("pk", bytes_type, column_kind::partition_key)
        .with_column("v", bytes_type)
        .build();

    auto cf_stats = make_lw_shared<::cf_stats>();

    column_family::config cfg;
    cfg.enable_disk_reads = true;
    cfg.enable_disk_writes = true;
    cfg.enable_cache = true;
    cfg.enable_incremental_backups = false;
    cfg.cf_stats = &*cf_stats;

    return with_column_family(s, cfg, [s](column_family& cf) {
        return seastar::async([s, &cf] {
            // populate
            auto new_key = [&] {
                static thread_local int next = 0;
                return dht::global_partitioner().decorate_key(*s,
                    partition_key::from_single_value(*s, to_bytes(sprint("key%d", next++))));
            };
            auto make_mutation = [&] {
                mutation m(new_key(), s);
                m.set_clustered_cell(clustering_key::make_empty(), "v", data_value(to_bytes("value")), 1);
                return m;
            };

            std::vector<mutation> mutations;
            for (int i = 0; i < 1000; ++i) {
                auto m = make_mutation();
                cf.apply(m);
                mutations.emplace_back(std::move(m));
            }

            std::sort(mutations.begin(), mutations.end(), mutation_decorated_key_less_comparator());

            // Flush will happen in the middle of reading for this scanner
            auto assert_that_scanner1 = assert_that(cf.make_reader(s, query::full_partition_range));

            // Flush will happen before it is invoked
            auto assert_that_scanner2 = assert_that(cf.make_reader(s, query::full_partition_range));

            // Flush will happen after all data was read, but before EOS was consumed
            auto assert_that_scanner3 = assert_that(cf.make_reader(s, query::full_partition_range));

            assert_that_scanner1.produces(mutations[0]);
            assert_that_scanner1.produces(mutations[1]);

            for (unsigned i = 0; i < mutations.size(); ++i) {
                assert_that_scanner3.produces(mutations[i]);
            }

            memtable& m = cf.active_memtable(); // held by scanners

            auto flushed = cf.flush();

            while (!m.is_flushed()) {
                sleep(10ms).get();
            }

            for (unsigned i = 2; i < mutations.size(); ++i) {
                assert_that_scanner1.produces(mutations[i]);
            }
            assert_that_scanner1.produces_end_of_stream();

            for (unsigned i = 0; i < mutations.size(); ++i) {
                assert_that_scanner2.produces(mutations[i]);
            }
            assert_that_scanner2.produces_end_of_stream();

            assert_that_scanner3.produces_end_of_stream();

            flushed.get();
        });
    }).then([cf_stats] {});
}

SEASTAR_TEST_CASE(test_multiple_memtables_multiple_partitions) {
    auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", int32_type}}, {{"c1", int32_type}}, {{"r1", int32_type}}, {}, utf8_type));

    auto cf_stats = make_lw_shared<::cf_stats>();

    column_family::config cfg;
    cfg.enable_disk_reads = false;
    cfg.enable_disk_writes = false;
    cfg.enable_incremental_backups = false;
    cfg.cf_stats = &*cf_stats;
    return with_column_family(s, cfg, [s] (auto& cf) mutable {
        std::map<int32_t, std::map<int32_t, int32_t>> shadow, result;

        const column_definition& r1_col = *s->get_column_definition("r1");

        api::timestamp_type ts = 0;
        auto insert_row = [&] (int32_t p1, int32_t c1, int32_t r1) {
            auto key = partition_key::from_exploded(*s, {int32_type->decompose(p1)});
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(c1)});
            mutation m(key, s);
            m.set_clustered_cell(c_key, r1_col, atomic_cell::make_live(ts++, int32_type->decompose(r1)));
            cf.apply(std::move(m));
            shadow[p1][c1] = r1;
        };
        std::minstd_rand random_engine;
        std::normal_distribution<> pk_distribution(0, 10);
        std::normal_distribution<> ck_distribution(0, 5);
        std::normal_distribution<> r_distribution(0, 100);
        for (unsigned i = 0; i < 10; ++i) {
            for (unsigned j = 0; j < 100; ++j) {
                insert_row(pk_distribution(random_engine), ck_distribution(random_engine), r_distribution(random_engine));
            }
            cf.flush();
        }

        return do_with(std::move(result), [&cf, s, &r1_col, shadow] (auto& result) {
            return cf.for_all_partitions_slow(s, [&, s] (const dht::decorated_key& pk, const mutation_partition& mp) {
                auto p1 = value_cast<int32_t>(int32_type->deserialize(pk._key.explode(*s)[0]));
                for (const rows_entry& re : mp.range(*s, nonwrapping_range<clustering_key_prefix>())) {
                    auto c1 = value_cast<int32_t>(int32_type->deserialize(re.key().explode(*s)[0]));
                    auto cell = re.row().cells().find_cell(r1_col.id);
                    if (cell) {
                        result[p1][c1] = value_cast<int32_t>(int32_type->deserialize(cell->as_atomic_cell().value()));
                    }
                }
                return true;
            }).then([&result, shadow] (bool ok) {
                BOOST_REQUIRE(shadow == result);
            });
        });
    }).then([cf_stats] {});
}

SEASTAR_TEST_CASE(test_cell_ordering) {
    auto now = gc_clock::now();
    auto ttl_1 = gc_clock::duration(1);
    auto ttl_2 = gc_clock::duration(2);
    auto expiry_1 = now + ttl_1;
    auto expiry_2 = now + ttl_2;

    auto assert_order = [] (atomic_cell_view first, atomic_cell_view second) {
        if (compare_atomic_cell_for_merge(first, second) >= 0) {
            BOOST_FAIL(sprint("Expected %s < %s", first, second));
        }
        if (compare_atomic_cell_for_merge(second, first) <= 0) {
            BOOST_FAIL(sprint("Expected %s < %s", second, first));
        }
    };

    auto assert_equal = [] (atomic_cell_view c1, atomic_cell_view c2) {
        BOOST_REQUIRE(compare_atomic_cell_for_merge(c1, c2) == 0);
        BOOST_REQUIRE(compare_atomic_cell_for_merge(c2, c1) == 0);
    };

    assert_equal(
        atomic_cell::make_live(0, bytes("value")),
        atomic_cell::make_live(0, bytes("value")));

    assert_equal(
        atomic_cell::make_live(1, bytes("value"), expiry_1, ttl_1),
        atomic_cell::make_live(1, bytes("value")));

    assert_equal(
        atomic_cell::make_dead(1, expiry_1),
        atomic_cell::make_dead(1, expiry_1));

    // If one cell doesn't have an expiry, Origin considers them equal.
    assert_equal(
        atomic_cell::make_live(1, bytes(), expiry_2, ttl_2),
        atomic_cell::make_live(1, bytes()));

    // Origin doesn't compare ttl (is it wise?)
    assert_equal(
        atomic_cell::make_live(1, bytes("value"), expiry_1, ttl_1),
        atomic_cell::make_live(1, bytes("value"), expiry_1, ttl_2));

    assert_order(
        atomic_cell::make_live(0, bytes("value1")),
        atomic_cell::make_live(0, bytes("value2")));

    assert_order(
        atomic_cell::make_live(0, bytes("value12")),
        atomic_cell::make_live(0, bytes("value2")));

    // Live cells are ordered first by timestamp...
    assert_order(
        atomic_cell::make_live(0, bytes("value2")),
        atomic_cell::make_live(1, bytes("value1")));

    // ..then by value
    assert_order(
        atomic_cell::make_live(1, bytes("value1"), expiry_2, ttl_2),
        atomic_cell::make_live(1, bytes("value2"), expiry_1, ttl_1));

    // ..then by expiry
    assert_order(
        atomic_cell::make_live(1, bytes(), expiry_1, ttl_1),
        atomic_cell::make_live(1, bytes(), expiry_2, ttl_1));

    // Dead wins
    assert_order(
        atomic_cell::make_live(1, bytes("value")),
        atomic_cell::make_dead(1, expiry_1));

    // Dead wins with expiring cell
    assert_order(
        atomic_cell::make_live(1, bytes("value"), expiry_2, ttl_2),
        atomic_cell::make_dead(1, expiry_1));

    // Deleted cells are ordered first by timestamp
    assert_order(
        atomic_cell::make_dead(1, expiry_2),
        atomic_cell::make_dead(2, expiry_1));

    // ...then by expiry
    assert_order(
        atomic_cell::make_dead(1, expiry_1),
        atomic_cell::make_dead(1, expiry_2));
    return make_ready_future<>();
}

static query::partition_slice make_full_slice(const schema& s) {
    return partition_slice_builder(s).build();
}

SEASTAR_TEST_CASE(test_querying_of_mutation) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("v", bytes_type, column_kind::regular_column)
            .build();

        auto resultify = [s] (const mutation& m) -> query::result_set {
            auto slice = make_full_slice(*s);
            return query::result_set::from_raw_result(s, slice, m.query(slice));
        };

        mutation m(partition_key::from_single_value(*s, "key1"), s);
        m.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v1")), 1);

        assert_that(resultify(m))
            .has_only(a_row()
                .with_column("pk", data_value(bytes("key1")))
                .with_column("v", data_value(bytes("v1"))));

        m.partition().apply(tombstone(2, gc_clock::now()));

        assert_that(resultify(m)).is_empty();
    });
}

SEASTAR_TEST_CASE(test_partition_with_no_live_data_is_absent_in_data_query_results) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("sc1", bytes_type, column_kind::static_column)
            .with_column("ck", bytes_type, column_kind::clustering_key)
            .with_column("v", bytes_type, column_kind::regular_column)
            .build();

        mutation m(partition_key::from_single_value(*s, "key1"), s);
        m.partition().apply(tombstone(1, gc_clock::now()));
        m.partition().static_row().apply(*s->get_column_definition("sc1"),
            atomic_cell::make_dead(2, gc_clock::now()));
        m.set_clustered_cell(clustering_key::from_single_value(*s, bytes_type->decompose(data_value(bytes("A")))),
            *s->get_column_definition("v"), atomic_cell::make_dead(2, gc_clock::now()));

        auto slice = make_full_slice(*s);

        assert_that(query::result_set::from_raw_result(s, slice, m.query(slice)))
            .is_empty();
    });
}

SEASTAR_TEST_CASE(test_partition_with_live_data_in_static_row_is_present_in_the_results_even_if_static_row_was_not_queried) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("sc1", bytes_type, column_kind::static_column)
            .with_column("ck", bytes_type, column_kind::clustering_key)
            .with_column("v", bytes_type, column_kind::regular_column)
            .build();

        mutation m(partition_key::from_single_value(*s, "key1"), s);
        m.partition().static_row().apply(*s->get_column_definition("sc1"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("sc1:value")))));

        auto slice = partition_slice_builder(*s)
            .with_no_static_columns()
            .with_regular_column("v")
            .build();

        assert_that(query::result_set::from_raw_result(s, slice, m.query(slice)))
            .has_only(a_row()
                .with_column("pk", data_value(bytes("key1")))
                .with_column("v", data_value::make_null(bytes_type)));
    });
}

SEASTAR_TEST_CASE(test_query_result_with_one_regular_column_missing) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("ck", bytes_type, column_kind::clustering_key)
            .with_column("v1", bytes_type, column_kind::regular_column)
            .with_column("v2", bytes_type, column_kind::regular_column)
            .build();

        mutation m(partition_key::from_single_value(*s, "key1"), s);
        m.set_clustered_cell(clustering_key::from_single_value(*s, bytes("ck:A")),
            *s->get_column_definition("v1"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v1:value")))));

        auto slice = partition_slice_builder(*s).build();

        assert_that(query::result_set::from_raw_result(s, slice, m.query(slice)))
            .has_only(a_row()
                .with_column("pk", data_value(bytes("key1")))
                .with_column("ck", data_value(bytes("ck:A")))
                .with_column("v1", data_value(bytes("v1:value")))
                .with_column("v2", data_value::make_null(bytes_type)));
    });
}

SEASTAR_TEST_CASE(test_row_counting) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("sc1", bytes_type, column_kind::static_column)
            .with_column("ck", bytes_type, column_kind::clustering_key)
            .with_column("v", bytes_type, column_kind::regular_column)
            .build();

        auto col_v = *s->get_column_definition("v");

        mutation m(partition_key::from_single_value(*s, "key1"), s);

        BOOST_REQUIRE_EQUAL(0, m.live_row_count());

        auto ckey1 = clustering_key::from_single_value(*s, bytes_type->decompose(data_value(bytes("A"))));
        auto ckey2 = clustering_key::from_single_value(*s, bytes_type->decompose(data_value(bytes("B"))));

        m.set_clustered_cell(ckey1, col_v, atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v:value")))));

        BOOST_REQUIRE_EQUAL(1, m.live_row_count());

        m.partition().static_row().apply(*s->get_column_definition("sc1"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("sc1:value")))));

        BOOST_REQUIRE_EQUAL(1, m.live_row_count());

        m.set_clustered_cell(ckey1, col_v, atomic_cell::make_dead(2, gc_clock::now()));

        BOOST_REQUIRE_EQUAL(1, m.live_row_count());

        m.partition().static_row().apply(*s->get_column_definition("sc1"),
            atomic_cell::make_dead(2, gc_clock::now()));

        BOOST_REQUIRE_EQUAL(0, m.live_row_count());

        m.partition().clustered_row(*s, ckey1).apply(api::timestamp_type(3));

        BOOST_REQUIRE_EQUAL(1, m.live_row_count());

        m.partition().apply(tombstone(3, gc_clock::now()));

        BOOST_REQUIRE_EQUAL(0, m.live_row_count());

        m.set_clustered_cell(ckey1, col_v, atomic_cell::make_live(4, bytes_type->decompose(data_value(bytes("v:value")))));
        m.set_clustered_cell(ckey2, col_v, atomic_cell::make_live(4, bytes_type->decompose(data_value(bytes("v:value")))));

        BOOST_REQUIRE_EQUAL(2, m.live_row_count());
    });
}

SEASTAR_TEST_CASE(test_tombstone_apply) {
    auto s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("v", bytes_type, column_kind::regular_column)
            .build();

    auto pkey = partition_key::from_single_value(*s, "key1");

    mutation m1(pkey, s);

    BOOST_REQUIRE_EQUAL(m1.partition().partition_tombstone(), tombstone());

    mutation m2(pkey, s);
    auto tomb = tombstone(api::new_timestamp(), gc_clock::now());
    m2.partition().apply(tomb);
    BOOST_REQUIRE_EQUAL(m2.partition().partition_tombstone(), tomb);

    m1.apply(m2);

    BOOST_REQUIRE_EQUAL(m1.partition().partition_tombstone(), tomb);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_marker_apply) {
    auto s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("ck", bytes_type, column_kind::clustering_key)
            .with_column("v", bytes_type, column_kind::regular_column)
            .build();

    auto pkey = partition_key::from_single_value(*s, "pk1");
    auto ckey = clustering_key::from_single_value(*s, "ck1");

    auto mutation_with_marker = [&] (row_marker rm) {
        mutation m(pkey, s);
        m.partition().clustered_row(*s, ckey).marker() = rm;
        return m;
    };

    {
        mutation m(pkey, s);
        auto marker = row_marker(api::new_timestamp());
        auto mm = mutation_with_marker(marker);
        m.apply(mm);
        BOOST_REQUIRE_EQUAL(m.partition().clustered_row(*s, ckey).marker(), marker);
    }

    {
        mutation m(pkey, s);
        auto marker = row_marker(api::new_timestamp(), std::chrono::seconds(1), gc_clock::now());
        m.apply(mutation_with_marker(marker));
        BOOST_REQUIRE_EQUAL(m.partition().clustered_row(*s, ckey).marker(), marker);
    }

    return make_ready_future<>();
}

class failure_injecting_allocation_strategy : public allocation_strategy {
    allocation_strategy& _delegate;
    uint64_t _alloc_count;
    uint64_t _fail_at = std::numeric_limits<uint64_t>::max();
public:
    failure_injecting_allocation_strategy(allocation_strategy& delegate) : _delegate(delegate) {}

    virtual void* alloc(migrate_fn mf, size_t size, size_t alignment) override {
        if (_alloc_count >= _fail_at) {
            stop_failing();
            throw std::bad_alloc();
        }
        ++_alloc_count;
        return _delegate.alloc(mf, size, alignment);
    }

    virtual void free(void* ptr) override {
        _delegate.free(ptr);
    }

    virtual size_t object_memory_size_in_allocator(const void* obj) const noexcept override {
        return _delegate.object_memory_size_in_allocator(obj);
    }

    // Counts allocation attempts which are not failed due to fail_at().
    uint64_t alloc_count() const {
        return _alloc_count;
    }

    void fail_after(uint64_t count) {
        _fail_at = _alloc_count + count;
    }

    void stop_failing() {
        _fail_at = std::numeric_limits<uint64_t>::max();
    }
};

SEASTAR_TEST_CASE(test_apply_is_atomic_in_case_of_allocation_failures) {
    random_mutation_generator gen;

    failure_injecting_allocation_strategy alloc(standard_allocator());
    with_allocator(alloc, [&] {
        auto target = gen();

        BOOST_TEST_MESSAGE(sprint("Target: %s", target));

        for (int i = 0; i < 10; ++i) {
            auto second = gen();

            BOOST_TEST_MESSAGE(sprint("Second: %s", second));

            auto expected_apply_result = target;
            expected_apply_result.apply(second);

            BOOST_TEST_MESSAGE(sprint("Expected: %s", expected_apply_result));

            // Test the apply(const mutation&) variant
            {
                auto m = target;

                // Try to fail at every possible allocation point during apply()
                size_t fail_offset = 0;
                while (true) {
                    BOOST_TEST_MESSAGE(sprint("Failing allocation at %d", fail_offset));
                    alloc.fail_after(fail_offset++);
                    try {
                        m.apply(second);
                        alloc.stop_failing();
                        BOOST_TEST_MESSAGE("Checking that apply has expected result");
                        assert_that(m).is_equal_to(expected_apply_result);
                        break; // we exhausted all allocation points
                    } catch (const std::bad_alloc&) {
                        BOOST_TEST_MESSAGE("Checking that apply was reverted");
                        assert_that(m).is_equal_to(target);
                    }
                }
            }

            // Test the apply(mutation&&) variant
            {
                size_t fail_offset = 0;
                while (true) {
                    auto copy_of_second = second;
                    auto m = target;
                    alloc.fail_after(fail_offset++);
                    try {
                        m.apply(std::move(copy_of_second));
                        alloc.stop_failing();
                        assert_that(m).is_equal_to(expected_apply_result);
                        break; // we exhausted all allocation points
                    } catch (const std::bad_alloc&) {
                        assert_that(m).is_equal_to(target);
                        // they should still commute
                        m.apply(copy_of_second);
                        assert_that(m).is_equal_to(expected_apply_result);
                    }
                }
            }
        }
    });

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_mutation_diff) {
    return seastar::async([] {
        auto my_set_type = set_type_impl::get_instance(int32_type, true);
        auto s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("sc1", bytes_type, column_kind::static_column)
            .with_column("ck", bytes_type, column_kind::clustering_key)
            .with_column("v1", bytes_type, column_kind::regular_column)
            .with_column("v2", bytes_type, column_kind::regular_column)
            .with_column("v3", my_set_type, column_kind::regular_column)
            .build();

        auto ckey1 = clustering_key::from_single_value(*s, bytes_type->decompose(data_value(bytes("A"))));
        auto ckey2 = clustering_key::from_single_value(*s, bytes_type->decompose(data_value(bytes("B"))));

        mutation m1(partition_key::from_single_value(*s, "key1"), s);
        m1.set_static_cell(*s->get_column_definition("sc1"),
            atomic_cell::make_dead(2, gc_clock::now()));

        m1.partition().apply(tombstone { 1, gc_clock::now() });
        m1.set_clustered_cell(ckey1, *s->get_column_definition("v1"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v1:value1")))));
        m1.set_clustered_cell(ckey1, *s->get_column_definition("v2"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v2:value2")))));

        m1.partition().clustered_row(*s, ckey2).apply(row_marker(3));
        m1.set_clustered_cell(ckey2, *s->get_column_definition("v2"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v2:value4")))));
        map_type_impl::mutation mset1 {{}, {{int32_type->decompose(1), make_atomic_cell({})}, {int32_type->decompose(2), make_atomic_cell({})}}};
        m1.set_clustered_cell(ckey2, *s->get_column_definition("v3"),
            my_set_type->serialize_mutation_form(mset1));

        mutation m2(partition_key::from_single_value(*s, "key1"), s);
        m2.set_clustered_cell(ckey1, *s->get_column_definition("v1"),
            atomic_cell::make_live(1, bytes_type->decompose(data_value(bytes("v1:value1a")))));
        m2.set_clustered_cell(ckey1, *s->get_column_definition("v2"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v2:value2")))));

        m2.set_clustered_cell(ckey2, *s->get_column_definition("v1"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v1:value3")))));
        m2.set_clustered_cell(ckey2, *s->get_column_definition("v2"),
            atomic_cell::make_live(3, bytes_type->decompose(data_value(bytes("v2:value4a")))));
        map_type_impl::mutation mset2 {{}, {{int32_type->decompose(1), make_atomic_cell({})}, {int32_type->decompose(3), make_atomic_cell({})}}};
        m2.set_clustered_cell(ckey2, *s->get_column_definition("v3"),
            my_set_type->serialize_mutation_form(mset2));

        mutation m3(partition_key::from_single_value(*s, "key1"), s);
        m3.set_clustered_cell(ckey1, *s->get_column_definition("v1"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v1:value1")))));

        m3.set_clustered_cell(ckey2, *s->get_column_definition("v1"),
            atomic_cell::make_live(2, bytes_type->decompose(data_value(bytes("v1:value3")))));
        m3.set_clustered_cell(ckey2, *s->get_column_definition("v2"),
            atomic_cell::make_live(3, bytes_type->decompose(data_value(bytes("v2:value4a")))));
        map_type_impl::mutation mset3 {{}, {{int32_type->decompose(1), make_atomic_cell({})}}};
        m3.set_clustered_cell(ckey2, *s->get_column_definition("v3"),
            my_set_type->serialize_mutation_form(mset3));

        mutation m12(partition_key::from_single_value(*s, "key1"), s);
        m12.apply(m1);
        m12.apply(m2);

        auto m2_1 = m2.partition().difference(s, m1.partition());
        BOOST_REQUIRE_EQUAL(m2_1.partition_tombstone(), tombstone());
        BOOST_REQUIRE(!m2_1.static_row().size());
        BOOST_REQUIRE(!m2_1.find_row(*s, ckey1));
        BOOST_REQUIRE(m2_1.find_row(*s, ckey2));
        BOOST_REQUIRE(m2_1.find_row(*s, ckey2)->find_cell(2));
        auto cmv = m2_1.find_row(*s, ckey2)->find_cell(2)->as_collection_mutation();
        auto cm = my_set_type->deserialize_mutation_form(cmv);
        BOOST_REQUIRE(cm.cells.size() == 1);
        BOOST_REQUIRE(cm.cells.front().first == int32_type->decompose(3));

        mutation m12_1(partition_key::from_single_value(*s, "key1"), s);
        m12_1.apply(m1);
        m12_1.partition().apply(*s, m2_1, *s);
        BOOST_REQUIRE_EQUAL(m12, m12_1);

        auto m1_2 = m1.partition().difference(s, m2.partition());
        BOOST_REQUIRE_EQUAL(m1_2.partition_tombstone(), m12.partition().partition_tombstone());
        BOOST_REQUIRE(m1_2.find_row(*s, ckey1));
        BOOST_REQUIRE(m1_2.find_row(*s, ckey2));
        BOOST_REQUIRE(!m1_2.find_row(*s, ckey1)->find_cell(1));
        BOOST_REQUIRE(!m1_2.find_row(*s, ckey2)->find_cell(0));
        BOOST_REQUIRE(!m1_2.find_row(*s, ckey2)->find_cell(1));
        cmv = m1_2.find_row(*s, ckey2)->find_cell(2)->as_collection_mutation();
        cm = my_set_type->deserialize_mutation_form(cmv);
        BOOST_REQUIRE(cm.cells.size() == 1);
        BOOST_REQUIRE(cm.cells.front().first == int32_type->decompose(2));

        mutation m12_2(partition_key::from_single_value(*s, "key1"), s);
        m12_2.apply(m2);
        m12_2.partition().apply(*s, m1_2, *s);
        BOOST_REQUIRE_EQUAL(m12, m12_2);

        auto m3_12 = m3.partition().difference(s, m12.partition());
        BOOST_REQUIRE(m3_12.empty());

        auto m12_3 = m12.partition().difference(s, m3.partition());
        BOOST_REQUIRE_EQUAL(m12_3.partition_tombstone(), m12.partition().partition_tombstone());

        mutation m123(partition_key::from_single_value(*s, "key1"), s);
        m123.apply(m3);
        m123.partition().apply(*s, m12_3, *s);
        BOOST_REQUIRE_EQUAL(m12, m123);
    });
}

SEASTAR_TEST_CASE(test_large_blobs) {
    return seastar::async([] {
        auto s = make_lw_shared(schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {}, {}, {{"s1", bytes_type}}, utf8_type));

        auto mt = make_lw_shared<memtable>(s);

        auto blob1 = make_blob(1234567);
        auto blob2 = make_blob(2345678);


        const column_definition& s1_col = *s->get_column_definition("s1");
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});

        mutation m(key, s);
        m.set_static_cell(s1_col, make_atomic_cell(bytes_type->decompose(data_value(blob1))));
        mt->apply(std::move(m));

        auto p = get_partition(*mt, key);
        row& r = p.static_row();
        auto i = r.find_cell(s1_col.id);
        BOOST_REQUIRE(i);
        auto cell = i->as_atomic_cell();
        BOOST_REQUIRE(cell.is_live());
        BOOST_REQUIRE(bytes_type->equal(cell.value(), bytes_type->decompose(data_value(blob1))));

        // Stress managed_bytes::linearize and scatter by merging a value into the cell
        mutation m2(key, s);
        m2.set_static_cell(s1_col, atomic_cell::make_live(7, bytes_type->decompose(data_value(blob2))));
        mt->apply(std::move(m2));

        auto p2 = get_partition(*mt, key);
        row& r2 = p2.static_row();
        auto i2 = r2.find_cell(s1_col.id);
        BOOST_REQUIRE(i2);
        auto cell2 = i2->as_atomic_cell();
        BOOST_REQUIRE(cell2.is_live());
        BOOST_REQUIRE(bytes_type->equal(cell2.value(), bytes_type->decompose(data_value(blob2))));
    });
}

SEASTAR_TEST_CASE(test_mutation_equality) {
    return seastar::async([] {
        for_each_mutation_pair([] (auto&& m1, auto&& m2, are_equal eq) {
            if (eq) {
                assert_that(m1).is_equal_to(m2);
            } else {
                assert_that(m1).is_not_equal_to(m2);
            }
        });
    });
}

SEASTAR_TEST_CASE(test_mutation_hash) {
    return seastar::async([] {
        for_each_mutation_pair([] (auto&& m1, auto&& m2, are_equal eq) {
            auto get_hash = [] (const mutation& m) {
                md5_hasher h;
                feed_hash(h, m);
                return h.finalize();
            };
            auto h1 = get_hash(m1);
            auto h2 = get_hash(m2);
            if (eq) {
                if (h1 != h2) {
                    BOOST_FAIL(sprint("Hash should be equal for %s and %s", m1, m2));
                }
            } else {
                // We're using a strong hasher, collision should be unlikely
                if (h1 == h2) {
                    BOOST_FAIL(sprint("Hash should be different for %s and %s", m1, m2));
                }
            }
        });
    });
}

static mutation compacted(const mutation& m) {
    auto result = m;
    result.partition().compact_for_compaction(*result.schema(), always_gc, gc_clock::now());
    return result;
}

SEASTAR_TEST_CASE(test_query_digest) {
    return seastar::async([] {
        auto check_digests_equal = [] (const mutation& m1, const mutation& m2) {
            auto ps1 = partition_slice_builder(*m1.schema()).build();
            auto ps2 = partition_slice_builder(*m2.schema()).build();
            auto digest1 = *m1.query(ps1, query::result_request::only_digest).digest();
            auto digest2 = *m2.query(ps2, query::result_request::only_digest).digest();
            if (digest1 != digest2) {
                BOOST_FAIL(sprint("Digest should be the same for %s and %s", m1, m2));
            }
        };

        for_each_mutation_pair([&] (const mutation& m1, const mutation& m2, are_equal eq) {
            if (m1.schema()->version() != m2.schema()->version()) {
                return;
            }

            if (eq) {
                check_digests_equal(compacted(m1), m2);
                check_digests_equal(m1, compacted(m2));
            } else {
                BOOST_TEST_MESSAGE("If not equal, they should become so after applying diffs mutually");

                schema_ptr s = m1.schema();

                auto m3 = m2;
                {
                    auto diff = m1.partition().difference(s, m2.partition());
                    m3.partition().apply(*m3.schema(), std::move(diff));
                }

                auto m4 = m1;
                {
                    auto diff = m2.partition().difference(s, m1.partition());
                    m4.partition().apply(*m4.schema(), std::move(diff));
                }

                check_digests_equal(m3, m4);
            }
        });
    });
}

SEASTAR_TEST_CASE(test_mutation_upgrade_of_equal_mutations) {
    return seastar::async([] {
        for_each_mutation_pair([](auto&& m1, auto&& m2, are_equal eq) {
            if (eq == are_equal::yes) {
                assert_that(m1).is_upgrade_equivalent(m2.schema());
                assert_that(m2).is_upgrade_equivalent(m1.schema());
            }
        });
    });
}

SEASTAR_TEST_CASE(test_mutation_upgrade) {
    return seastar::async([] {
        auto make_builder = [] {
            return schema_builder("ks", "cf")
                    .with_column("pk", bytes_type, column_kind::partition_key)
                    .with_column("ck", bytes_type, column_kind::clustering_key);
        };

        auto s = make_builder()
                .with_column("sc1", bytes_type, column_kind::static_column)
                .with_column("v1", bytes_type, column_kind::regular_column)
                .with_column("v2", bytes_type, column_kind::regular_column)
                .build();

        auto pk = partition_key::from_singular(*s, data_value(bytes("key1")));
        auto ckey1 = clustering_key::from_singular(*s, data_value(bytes("A")));

        {
            mutation m(pk, s);
            m.set_clustered_cell(ckey1, "v2", data_value(bytes("v2:value")), 1);

            assert_that(m).is_upgrade_equivalent(
                    make_builder() // without v1
                            .with_column("sc1", bytes_type, column_kind::static_column)
                            .with_column("v2", bytes_type, column_kind::regular_column)
                            .build());

            assert_that(m).is_upgrade_equivalent(
                    make_builder() // without sc1
                            .with_column("v1", bytes_type, column_kind::static_column)
                            .with_column("v2", bytes_type, column_kind::regular_column)
                            .build());

            assert_that(m).is_upgrade_equivalent(
                    make_builder() // with v1 recreated as static
                            .with_column("sc1", bytes_type, column_kind::static_column)
                            .with_column("v1", bytes_type, column_kind::static_column)
                            .with_column("v2", bytes_type, column_kind::regular_column)
                            .build());

            assert_that(m).is_upgrade_equivalent(
                    make_builder() // with new column inserted before v1
                            .with_column("sc1", bytes_type, column_kind::static_column)
                            .with_column("v0", bytes_type, column_kind::regular_column)
                            .with_column("v1", bytes_type, column_kind::regular_column)
                            .with_column("v2", bytes_type, column_kind::regular_column)
                            .build());

            assert_that(m).is_upgrade_equivalent(
                    make_builder() // with new column inserted after v2
                            .with_column("sc1", bytes_type, column_kind::static_column)
                            .with_column("v0", bytes_type, column_kind::regular_column)
                            .with_column("v2", bytes_type, column_kind::regular_column)
                            .with_column("v3", bytes_type, column_kind::regular_column)
                            .build());
        }

        {
            mutation m(pk, s);
            m.set_clustered_cell(ckey1, "v1", data_value(bytes("v2:value")), 1);
            m.set_clustered_cell(ckey1, "v2", data_value(bytes("v2:value")), 1);

            auto s2 = make_builder() // v2 changed into a static column, v1 removed
                    .with_column("v2", bytes_type, column_kind::static_column)
                    .build();

            m.upgrade(s2);

            mutation m2(pk, s2);
            m2.partition().clustered_row(*s2, ckey1);
            assert_that(m).is_equal_to(m2);
        }

        {
            mutation m(pk, make_builder()
                    .with_column("v1", bytes_type, column_kind::regular_column)
                    .with_column("v2", bytes_type, column_kind::regular_column)
                    .with_column("v3", bytes_type, column_kind::regular_column)
                    .build());
            m.set_clustered_cell(ckey1, "v1", data_value(bytes("v1:value")), 1);
            m.set_clustered_cell(ckey1, "v2", data_value(bytes("v2:value")), 1);
            m.set_clustered_cell(ckey1, "v3", data_value(bytes("v3:value")), 1);

            auto s2 = make_builder() // v2 changed into a static column
                    .with_column("v1", bytes_type, column_kind::regular_column)
                    .with_column("v2", bytes_type, column_kind::static_column)
                    .with_column("v3", bytes_type, column_kind::regular_column)
                    .build();

            m.upgrade(s2);

            mutation m2(pk, s2);
            m2.set_clustered_cell(ckey1, "v1", data_value(bytes("v1:value")), 1);
            m2.set_clustered_cell(ckey1, "v3", data_value(bytes("v3:value")), 1);

            assert_that(m).is_equal_to(m2);
        }
    });
}

SEASTAR_TEST_CASE(test_querying_expired_cells) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
                .with_column("pk", bytes_type, column_kind::partition_key)
                .with_column("ck", bytes_type, column_kind::clustering_key)
                .with_column("s1", bytes_type, column_kind::static_column)
                .with_column("s2", bytes_type, column_kind::static_column)
                .with_column("s3", bytes_type, column_kind::static_column)
                .with_column("v1", bytes_type)
                .with_column("v2", bytes_type)
                .with_column("v3", bytes_type)
                .build();

        auto pk = partition_key::from_singular(*s, data_value(bytes("key1")));
        auto ckey1 = clustering_key::from_singular(*s, data_value(bytes("A")));

        auto ttl = std::chrono::seconds(1);
        auto t1 = gc_clock::now();
        auto t2 = t1 + std::chrono::seconds(1);
        auto t3 = t2 + std::chrono::seconds(1);
        auto t4 = t3 + std::chrono::seconds(1);

        auto v1 = data_value(bytes("1"));
        auto v2 = data_value(bytes("2"));
        auto v3 = data_value(bytes("3"));

        auto results_at_time = [s] (const mutation& m, gc_clock::time_point t) {
            auto slice = partition_slice_builder(*s)
                    .with_regular_column("v1")
                    .with_regular_column("v2")
                    .with_regular_column("v3")
                    .with_static_column("s1")
                    .with_static_column("s2")
                    .with_static_column("s3")
                    .without_clustering_key_columns()
                    .without_partition_key_columns()
                    .build();
            return query::result_set::from_raw_result(s, slice, m.query(slice, query::result_request::result_and_digest, t));
        };

        {
            mutation m(pk, s);
            m.set_clustered_cell(ckey1, *s->get_column_definition("v1"), atomic_cell::make_live(api::new_timestamp(), v1.serialize(), t1, ttl));
            m.set_clustered_cell(ckey1, *s->get_column_definition("v2"), atomic_cell::make_live(api::new_timestamp(), v2.serialize(), t2, ttl));
            m.set_clustered_cell(ckey1, *s->get_column_definition("v3"), atomic_cell::make_live(api::new_timestamp(), v3.serialize(), t3, ttl));
            m.set_static_cell(*s->get_column_definition("s1"), atomic_cell::make_live(api::new_timestamp(), v1.serialize(), t1, ttl));
            m.set_static_cell(*s->get_column_definition("s2"), atomic_cell::make_live(api::new_timestamp(), v2.serialize(), t2, ttl));
            m.set_static_cell(*s->get_column_definition("s3"), atomic_cell::make_live(api::new_timestamp(), v3.serialize(), t3, ttl));

            assert_that(results_at_time(m, t1))
                    .has_only(a_row()
                         .with_column("s1", v1)
                         .with_column("s2", v2)
                         .with_column("s3", v3)
                         .with_column("v1", v1)
                         .with_column("v2", v2)
                         .with_column("v3", v3)
                         .and_only_that());

            assert_that(results_at_time(m, t2))
                    .has_only(a_row()
                         .with_column("s2", v2)
                         .with_column("s3", v3)
                         .with_column("v2", v2)
                         .with_column("v3", v3)
                         .and_only_that());

            assert_that(results_at_time(m, t3))
                    .has_only(a_row()
                         .with_column("s3", v3)
                         .with_column("v3", v3)
                         .and_only_that());

            assert_that(results_at_time(m, t4)).is_empty();
        }

        {
            mutation m(pk, s);
            m.set_clustered_cell(ckey1, *s->get_column_definition("v1"), atomic_cell::make_live(api::new_timestamp(), v1.serialize(), t1, ttl));
            m.set_static_cell(*s->get_column_definition("s1"), atomic_cell::make_live(api::new_timestamp(), v1.serialize(), t3, ttl));

            assert_that(results_at_time(m, t2))
                    .has_only(a_row().with_column("s1", v1).and_only_that());

            assert_that(results_at_time(m, t4)).is_empty();
        }
    });
}

SEASTAR_TEST_CASE(test_tombstone_purge) {
    auto builder = schema_builder("tests", "tombstone_purge")
        .with_column("id", utf8_type, column_kind::partition_key)
        .with_column("value", int32_type);
    builder.set_gc_grace_seconds(0);
    auto s = builder.build();

    auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
    const column_definition& col = *s->get_column_definition("value");

    mutation m(key, s);
    m.set_clustered_cell(clustering_key::make_empty(), col, make_atomic_cell(int32_type->decompose(1)));
    tombstone tomb(api::new_timestamp(), gc_clock::now() - std::chrono::seconds(1));
    m.partition().apply(tomb);
    BOOST_REQUIRE(!m.partition().empty());
    m.partition().compact_for_compaction(*s, always_gc, gc_clock::now());
    // Check that row was covered by tombstone.
    BOOST_REQUIRE(m.partition().empty());
    // Check that tombstone was purged after compact_for_compaction().
    BOOST_REQUIRE(!m.partition().partition_tombstone());

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_slicing_mutation) {
    auto s = schema_builder("ks", "cf")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck", int32_type, column_kind::clustering_key)
        .with_column("v", int32_type)
        .build();

    auto pk = partition_key::from_exploded(*s, { int32_type->decompose(0) });
    mutation m(pk, s);
    constexpr auto row_count = 8;
    for (auto i = 0; i < row_count; i++) {
        m.set_clustered_cell(clustering_key_prefix::from_single_value(*s, int32_type->decompose(i)),
                             to_bytes("v"), data_value(i), api::new_timestamp());
    }

    auto verify_rows = [&] (mutation_partition& mp, std::vector<int> rows) {
        std::deque<clustering_key> cks;
        for (auto&& cr : rows) {
            cks.emplace_back(clustering_key_prefix::from_single_value(*s, int32_type->decompose(cr)));
        }
        clustering_key::equality ck_eq(*s);
        for (auto&& cr : mp.clustered_rows()) {
            BOOST_REQUIRE(ck_eq(cr.key(), cks.front()));
            cks.pop_front();
        }
    };

    auto test_slicing = [&] (query::clustering_row_ranges ranges, std::vector<int> expected_rows) {
        mutation_partition mp1(m.partition(), *s, ranges);
        auto mp_temp = m.partition();
        mutation_partition mp2(std::move(mp_temp), *s, ranges);

        BOOST_REQUIRE(mp1.equal(*s, mp2));
        verify_rows(mp1, expected_rows);
    };

    test_slicing(query::clustering_row_ranges {
            query::clustering_range {
                { },
                query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(2)), false },
            },
            clustering_key_prefix::from_single_value(*s, int32_type->decompose(5)),
            query::clustering_range {
                query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(7)) },
                query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(10)) },
            },
        },
        std::vector<int> { 0, 1, 5, 7 });

    test_slicing(query::clustering_row_ranges {
            query::clustering_range {
                query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(1)) },
                query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(2)) },
            },
            query::clustering_range {
                query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(4)), false },
                query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(6)) },
            },
            query::clustering_range {
                query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(7)), false },
                { },
            },
        },
        std::vector<int> { 1, 2, 5, 6 });

    test_slicing(query::clustering_row_ranges {
            query::clustering_range {
                { },
                { },
            },
        },
        std::vector<int> { 0, 1, 2, 3, 4, 5, 6, 7 });

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_trim_rows) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
                .with_column("pk", int32_type, column_kind::partition_key)
                .with_column("ck", int32_type, column_kind::clustering_key)
                .with_column("v", int32_type)
                .build();

        auto pk = partition_key::from_exploded(*s, { int32_type->decompose(0) });
        mutation m(pk, s);
        constexpr auto row_count = 8;
        for (auto i = 0; i < row_count; i++) {
            m.set_clustered_cell(clustering_key_prefix::from_single_value(*s, int32_type->decompose(i)),
                                 to_bytes("v"), data_value(i), api::new_timestamp() - 5);
        }
        m.partition().apply(tombstone(api::new_timestamp(), gc_clock::now()));

        auto now = gc_clock::now() + gc_clock::duration(std::chrono::hours(1));

        auto compact_and_expect_empty = [&] (mutation m, std::vector<query::clustering_range> ranges) {
            mutation m2 = m;
            m.partition().compact_for_query(*s, now, ranges, false, query::max_rows);
            BOOST_REQUIRE(m.partition().clustered_rows().empty());

            std::reverse(ranges.begin(), ranges.end());
            m2.partition().compact_for_query(*s, now, ranges, true, query::max_rows);
            BOOST_REQUIRE(m2.partition().clustered_rows().empty());
        };

        std::vector<query::clustering_range> ranges = {
                query::clustering_range::make_starting_with(clustering_key_prefix::from_single_value(*s, int32_type->decompose(5)))
        };
        compact_and_expect_empty(m, ranges);

        ranges = {
            query::clustering_range::make_starting_with(clustering_key_prefix::from_single_value(*s, int32_type->decompose(50)))
        };
        compact_and_expect_empty(m, ranges);

        ranges = {
            query::clustering_range::make_ending_with(clustering_key_prefix::from_single_value(*s, int32_type->decompose(5)))
        };
        compact_and_expect_empty(m, ranges);

        ranges = {
            query::clustering_range::make_open_ended_both_sides()
        };
        compact_and_expect_empty(m, ranges);
    });
}
