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


#include <boost/test/unit_test.hpp>
#include <boost/intrusive/parent_from_member.hpp>
#include <algorithm>
#include <chrono>

#include <seastar/core/thread.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/sleep.hh>
#include <seastar/tests/test-utils.hh>
#include <seastar/util/defer.hh>
#include <deque>
#include "utils/phased_barrier.hh"

#include "utils/logalloc.hh"
#include "utils/managed_ref.hh"
#include "utils/managed_bytes.hh"
#include "log.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

[[gnu::unused]]
static auto x = [] {
    logging::logger_registry().set_all_loggers_level(logging::log_level::debug);
    return 0;
}();

using namespace logalloc;

SEASTAR_TEST_CASE(test_compaction) {
    return seastar::async([] {
        region reg;

        with_allocator(reg.allocator(), [&reg] {
            std::vector<managed_ref<int>> _allocated;

            // Allocate several segments

            auto reclaim_counter_1 = reg.reclaim_counter();

            for (int i = 0; i < 32 * 1024 * 4; i++) {
                _allocated.push_back(make_managed<int>());
            }

            // Allocation should not invalidate references
            BOOST_REQUIRE_EQUAL(reg.reclaim_counter(), reclaim_counter_1);

            shard_tracker().reclaim_all_free_segments();

            // Free 1/3 randomly

            std::random_shuffle(_allocated.begin(), _allocated.end());

            auto it = _allocated.begin();
            size_t nr_freed = _allocated.size() / 3;
            for (size_t i = 0; i < nr_freed; ++i) {
                *it++ = {};
            }

            // Freeing should not invalidate references
            BOOST_REQUIRE_EQUAL(reg.reclaim_counter(), reclaim_counter_1);

            // Try to reclaim

            size_t target = sizeof(managed<int>) * nr_freed;
            BOOST_REQUIRE(shard_tracker().reclaim(target) >= target);

            // There must have been some compaction during such reclaim
            BOOST_REQUIRE(reg.reclaim_counter() != reclaim_counter_1);
        });
    });
}


SEASTAR_TEST_CASE(test_compaction_with_multiple_regions) {
    return seastar::async([] {
        region reg1;
        region reg2;

        std::vector<managed_ref<int>> allocated1;
        std::vector<managed_ref<int>> allocated2;

        int count = 32 * 1024 * 4;
        
        with_allocator(reg1.allocator(), [&] {
            for (int i = 0; i < count; i++) {
                allocated1.push_back(make_managed<int>());
            }
        });

        with_allocator(reg2.allocator(), [&] {
            for (int i = 0; i < count; i++) {
                allocated2.push_back(make_managed<int>());
            }
        });

        size_t quarter = shard_tracker().region_occupancy().total_space() / 4;

        shard_tracker().reclaim_all_free_segments();

        // Can't reclaim anything yet
        BOOST_REQUIRE(shard_tracker().reclaim(quarter) == 0);
        
        // Free 60% from the second pool

        // Shuffle, so that we don't free whole segments back to the pool
        // and there's nothing to reclaim.
        std::random_shuffle(allocated2.begin(), allocated2.end());

        with_allocator(reg2.allocator(), [&] {
            auto it = allocated2.begin();
            for (size_t i = 0; i < (count * 0.6); ++i) {
                *it++ = {};
            }
        });

        BOOST_REQUIRE(shard_tracker().reclaim(quarter) >= quarter);
        BOOST_REQUIRE(shard_tracker().reclaim(quarter) < quarter);

        // Free 60% from the first pool

        std::random_shuffle(allocated1.begin(), allocated1.end());

        with_allocator(reg1.allocator(), [&] {
            auto it = allocated1.begin();
            for (size_t i = 0; i < (count * 0.6); ++i) {
                *it++ = {};
            }
        });

        BOOST_REQUIRE(shard_tracker().reclaim(quarter) >= quarter);
        BOOST_REQUIRE(shard_tracker().reclaim(quarter) < quarter);

        with_allocator(reg2.allocator(), [&] () mutable {
            allocated2.clear();
        });

        with_allocator(reg1.allocator(), [&] () mutable {
            allocated1.clear();
        });
    });
}

SEASTAR_TEST_CASE(test_mixed_type_compaction) {
    return seastar::async([] {
        static bool a_moved = false;
        static bool b_moved = false;
        static bool c_moved = false;

        static bool a_destroyed = false;
        static bool b_destroyed = false;
        static bool c_destroyed = false;

        struct A {
            uint8_t v = 0xca;
            A() = default;
            A(A&&) noexcept {
                a_moved = true;
            }
            ~A() {
                BOOST_REQUIRE(v == 0xca);
                a_destroyed = true;
            }
        };
        struct B {
            uint16_t v = 0xcafe;
            B() = default;
            B(B&&) noexcept {
                b_moved = true;
            }
            ~B() {
                BOOST_REQUIRE(v == 0xcafe);
                b_destroyed = true;
            }
        };
        struct C {
            uint64_t v = 0xcafebabe;
            C() = default;
            C(C&&) noexcept {
                c_moved = true;
            }
            ~C() {
                BOOST_REQUIRE(v == 0xcafebabe);
                c_destroyed = true;
            }
        };

        region reg;
        with_allocator(reg.allocator(), [&] {
            {
                std::vector<int*> objs;

                auto p1 = make_managed<A>();

                int junk_count = 10;

                for (int i = 0; i < junk_count; i++) {
                    objs.push_back(reg.allocator().construct<int>(i));
                }

                auto p2 = make_managed<B>();

                for (int i = 0; i < junk_count; i++) {
                    objs.push_back(reg.allocator().construct<int>(i));
                }

                auto p3 = make_managed<C>();

                for (auto&& p : objs) {
                    reg.allocator().destroy(p);
                }

                reg.full_compaction();

                BOOST_REQUIRE(a_moved);
                BOOST_REQUIRE(b_moved);
                BOOST_REQUIRE(c_moved);

                BOOST_REQUIRE(a_destroyed);
                BOOST_REQUIRE(b_destroyed);
                BOOST_REQUIRE(c_destroyed);

                a_destroyed = false;
                b_destroyed = false;
                c_destroyed = false;
            }

            BOOST_REQUIRE(a_destroyed);
            BOOST_REQUIRE(b_destroyed);
            BOOST_REQUIRE(c_destroyed);
        });
    });
}

SEASTAR_TEST_CASE(test_blob) {
    return seastar::async([] {
        region reg;
        with_allocator(reg.allocator(), [&] {
            auto src = bytes("123456");
            managed_bytes b(src);

            BOOST_REQUIRE(bytes_view(b) == src);

            reg.full_compaction();

            BOOST_REQUIRE(bytes_view(b) == src);
        });
    });
}

SEASTAR_TEST_CASE(test_merging) {
    return seastar::async([] {
        region reg1;
        region reg2;

        reg1.merge(reg2);

        managed_ref<int> r1;

        with_allocator(reg1.allocator(), [&] {
            r1 = make_managed<int>();
        });

        reg2.merge(reg1);

        with_allocator(reg2.allocator(), [&] {
            r1 = {};
        });

        std::vector<managed_ref<int>> refs;

        with_allocator(reg1.allocator(), [&] {
            for (int i = 0; i < 10000; ++i) {
                refs.emplace_back(make_managed<int>());
            }
        });

        reg2.merge(reg1);

        with_allocator(reg2.allocator(), [&] {
            refs.clear();
        });
    });
}

#ifndef DEFAULT_ALLOCATOR
SEASTAR_TEST_CASE(test_region_lock) {
    return seastar::async([] {
        region reg;
        with_allocator(reg.allocator(), [&] {
            std::deque<managed_bytes> refs;

            for (int i = 0; i < 1024 * 10; ++i) {
                refs.push_back(managed_bytes(managed_bytes::initialized_later(), 1024));
            }

            // Evict 30% so that region is compactible, but do it randomly so that
            // segments are not released into the standard allocator without compaction.
            std::random_shuffle(refs.begin(), refs.end());
            for (size_t i = 0; i < refs.size() * 0.3; ++i) {
                refs.pop_back();
            }

            reg.make_evictable([&refs] {
                if (refs.empty()) {
                    return memory::reclaiming_result::reclaimed_nothing;
                }
                refs.pop_back();
                return memory::reclaiming_result::reclaimed_something;
            });

            std::deque<bytes> objects;

            auto counter = reg.reclaim_counter();

            // Verify that with compaction lock we rather run out of memory
            // than compact it
            {
                BOOST_REQUIRE(reg.reclaiming_enabled());

                logalloc::reclaim_lock _(reg);

                BOOST_REQUIRE(!reg.reclaiming_enabled());
                auto used_before = reg.occupancy().used_space();

                try {
                    while (true) {
                        objects.push_back(bytes(bytes::initialized_later(), 1024*1024));
                    }
                } catch (const std::bad_alloc&) {
                    // expected
                }

                BOOST_REQUIRE(reg.reclaim_counter() == counter);
                BOOST_REQUIRE(reg.occupancy().used_space() == used_before); // eviction is also disabled
            }

            BOOST_REQUIRE(reg.reclaiming_enabled());
        });
    });
}

SEASTAR_TEST_CASE(test_large_allocation) {
    return seastar::async([] {
        logalloc::region r_evictable;
        logalloc::region r_non_evictable;

        static constexpr unsigned element_size = 16 * 1024;

        std::deque<managed_bytes> evictable;
        std::deque<managed_bytes> non_evictable;
        try {
            while (true) {
                with_allocator(r_evictable.allocator(), [&] {
                    evictable.push_back(bytes(bytes::initialized_later(),element_size));
                });
                with_allocator(r_non_evictable.allocator(), [&] {
                    non_evictable.push_back(bytes(bytes::initialized_later(),element_size));
                });
            }
        } catch (const std::bad_alloc&) {
            // expected
        }

        std::random_shuffle(evictable.begin(), evictable.end());
        r_evictable.make_evictable([&] {
            return with_allocator(r_evictable.allocator(), [&] {
                if (evictable.empty()) {
                    return memory::reclaiming_result::reclaimed_nothing;
                }
                evictable.pop_front();
                return memory::reclaiming_result::reclaimed_something;
            });
        });

        auto clear_all = [&] {
            with_allocator(r_non_evictable.allocator(), [&] {
                non_evictable.clear();
            });
            with_allocator(r_evictable.allocator(), [&] {
                evictable.clear();
            });
        };

        try {
            auto ptr = std::make_unique<char[]>(evictable.size() * element_size / 4 * 3);
        } catch (const std::bad_alloc&) {
            // This shouldn't have happened, but clear remaining lsa data
            // properly so that humans see bad_alloc instead of some confusing
            // assertion failure caused by destroying evictable and
            // non_evictable without with_allocator().
            clear_all();
            throw;
        }

        clear_all();
    });
}
#endif

SEASTAR_TEST_CASE(test_region_groups) {
    return seastar::async([] {
        logalloc::region_group just_four;
        logalloc::region_group all;
        logalloc::region_group one_and_two(&all);

        auto one = std::make_unique<logalloc::region>(one_and_two);
        auto two = std::make_unique<logalloc::region>(one_and_two);
        auto three = std::make_unique<logalloc::region>(all);
        auto four = std::make_unique<logalloc::region>(just_four);
        auto five = std::make_unique<logalloc::region>();

        constexpr size_t one_count = 1024 * 1024;
        std::vector<managed_ref<int>> one_objs;
        with_allocator(one->allocator(), [&] {
            for (size_t i = 0; i < one_count; i++) {
                one_objs.emplace_back(make_managed<int>());
            }
        });
        BOOST_REQUIRE_GE(ssize_t(one->occupancy().used_space()), ssize_t(one_count * sizeof(int)));
        BOOST_REQUIRE_GE(ssize_t(one->occupancy().total_space()), ssize_t(one->occupancy().used_space()));
        BOOST_REQUIRE_EQUAL(one_and_two.memory_used(), one->occupancy().total_space());
        BOOST_REQUIRE_EQUAL(all.memory_used(), one->occupancy().total_space());

        constexpr size_t two_count = 512 * 1024;
        std::vector<managed_ref<int>> two_objs;
        with_allocator(two->allocator(), [&] {
            for (size_t i = 0; i < two_count; i++) {
                two_objs.emplace_back(make_managed<int>());
            }
        });
        BOOST_REQUIRE_GE(ssize_t(two->occupancy().used_space()), ssize_t(two_count * sizeof(int)));
        BOOST_REQUIRE_GE(ssize_t(two->occupancy().total_space()), ssize_t(two->occupancy().used_space()));
        BOOST_REQUIRE_EQUAL(one_and_two.memory_used(), one->occupancy().total_space() + two->occupancy().total_space());
        BOOST_REQUIRE_EQUAL(all.memory_used(), one_and_two.memory_used());

        constexpr size_t three_count = 2048 * 1024;
        std::vector<managed_ref<int>> three_objs;
        with_allocator(three->allocator(), [&] {
            for (size_t i = 0; i < three_count; i++) {
                three_objs.emplace_back(make_managed<int>());
            }
        });
        BOOST_REQUIRE_GE(ssize_t(three->occupancy().used_space()), ssize_t(three_count * sizeof(int)));
        BOOST_REQUIRE_GE(ssize_t(three->occupancy().total_space()), ssize_t(three->occupancy().used_space()));
        BOOST_REQUIRE_EQUAL(all.memory_used(), one_and_two.memory_used() + three->occupancy().total_space());

        constexpr size_t four_count = 256 * 1024;
        std::vector<managed_ref<int>> four_objs;
        with_allocator(four->allocator(), [&] {
            for (size_t i = 0; i < four_count; i++) {
                four_objs.emplace_back(make_managed<int>());
            }
        });
        BOOST_REQUIRE_GE(ssize_t(four->occupancy().used_space()), ssize_t(four_count * sizeof(int)));
        BOOST_REQUIRE_GE(ssize_t(four->occupancy().total_space()), ssize_t(four->occupancy().used_space()));
        BOOST_REQUIRE_EQUAL(just_four.memory_used(), four->occupancy().total_space());

        with_allocator(five->allocator(), [] {
            std::vector<managed_ref<int>> five_objs;
            for (size_t i = 0; i < 16 * 1024; i++) {
                five_objs.emplace_back(make_managed<int>());
            }
        });

        three->merge(*four);
        BOOST_REQUIRE_GE(ssize_t(three->occupancy().used_space()), ssize_t((three_count  + four_count)* sizeof(int)));
        BOOST_REQUIRE_GE(ssize_t(three->occupancy().total_space()), ssize_t(three->occupancy().used_space()));
        BOOST_REQUIRE_EQUAL(all.memory_used(), one_and_two.memory_used() + three->occupancy().total_space());
        BOOST_REQUIRE_EQUAL(just_four.memory_used(), 0);

        three->merge(*five);
        BOOST_REQUIRE_GE(ssize_t(three->occupancy().used_space()), ssize_t((three_count  + four_count)* sizeof(int)));
        BOOST_REQUIRE_GE(ssize_t(three->occupancy().total_space()), ssize_t(three->occupancy().used_space()));
        BOOST_REQUIRE_EQUAL(all.memory_used(), one_and_two.memory_used() + three->occupancy().total_space());

        with_allocator(two->allocator(), [&] {
            two_objs.clear();
        });
        two.reset();
        BOOST_REQUIRE_EQUAL(one_and_two.memory_used(), one->occupancy().total_space());
        BOOST_REQUIRE_EQUAL(all.memory_used(), one_and_two.memory_used() + three->occupancy().total_space());

        with_allocator(one->allocator(), [&] {
            one_objs.clear();
        });
        one.reset();
        BOOST_REQUIRE_EQUAL(one_and_two.memory_used(), 0);
        BOOST_REQUIRE_EQUAL(all.memory_used(), three->occupancy().total_space());

        with_allocator(three->allocator(), [&] {
            three_objs.clear();
            four_objs.clear();
        });
        three.reset();
        four.reset();
        five.reset();
        BOOST_REQUIRE_EQUAL(all.memory_used(), 0);
    });
}

using namespace std::chrono_literals;

template <typename FutureType>
inline void quiesce(FutureType&& fut) {
    // Unfortunately seastar::thread::yield is not enough here, because the process of releasing
    // a request may be broken into many continuations. While we could just yield many times, the
    // exact amount needed to guarantee execution would be dependent on the internals of the
    // implementation, we want to avoid that.
    with_timeout(lowres_clock::now() + 2s, std::move(fut)).get();
}

// Simple RAII structure that wraps around a region_group
// Not using defer because we usually employ many region groups
struct test_region_group: public logalloc::region_group {
    test_region_group(region_group* parent, region_group_reclaimer& reclaimer) : logalloc::region_group(parent, reclaimer) {}
    test_region_group(region_group_reclaimer& reclaimer) : logalloc::region_group(nullptr, reclaimer) {}

    ~test_region_group() {
        shutdown().get();
    }
};

struct test_region: public logalloc::region  {
    test_region(test_region_group& rg) : logalloc::region(rg) {}
    ~test_region() {
        clear();
    }

    void clear() {
        with_allocator(allocator(), [this] {
            std::vector<managed_bytes>().swap(_alloc);
            std::vector<managed_ref<uint64_t>>().swap(_alloc_simple);
        });
    }
    void alloc(size_t size = logalloc::segment_size) {
        with_allocator(allocator(), [this, size] {
            _alloc.push_back(bytes(bytes::initialized_later(), size));
        });
    }

    void alloc_small(size_t nr = 1) {
        with_allocator(allocator(), [this] {
            _alloc_simple.emplace_back(make_managed<uint64_t>());
        });
    }
private:
    std::vector<managed_bytes> _alloc;
    // For small objects we don't want to get caught in basic_sstring's internal buffer. We know
    // which size we need to allocate to avoid that, but that's technically internal representation.
    // Better to use integers if we want something small.
    std::vector<managed_ref<uint64_t>> _alloc_simple;
};

SEASTAR_TEST_CASE(test_region_groups_basic_throttling) {
    return seastar::async([] {
        region_group_reclaimer simple_reclaimer(logalloc::segment_size);

        // singleton hierarchy, only one segment allowed
        test_region_group simple(simple_reclaimer);
        auto simple_region = std::make_unique<test_region>(simple);

        // Expectation: after first allocation region will have one segment,
        // memory_used() == throttle_threshold and we are good to go, future
        // is ready immediately.
        //
        // The allocation of the first element won't change the memory usage inside
        // the group and we'll be okay to do that a second time.
        auto fut = simple.run_when_memory_available([&simple_region] { simple_region->alloc_small(); });
        BOOST_REQUIRE_EQUAL(fut.available(), true);
        BOOST_REQUIRE_EQUAL(simple.memory_used(), logalloc::segment_size);

        fut = simple.run_when_memory_available([&simple_region] { simple_region->alloc_small(); });
        BOOST_REQUIRE_EQUAL(fut.available(), true);
        BOOST_REQUIRE_EQUAL(simple.memory_used(), logalloc::segment_size);

        auto big_region = std::make_unique<test_region>(simple);
        // Allocate a big chunk, that will certainly get us over the threshold
        big_region->alloc();

        // We should not be permitted to go forward with a new allocation now...
        fut = simple.run_when_memory_available([&simple_region] { simple_region->alloc_small(); });
        BOOST_REQUIRE_EQUAL(fut.available(), false);
        BOOST_REQUIRE_GT(simple.memory_used(), logalloc::segment_size);

        // But when we remove the big bytes allocator from the region, then we should.
        // Internally, we can't guarantee that just freeing the object will give the segment back,
        // that's up to the internal policies. So to make sure we need to remove the whole region.
        big_region.reset();

        quiesce(std::move(fut));
    });
}

SEASTAR_TEST_CASE(test_region_groups_linear_hierarchy_throttling_child_alloc) {
    return seastar::async([] {
        region_group_reclaimer parent_reclaimer(2 * logalloc::segment_size);
        region_group_reclaimer child_reclaimer(logalloc::segment_size);

        test_region_group parent(parent_reclaimer);
        test_region_group child(&parent, child_reclaimer);

        auto child_region = std::make_unique<test_region>(child);
        auto parent_region = std::make_unique<test_region>(parent);

        child_region->alloc();
        BOOST_REQUIRE_GE(parent.memory_used(), logalloc::segment_size);

        auto fut = parent.run_when_memory_available([&parent_region] { parent_region->alloc_small(); });
        BOOST_REQUIRE_EQUAL(fut.available(), true);
        BOOST_REQUIRE_GE(parent.memory_used(), 2 * logalloc::segment_size);

        // This time child will use all parent's memory. Note that because the child's memory limit
        // is lower than the parent's, for that to happen we need to allocate directly.
        child_region->alloc();
        BOOST_REQUIRE_GE(child.memory_used(), 2 * logalloc::segment_size);

        fut = parent.run_when_memory_available([&parent_region] { parent_region->alloc_small(); });
        BOOST_REQUIRE_EQUAL(fut.available(), false);
        BOOST_REQUIRE_GE(parent.memory_used(), 2 * logalloc::segment_size);

        child_region.reset();
        quiesce(std::move(fut));
    });
}

SEASTAR_TEST_CASE(test_region_groups_linear_hierarchy_throttling_parent_alloc) {
    return seastar::async([] {
        region_group_reclaimer simple_reclaimer(logalloc::segment_size);

        test_region_group parent(simple_reclaimer);
        test_region_group child(&parent, simple_reclaimer);

        auto parent_region = std::make_unique<test_region>(parent);

        parent_region->alloc();
        BOOST_REQUIRE_GE(parent.memory_used(), logalloc::segment_size);

        auto fut = child.run_when_memory_available([] {});
        BOOST_REQUIRE_EQUAL(fut.available(), false);

        parent_region.reset();
        quiesce(std::move(fut));
    });
}

SEASTAR_TEST_CASE(test_region_groups_fifo_order) {
    // tests that requests that are queued for later execution execute in FIFO order
    return seastar::async([] {
        region_group_reclaimer simple_reclaimer(logalloc::segment_size);

        test_region_group rg(simple_reclaimer);

        auto region = std::make_unique<test_region>(rg);

        // fill the parent. Try allocating at child level. Should not be allowed.
        region->alloc();
        BOOST_REQUIRE_GE(rg.memory_used(), logalloc::segment_size);

        auto exec_cnt = make_lw_shared<int>(0);
        std::vector<future<>> executions;

        for (auto index = 0; index < 100; ++index) {
            auto fut = rg.run_when_memory_available([exec_cnt, index] {
                BOOST_REQUIRE_EQUAL(index, (*exec_cnt)++);
            });
            BOOST_REQUIRE_EQUAL(fut.available(), false);
            executions.push_back(std::move(fut));
        }

        region.reset();
        quiesce(when_all(executions.begin(), executions.end()));
    });
}

SEASTAR_TEST_CASE(test_region_groups_linear_hierarchy_throttling_moving_restriction) {
    // Hierarchy here is A -> B -> C.
    // We will fill B causing an execution in C to fail. We then fill A and free B.
    //
    // C should still be blocked.
    return seastar::async([] {
        region_group_reclaimer simple_reclaimer(logalloc::segment_size);

        test_region_group root(simple_reclaimer);
        test_region_group inner(&root, simple_reclaimer);
        test_region_group child(&inner, simple_reclaimer);

        auto inner_region = std::make_unique<test_region>(inner);
        auto root_region = std::make_unique<test_region>(root);

        // fill the inner node. Try allocating at child level. Should not be allowed.
        circular_buffer<managed_bytes> big_alloc;
        with_allocator(inner_region->allocator(), [&big_alloc] {
            big_alloc.push_back(bytes(bytes::initialized_later(), logalloc::segment_size));
        });
        BOOST_REQUIRE_GE(inner.memory_used(), logalloc::segment_size);

        auto fut = child.run_when_memory_available([] {});
        BOOST_REQUIRE_EQUAL(fut.available(), false);

        // Now fill the root...
        with_allocator(root_region->allocator(), [&big_alloc] {
            big_alloc.push_back(bytes(bytes::initialized_later(), logalloc::segment_size));
        });
        BOOST_REQUIRE_GE(root.memory_used(), logalloc::segment_size);

        // And free the inner node. We will verify that
        // 1) the notifications that the inner node sent the child when it was freed won't
        //    erroneously cause it to execute
        // 2) the child is still able to receive notifications from the root
        with_allocator(inner_region->allocator(), [&big_alloc] {
            big_alloc.pop_front();
        });
        inner_region.reset();

        // Verifying (1)
        // Can't quiesce because we don't want to wait on the futures.
        sleep(10ms);
        BOOST_REQUIRE_EQUAL(fut.available(), false);

        // Verifying (2)
        with_allocator(root_region->allocator(), [&big_alloc] {
            big_alloc.pop_front();
        });
        root_region.reset();
        quiesce(std::move(fut));
    });
}

SEASTAR_TEST_CASE(test_region_groups_tree_hierarchy_throttling_leaf_alloc) {
    return seastar::async([] {
        class leaf {
            region_group_reclaimer _leaf_reclaimer;
            test_region_group _rg;
            std::unique_ptr<test_region> _region;
        public:
            leaf(test_region_group& parent)
                : _leaf_reclaimer(logalloc::segment_size)
                , _rg(&parent, _leaf_reclaimer)
                , _region(std::make_unique<test_region>(_rg))
                {}

            void alloc(size_t size) {
                _region->alloc(size);
            }

            future<> try_alloc(size_t size) {
                return _rg.run_when_memory_available([this, size] {
                    alloc(size);
                });
            }
            void reset() {
                _region.reset(new test_region(_rg));
            }
        };

        region_group_reclaimer simple_reclaimer(logalloc::segment_size);
        test_region_group parent(simple_reclaimer);

        leaf first_leaf(parent);
        leaf second_leaf(parent);
        leaf third_leaf(parent);

        first_leaf.alloc(logalloc::segment_size);
        second_leaf.alloc(logalloc::segment_size);
        third_leaf.alloc(logalloc::segment_size);

        auto fut_1 = first_leaf.try_alloc(sizeof(uint64_t));
        auto fut_2 = second_leaf.try_alloc(sizeof(uint64_t));
        auto fut_3 = third_leaf.try_alloc(sizeof(uint64_t));

        BOOST_REQUIRE_EQUAL(fut_1.available() || fut_2.available() || fut_3.available(), false);

        // Total memory is still 2 * segment_size, can't proceed
        first_leaf.reset();
        // Can't quiesce because we don't want to wait on the futures.
        sleep(10ms);

        BOOST_REQUIRE_EQUAL(fut_1.available() || fut_2.available() || fut_3.available(), false);

        // Now all futures should resolve.
        first_leaf.reset();
        second_leaf.reset();
        third_leaf.reset();
        quiesce(when_all(std::move(fut_1), std::move(fut_2), std::move(fut_3)));
    });
}

// Helper for all async reclaim tests.
class test_async_reclaim_region {
    logalloc::region _region;
    std::vector<managed_bytes> _alloc;
    size_t _alloc_size;
    // Make sure we don't reclaim the same region more than once. It is supposed to be empty
    // after the first reclaim
    int _reclaim_counter = 0;
    region_group& _rg;
public:
    test_async_reclaim_region(region_group& rg, size_t alloc_size)
            : _region(rg)
            , _alloc_size(alloc_size)
            , _rg(rg)
    {
        with_allocator(_region.allocator(), [this] {
            _alloc.push_back(bytes(bytes::initialized_later(), this->_alloc_size));
        });

    }

    ~test_async_reclaim_region() {
        with_allocator(_region.allocator(), [this] {
            std::vector<managed_bytes>().swap(_alloc);
        });
    }

    size_t evict() {
        BOOST_REQUIRE_EQUAL(_reclaim_counter++, 0);
        with_allocator(_region.allocator(), [this] {
            std::vector<managed_bytes>().swap(_alloc);
        });
        _region = logalloc::region(_rg);
        return this->_alloc_size;
    }
    static test_async_reclaim_region& from_region(region* region_ptr) {
        auto aptr = boost::intrusive::get_parent_from_member(region_ptr, &test_async_reclaim_region::_region);
        return *aptr;
    }
};

class test_reclaimer: public region_group_reclaimer {
    size_t _threshold;
    test_reclaimer *_result_accumulator;
    region_group _rg;
    std::vector<size_t> _reclaim_sizes;
    bool _shutdown = false;
    shared_promise<> _unleash_reclaimer;
    seastar::gate _reclaimers_done;
public:
    virtual void start_reclaiming() noexcept override {
        with_gate(_reclaimers_done, [this] {
            return _unleash_reclaimer.get_shared_future().then([this] {
                while (this->under_pressure()) {
                    size_t reclaimed = test_async_reclaim_region::from_region(_rg.get_largest_region()).evict();
                    _result_accumulator->_reclaim_sizes.push_back(reclaimed);
                }
            });
        });
    }

    ~test_reclaimer() {
        _reclaimers_done.close().get();
        _rg.shutdown().get();
    }

    std::vector<size_t>& reclaim_sizes() {
        return _reclaim_sizes;
    }

    region_group& rg() {
        return _rg;
    }

    test_reclaimer(size_t threshold) : region_group_reclaimer(threshold), _result_accumulator(this), _rg(*this) {}
    test_reclaimer(test_reclaimer& parent, size_t threshold) : region_group_reclaimer(threshold), _result_accumulator(&parent), _rg(&parent._rg, *this) {}

    void unleash() {
        _unleash_reclaimer.set_value();
    }
};

SEASTAR_TEST_CASE(test_region_groups_basic_throttling_simple_active_reclaim) {
    return seastar::async([] {
        // allocate a single region to exhaustion, and make sure active reclaim is activated.
        test_reclaimer simple(logalloc::segment_size);
        test_async_reclaim_region simple_region(simple.rg(), logalloc::segment_size);
        simple.unleash();

        // Can't run this function until we have reclaimed something
        auto fut = simple.rg().run_when_memory_available([] {});

        // Initially not available
        BOOST_REQUIRE_EQUAL(fut.available(), false);
        quiesce(std::move(fut));

        BOOST_REQUIRE_EQUAL(simple.reclaim_sizes().size(), 1);
    });
}

SEASTAR_TEST_CASE(test_region_groups_basic_throttling_active_reclaim_worst_offender) {
    return seastar::async([] {
        // allocate three regions with three different sizes (segment boundary must be used due to
        // LSA granularity).
        //
        // The function can only be executed when all three are freed - which exercises continous
        // reclaim, but they must be freed in descending order of their sizes
        test_reclaimer simple(logalloc::segment_size);

        test_async_reclaim_region small_region(simple.rg(), logalloc::segment_size);
        test_async_reclaim_region medium_region(simple.rg(), 2 * logalloc::segment_size);
        test_async_reclaim_region big_region(simple.rg(), 3 * logalloc::segment_size);
        simple.unleash();

        // Can't run this function until we have reclaimed
        auto fut = simple.rg().run_when_memory_available([&simple] {
            BOOST_REQUIRE_EQUAL(simple.reclaim_sizes().size(), 3);
        });

        // Initially not available
        BOOST_REQUIRE_EQUAL(fut.available(), false);
        quiesce(std::move(fut));

        // Test if the ordering is the one we have expected
        BOOST_REQUIRE_EQUAL(simple.reclaim_sizes()[2], logalloc::segment_size);
        BOOST_REQUIRE_EQUAL(simple.reclaim_sizes()[1], 2 * logalloc::segment_size);
        BOOST_REQUIRE_EQUAL(simple.reclaim_sizes()[0], 3 * logalloc::segment_size);
    });
}

SEASTAR_TEST_CASE(test_region_groups_basic_throttling_active_reclaim_leaf_offender) {
    return seastar::async([] {
        // allocate a parent region group (A) with two leaf region groups (B and C), so that B has
        // the largest size, then A, then C. Make sure that the freeing happens in descending order.
        // of their sizes regardless of the topology
        test_reclaimer root(logalloc::segment_size);
        test_reclaimer large_leaf(root, logalloc::segment_size);
        test_reclaimer small_leaf(root, logalloc::segment_size);

        test_async_reclaim_region small_region(small_leaf.rg(), logalloc::segment_size);
        test_async_reclaim_region medium_region(root.rg(), 2 * logalloc::segment_size);
        test_async_reclaim_region big_region(large_leaf.rg(), 3 * logalloc::segment_size);
        root.unleash();
        large_leaf.unleash();
        small_leaf.unleash();

        // Can't run this function until we have reclaimed. Try at the root, and we'll make sure
        // that the leaves are forced correctly.
        auto fut = root.rg().run_when_memory_available([&root] {
            BOOST_REQUIRE_EQUAL(root.reclaim_sizes().size(), 3);
        });

        // Initially not available
        BOOST_REQUIRE_EQUAL(fut.available(), false);
        quiesce(std::move(fut));

        // Test if the ordering is the one we have expected
        BOOST_REQUIRE_EQUAL(root.reclaim_sizes()[2], logalloc::segment_size);
        BOOST_REQUIRE_EQUAL(root.reclaim_sizes()[1], 2 * logalloc::segment_size);
        BOOST_REQUIRE_EQUAL(root.reclaim_sizes()[0], 3 * logalloc::segment_size);
    });
}

SEASTAR_TEST_CASE(test_region_groups_basic_throttling_active_reclaim_ancestor_block) {
    return seastar::async([] {
        // allocate a parent region group (A) with a leaf region group (B)
        // Make sure that active reclaim still works when we block at an ancestor
        test_reclaimer root(logalloc::segment_size);
        test_reclaimer leaf(root, logalloc::segment_size);

        test_async_reclaim_region root_region(root.rg(), logalloc::segment_size);
        root.unleash();
        leaf.unleash();

        // Can't run this function until we have reclaimed. Try at the leaf, and we'll make sure
        // that the root reclaims
        auto fut = leaf.rg().run_when_memory_available([&root] {
            BOOST_REQUIRE_EQUAL(root.reclaim_sizes().size(), 1);
        });

        // Initially not available
        BOOST_REQUIRE_EQUAL(fut.available(), false);
        quiesce(std::move(fut));

        BOOST_REQUIRE_EQUAL(root.reclaim_sizes()[0], logalloc::segment_size);
    });
}

SEASTAR_TEST_CASE(test_region_groups_basic_throttling_active_reclaim_big_region_goes_first) {
    return seastar::async([] {
        // allocate a parent region group (A) with a leaf region group (B). B's usage is higher, but
        // due to multiple small regions. Make sure we reclaim from A first.
        test_reclaimer root(logalloc::segment_size);
        test_reclaimer leaf(root, logalloc::segment_size);

        test_async_reclaim_region root_region(root.rg(), 4 * logalloc::segment_size);
        test_async_reclaim_region big_leaf_region(leaf.rg(), 3 * logalloc::segment_size);
        test_async_reclaim_region small_leaf_region(leaf.rg(), 2 * logalloc::segment_size);
        root.unleash();
        leaf.unleash();

        auto fut = root.rg().run_when_memory_available([&root] {
            BOOST_REQUIRE_EQUAL(root.reclaim_sizes().size(), 3);
        });

        // Initially not available
        BOOST_REQUIRE_EQUAL(fut.available(), false);
        quiesce(std::move(fut));

        BOOST_REQUIRE_EQUAL(root.reclaim_sizes()[2], 2 * logalloc::segment_size);
        BOOST_REQUIRE_EQUAL(root.reclaim_sizes()[1], 3 * logalloc::segment_size);
        BOOST_REQUIRE_EQUAL(root.reclaim_sizes()[0], 4 * logalloc::segment_size);
    });
}

SEASTAR_TEST_CASE(test_region_groups_basic_throttling_active_reclaim_no_double_reclaim) {
    return seastar::async([] {
        // allocate a parent region group (A) with a leaf region group (B), and let B go over limit.
        // Both A and B try to execute requests, and we need to make sure that doesn't cause B's
        // region eviction function to be called more than once. Node that test_async_reclaim_region
        // will already make sure that we don't have double calls, so all we have to do is to
        // generate a situation in which a double call would happen
        test_reclaimer root(logalloc::segment_size);
        test_reclaimer leaf(root, logalloc::segment_size);

        test_async_reclaim_region leaf_region(leaf.rg(), logalloc::segment_size);
        root.unleash();
        leaf.unleash();

        auto fut_root = root.rg().run_when_memory_available([&root] {
            BOOST_REQUIRE_EQUAL(root.reclaim_sizes().size(), 1);
        });

        auto fut_leaf = leaf.rg().run_when_memory_available([&root] {
            BOOST_REQUIRE_EQUAL(root.reclaim_sizes().size(), 1);
        });

        // Initially not available
        BOOST_REQUIRE_EQUAL(fut_root.available(), false);
        BOOST_REQUIRE_EQUAL(fut_leaf.available(), false);
        quiesce(std::move(fut_root));
        quiesce(std::move(fut_leaf));

        BOOST_REQUIRE_EQUAL(root.reclaim_sizes().size(), 1);
        BOOST_REQUIRE_EQUAL(root.reclaim_sizes()[0], logalloc::segment_size);
    });
}

// Reproduces issue #2021
SEASTAR_TEST_CASE(test_no_crash_when_a_lot_of_requests_released_which_change_region_group_size) {
    return seastar::async([] {
#ifndef DEFAULT_ALLOCATOR // Because we need memory::stats().free_memory();
        logging::logger_registry().set_logger_level("lsa", seastar::log_level::debug);

        auto free_space = memory::stats().free_memory();
        size_t threshold = size_t(0.75 * free_space);
        region_group_reclaimer recl(threshold, threshold);
        region_group gr(recl);
        auto close_gr = defer([&gr] { gr.shutdown().get(); });
        region r(gr);

        with_allocator(r.allocator(), [&] {
            std::vector<managed_bytes> objs;

            r.make_evictable([&] {
                if (objs.empty()) {
                    return memory::reclaiming_result::reclaimed_nothing;
                }
                with_allocator(r.allocator(), [&] {
                    objs.pop_back();
                });
                return memory::reclaiming_result::reclaimed_something;
            });

            auto fill_to_pressure = [&] {
                while (!recl.under_pressure()) {
                    objs.emplace_back(managed_bytes(managed_bytes::initialized_later(), 1024));
                }
            };

            utils::phased_barrier request_barrier;
            auto wait_for_requests = defer([&] { request_barrier.advance_and_await().get(); });

            for (int i = 0; i < 1000000; ++i) {
                fill_to_pressure();
                future<> f = gr.run_when_memory_available([&, op = request_barrier.start()] {
                    // Trigger group size change (Refs issue #2021)
                    gr.update(-10);
                    gr.update(+10);
                });
                BOOST_REQUIRE(!f.available());
            }

            // Release
            while (recl.under_pressure()) {
                objs.pop_back();
            }
        });
#endif
    });
}

SEASTAR_TEST_CASE(test_reclaiming_runs_as_long_as_there_is_soft_pressure) {
    return seastar::async([] {
        size_t hard_threshold = logalloc::segment_size * 8;
        size_t soft_threshold = hard_threshold / 2;

        class reclaimer : public region_group_reclaimer {
            bool _reclaim = false;
        protected:
            void start_reclaiming() noexcept override {
                _reclaim = true;
            }

            void stop_reclaiming() noexcept override {
                _reclaim = false;
            }
        public:
            reclaimer(size_t hard_threshold, size_t soft_threshold)
                : region_group_reclaimer(hard_threshold, soft_threshold)
            { }
            bool reclaiming() const { return _reclaim; };
        };

        reclaimer recl(hard_threshold, soft_threshold);
        region_group gr(recl);
        auto close_gr = defer([&gr] { gr.shutdown().get(); });
        region r(gr);

        with_allocator(r.allocator(), [&] {
            std::vector<managed_bytes> objs;

            BOOST_REQUIRE(!recl.reclaiming());

            while (!recl.over_soft_limit()) {
                objs.emplace_back(managed_bytes(managed_bytes::initialized_later(), logalloc::segment_size));
            }

            BOOST_REQUIRE(recl.reclaiming());

            while (!recl.under_pressure()) {
                objs.emplace_back(managed_bytes(managed_bytes::initialized_later(), logalloc::segment_size));
            }

            BOOST_REQUIRE(recl.reclaiming());

            while (recl.under_pressure()) {
                objs.pop_back();
            }

            BOOST_REQUIRE(recl.over_soft_limit());
            BOOST_REQUIRE(recl.reclaiming());

            while (recl.over_soft_limit()) {
                objs.pop_back();
            }

            BOOST_REQUIRE(!recl.reclaiming());
        });
    });
}
