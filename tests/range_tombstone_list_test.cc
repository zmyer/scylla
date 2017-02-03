/*
 * Copyright (C) 2016 ScyllaDB
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

#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <random>
#include <iostream>
#include "keys.hh"
#include "schema_builder.hh"
#include "range_tombstone_list.hh"
#include "range_tombstone_to_prefix_tombstone_converter.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

static schema_ptr s = schema_builder("ks", "cf")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck1", int32_type, column_kind::clustering_key)
        .with_column("ck2", int32_type, column_kind::clustering_key)
        .with_column("v", int32_type, column_kind::regular_column)
        .build();

static auto gc_now = gc_clock::now();

static clustering_key_prefix key(std::vector<int32_t> components) {
    std::vector<bytes> exploded;
    std::transform(components.begin(), components.end(), std::back_inserter(exploded), [](auto&& c) {
        return int32_type->decompose(c);
    });
    return clustering_key_prefix::from_clustering_prefix(*s, exploded_clustering_prefix(std::move(exploded)));
}

static void assert_rt(auto&& expected, auto&& actual) {
    BOOST_REQUIRE(expected.equal(*s, actual));
}

static range_tombstone rt(int32_t start, int32_t end, api::timestamp_type timestamp) {
    return range_tombstone(key({start}), key({end}), {timestamp, gc_now});
}


static range_tombstone rtie(int32_t start, int32_t end, api::timestamp_type timestamp) {
    return range_tombstone(key({start}), bound_kind::incl_start, key({end}), bound_kind::excl_end, {timestamp, gc_now});
}

static range_tombstone rtei(int32_t start, int32_t end, api::timestamp_type timestamp) {
    return range_tombstone(key({start}), bound_kind::excl_start, key({end}), bound_kind::incl_end, {timestamp, gc_now});
}

static range_tombstone rtee(int32_t start, int32_t end, api::timestamp_type timestamp) {
    return range_tombstone(key({start}), bound_kind::excl_start, key({end}), bound_kind::excl_end, {timestamp, gc_now});
}

static range_tombstone at_least(int32_t start, api::timestamp_type timestamp) {
    return range_tombstone(bound_view(key({start}), bound_kind::incl_start), bound_view::top(), {timestamp, gc_now});
}

static range_tombstone at_most(int32_t end, api::timestamp_type timestamp) {
    return range_tombstone(bound_view::bottom(), bound_view(key({end}), bound_kind::incl_end), {timestamp, gc_now});
}

static range_tombstone less_than(int32_t end, api::timestamp_type timestamp) {
    return range_tombstone(bound_view::bottom(), bound_view(key({end}), bound_kind::excl_end), {timestamp, gc_now});
}

static range_tombstone greater_than(int32_t start, api::timestamp_type timestamp) {
    return range_tombstone(bound_view(key({start}), bound_kind::excl_start), bound_view::top(), {timestamp, gc_now});
}

BOOST_AUTO_TEST_CASE(test_sorted_addition) {
    range_tombstone_list l(*s);

    auto rt1 = rt(1, 5, 3);
    auto rt2 = rt(7, 10, 2);
    auto rt3 = rt(10, 13, 1);

    l.apply(*s, rt1);
    l.apply(*s, rt2);
    l.apply(*s, rt3);

    auto it = l.begin();
    assert_rt(rt1, *it++);
    assert_rt(rt2, *it++);
    assert_rt(rtei(10, 13, 1), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_non_sorted_addition) {
    range_tombstone_list l(*s);

    auto rt1 = rt(1, 5, 3);
    auto rt2 = rt(7, 10, 2);
    auto rt3 = rt(10, 13, 1);

    l.apply(*s, rt2);
    l.apply(*s, rt1);
    l.apply(*s, rt3);

    auto it = l.begin();
    assert_rt(rt1, *it++);
    assert_rt(rt2, *it++);
    assert_rt(rtei(10, 13, 1), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_overlapping_addition) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(4, 10, 3));
    l.apply(*s, rt(1, 7, 2));
    l.apply(*s, rt(8, 13, 4));
    l.apply(*s, rt(0, 15, 1));

    auto it = l.begin();
    assert_rt(rtie(0, 1, 1), *it++);
    assert_rt(rtie(1, 4, 2), *it++);
    assert_rt(rtie(4, 8, 3), *it++);
    assert_rt(rt(8, 13, 4), *it++);
    assert_rt(rtei(13, 15, 1), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_simple_overlap) {
    range_tombstone_list l1(*s);

    l1.apply(*s, rt(0, 10, 3));
    l1.apply(*s, rt(3, 7, 5));

    auto it = l1.begin();
    assert_rt(rtie(0, 3, 3), *it++);
    assert_rt(rt(3, 7, 5), *it++);
    assert_rt(rtei(7, 10, 3), *it++);
    BOOST_REQUIRE(it == l1.end());

    range_tombstone_list l2(*s);

    l2.apply(*s, rt(0, 10, 3));
    l2.apply(*s, rt(3, 7, 2));

    it = l2.begin();
    assert_rt(rt(0, 10, 3), *it++);
    BOOST_REQUIRE(it == l2.end());
}

BOOST_AUTO_TEST_CASE(test_overlapping_previous_end_equals_start) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(11, 12, 2));
    l.apply(*s, rt(1, 4, 2));
    l.apply(*s, rt(4, 10, 5));

    BOOST_REQUIRE(2 == l.search_tombstone_covering(*s, key({3})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({4})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({8})).timestamp);
    BOOST_REQUIRE(3 == l.size());
}

BOOST_AUTO_TEST_CASE(test_search) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(0, 4, 5));
    l.apply(*s, rt(4, 6, 2));
    l.apply(*s, rt(9, 12, 1));
    l.apply(*s, rt(14, 15, 3));
    l.apply(*s, rt(15, 17, 6));

    BOOST_REQUIRE(tombstone() == l.search_tombstone_covering(*s, key({-1})));

    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({0})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({3})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({4})).timestamp);

    BOOST_REQUIRE(tombstone() == l.search_tombstone_covering(*s, key({18})));

    BOOST_REQUIRE(3 == l.search_tombstone_covering(*s, key({14})).timestamp);

    BOOST_REQUIRE(6 == l.search_tombstone_covering(*s, key({15})).timestamp);

    BOOST_REQUIRE(tombstone() == l.search_tombstone_covering(*s, key({18})));

    range_tombstone_list l2(*s);
    l2.apply(*s, rt(1, 2, 5));
    BOOST_REQUIRE(5 == l2.search_tombstone_covering(*s, key({2})).timestamp);

    range_tombstone_list l3(*s);
    l3.apply(*s, rtie(1, 2, 5));
    BOOST_REQUIRE(tombstone() == l3.search_tombstone_covering(*s, key({2})));
}

BOOST_AUTO_TEST_CASE(test_search_prefix) {
    range_tombstone_list l(*s);

    l.apply(*s, range_tombstone(key({1}), bound_kind::incl_start, key({1, 2}), bound_kind::incl_end, {8, gc_now}));
    l.apply(*s, range_tombstone(key({1, 2}), bound_kind::excl_start, key({1, 3}), bound_kind::incl_end, {12, gc_now}));

    BOOST_REQUIRE(8 == l.search_tombstone_covering(*s, key({1})).timestamp);

    range_tombstone_list l1(*s);

    l1.apply(*s, range_tombstone(key({1}), bound_kind::excl_start, key({1, 2}), bound_kind::incl_end, {8, gc_now}));
    l1.apply(*s, range_tombstone(key({1, 2}), bound_kind::excl_start, key({1, 3}), bound_kind::incl_end, {12, gc_now}));

    BOOST_REQUIRE(tombstone() == l1.search_tombstone_covering(*s, key({1})));

    range_tombstone_list l2(*s);

    l2.apply(*s, rt(1, 1, 8));

    BOOST_REQUIRE(8 == l2.search_tombstone_covering(*s, key({1, 2})).timestamp);

    range_tombstone_list l3(*s);

    l3.apply(*s, range_tombstone(key({1}), bound_kind::incl_start, key({1, 2}), bound_kind::incl_end, {8, gc_now}));
    l3.apply(*s, range_tombstone(key({1, 2}), bound_kind::excl_start, key({1, 3}), bound_kind::incl_end, {10, gc_now}));
    l3.apply(*s, range_tombstone(key({1, 3}), bound_kind::excl_start, key({1}), bound_kind::incl_end, {12, gc_now}));
    BOOST_REQUIRE(8 == l3.search_tombstone_covering(*s, key({1})).timestamp);

    range_tombstone_list l4(*s);

    l4.apply(*s, range_tombstone(key({1, 2}), bound_kind::incl_start, key({1, 3}), bound_kind::incl_end, {12, gc_now}));
    BOOST_REQUIRE(tombstone() == l4.search_tombstone_covering(*s, key({1})));
}

BOOST_AUTO_TEST_CASE(test_add_prefixes) {
    range_tombstone_list l(*s);

    l.apply(*s, range_tombstone(key({1}), bound_kind::excl_start, key({1, 2}), bound_kind::incl_end, {8, gc_now}));
    l.apply(*s, range_tombstone(key({1}), bound_kind::incl_start, key({1}), bound_kind::incl_end, {8, gc_now}));

    auto it = l.begin();
    assert_rt(range_tombstone(key({1}), bound_kind::incl_start, key({1}), bound_kind::incl_end, {8, gc_now}), *it++);
    assert_rt(range_tombstone(key({1}), bound_kind::excl_start, key({1, 2}), bound_kind::incl_end, {8, gc_now}), *it++);
    BOOST_REQUIRE(it == l.end());

    range_tombstone_list l2(*s);

    l2.apply(*s, range_tombstone(key({1}), bound_kind::incl_start, key({1}), bound_kind::incl_end, {8, gc_now}));
    l2.apply(*s, range_tombstone(key({1}), bound_kind::excl_start, key({1, 2}), bound_kind::incl_end, {8, gc_now}));

    it = l2.begin();
    assert_rt(range_tombstone(key({1}), bound_kind::incl_start, key({1}), bound_kind::incl_end, {8, gc_now}), *it++);
    assert_rt(range_tombstone(key({1}), bound_kind::excl_start, key({1, 2}), bound_kind::incl_end, {8, gc_now}), *it++);
    BOOST_REQUIRE(it == l2.end());
}

BOOST_AUTO_TEST_CASE(test_add_different_prefixes) {
    range_tombstone_list l(*s);
    auto rt1 = range_tombstone(key({4}), key({4}), {7, gc_now});
    l.apply(*s, rt1);
    auto rt2 = range_tombstone(key({4, 1}), key({4, 2}), {7, gc_now});
    l.apply(*s, rt2);

    auto it = l.begin();
    assert_rt(rt1, *it++);
    BOOST_REQUIRE(it == l.end());

    auto rt3 = range_tombstone(key({4, 1}), key({4, 2}), {8, gc_now});
    l.apply(*s, rt3);

    it = l.begin();
    assert_rt(range_tombstone(key({4}), bound_kind::incl_start, key({4, 1}), bound_kind::excl_end, {7, gc_now}), *it++);
    assert_rt(range_tombstone(key({4, 1}), bound_kind::incl_start, key({4, 2}), bound_kind::incl_end, {8, gc_now}), *it++);
    assert_rt(range_tombstone(key({4, 2}), bound_kind::excl_start, key({4}), bound_kind::incl_end, {7, gc_now}), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_same) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(4, 4, 5));
    l.apply(*s, rt(4, 4, 6));
    l.apply(*s, rt(4, 4, 4));

    auto it = l.begin();
    assert_rt(rt(4, 4, 6), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_single_range_is_preserved) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(1, 2, 10));
    l.apply(*s, rt(7, 13, 8));
    l.apply(*s, rt(13, 13, 20));
    l.apply(*s, rt(13, 18, 12));

    auto it = l.begin();
    assert_rt(rt(1, 2, 10), *it++);
    assert_rt(rtie(7, 13, 8), *it++);
    assert_rt(rt(13, 13, 20), *it++);
    assert_rt(rtei(13, 18, 12),  *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_single_range_is_replaced) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(7, 13, 8));
    l.apply(*s, rt(13, 13, 20));
    l.apply(*s, rt(13, 18, 32));

    auto it = l.begin();
    assert_rt(rtie(7, 13, 8), *it++);
    assert_rt(rt(13, 18, 32),  *it++);
    BOOST_REQUIRE(it == l.end());
}

static bool assert_valid(range_tombstone_list& l) {
    bound_view::compare less(*s);
    auto it = l.begin();
    if (it == l.end()) {
        return true;
    }
    auto prev = *it;
    if (less(prev.end_bound(), prev.start_bound())) {
        std::cout << "Invalid empty slice " << prev << std::endl;
        return false;
    }
    while(++it != l.end()) {
        auto cur = *it;
        if (less(cur.end_bound(), cur.start_bound())) {
            std::cout << "Invalid empty slice " << cur << std::endl;
            return false;
        }
        if (less(cur.start_bound(), prev.end_bound())) {
            std::cout << "Ranges not in order or overlapping " << prev << " " << cur << std::endl;
            return false;
        }
    }
    return true;
}

static std::random_device rd;
static std::mt19937 gen(rd());
static std::uniform_int_distribution<int32_t> dist(0, 50);

static std::vector<range_tombstone> make_random() {
    std::vector<range_tombstone> rts;

    int32_t prev_end = 0;
    bool prev_end_incl = false;
    int32_t size = dist(gen) + 7;
    for (int32_t i = 0; i < size; ++i) {
        int32_t next_start = prev_end + dist(gen);
        int32_t next_end = next_start + dist(gen);

        bool start_incl = dist(gen) > 25;
        bool end_incl = dist(gen) > 25;

        if (prev_end == next_start) {
            start_incl = !prev_end_incl;
        }

        if (next_start == next_end) {
            if (start_incl) {
                end_incl = true;
            } else {
                next_end += 1;
            }
        }

        rts.emplace_back(key({next_start}),
                start_incl ? bound_kind::incl_start: bound_kind::excl_start,
                key({next_end}),
                end_incl ? bound_kind::incl_end: bound_kind::excl_end,
                tombstone(dist(gen), gc_now));

        prev_end = next_end;
        prev_end_incl = end_incl;
    }
    return rts;
}

BOOST_AUTO_TEST_CASE(test_add_random) {
    for (uint32_t i = 0; i < 1000; ++i) {
        auto input = make_random();
        range_tombstone_list l(*s);
        for (auto&& rt : input) {
            l.apply(*s, rt);
        }
        if (!assert_valid(l)) {
            std::cout << "For input:" << std::endl;
            for (auto&& rt : input) {
                std::cout << rt << std::endl;
            }
            std::cout << "Produced:" << std::endl;
            for (auto&& rt : l) {
                std::cout << rt << std::endl;
            }
            BOOST_REQUIRE(false);
        }
    }
}

BOOST_AUTO_TEST_CASE(test_non_sorted_addition_with_one_range_with_empty_end) {
    range_tombstone_list l(*s);

    auto rt1 = rt(1, 5, 3);
    auto rt2 = rt(7, 10, 2);
    auto rt3 = at_least(11, 1);

    l.apply(*s, rt2);
    l.apply(*s, rt3);
    l.apply(*s, rt1);

    auto it = l.begin();
    assert_rt(rt1, *it++);
    assert_rt(rt2, *it++);
    assert_rt(rt3, *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_range_with_empty_end_which_include_existing_range) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(4, 10, 3));
    l.apply(*s, at_least(3, 4));

    auto it = l.begin();
    assert_rt(at_least(3, 4), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_range_with_empty_start_and_end) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(4, 10, 3));
    l.apply(*s, at_most(12, 4));

    auto it = l.begin();
    assert_rt(at_most(12, 4), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_range_with_empty_end_to_range_with_empty_start_and_end) {
    range_tombstone_list l(*s);

    l.apply(*s, range_tombstone(bound_view::bottom(), bound_view::top(), tombstone(2, gc_now)));
    l.apply(*s, at_least(12, 4));

    auto it = l.begin();
    assert_rt(less_than(12, 2), *it++);
    assert_rt(at_least(12, 4), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_range_with_empty_end_which_include_existing_range_with_empty_end) {
    range_tombstone_list l(*s);

    l.apply(*s, at_least(5, 3));
    l.apply(*s, at_least(3, 4));

    auto it = l.begin();
    assert_rt(at_least(3, 4), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_included_range_to_range_with_empty_end) {
    range_tombstone_list l(*s);

    l.apply(*s, at_least(3, 3));
    l.apply(*s, rt(4, 10, 4));

    auto it = l.begin();
    assert_rt(rtie(3, 4, 3), *it++);
    assert_rt(rt(4, 10, 4), *it++);
    assert_rt(greater_than(10, 3), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_included_range_with_empty_end_to_range_with_empty_end) {
    range_tombstone_list l(*s);

    l.apply(*s, at_least(3, 3));
    l.apply(*s, at_least(5, 4));

    auto it = l.begin();
    assert_rt(rtie(3, 5, 3), *it++);
    assert_rt(at_least(5, 4), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_range_with_empty_end_which_overlaps_existing_range) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(4, 10, 3));
    l.apply(*s, at_least(6, 4));

    auto it = l.begin();
    assert_rt(rtie(4, 6, 3), *it++);
    assert_rt(at_least(6, 4), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_add_overlapping_range_to_range_with_empty_end) {
    range_tombstone_list l(*s);

    l.apply(*s, at_least(3, 3));
    l.apply(*s, rt(1, 10, 4));

    auto it = l.begin();
    assert_rt(rt(1, 10, 4), *it++);
    assert_rt(greater_than(10, 3), *it++);
    BOOST_REQUIRE(it == l.end());
}

BOOST_AUTO_TEST_CASE(test_search_with_empty_start) {
    range_tombstone_list l(*s);

    l.apply(*s, at_most(4, 5));
    l.apply(*s, rt(4, 6, 2));
    l.apply(*s, rt(9, 12, 1));
    l.apply(*s, rt(14, 15, 3));
    l.apply(*s, rt(15, 17, 6));

    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({-1})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({0})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({3})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({4})).timestamp);

    BOOST_REQUIRE(2 == l.search_tombstone_covering(*s, key({5})).timestamp);

    BOOST_REQUIRE(tombstone() == l.search_tombstone_covering(*s, key({7})));

    BOOST_REQUIRE(3 == l.search_tombstone_covering(*s, key({14})).timestamp);

    BOOST_REQUIRE(6 == l.search_tombstone_covering(*s, key({15})).timestamp);

    BOOST_REQUIRE(tombstone() == l.search_tombstone_covering(*s, key({18})));
}

BOOST_AUTO_TEST_CASE(test_search_with_empty_end) {
    range_tombstone_list l(*s);

    l.apply(*s, rt(0, 4, 5));
    l.apply(*s, rt(4, 6, 2));
    l.apply(*s, rt(9, 12, 1));
    l.apply(*s, rt(14, 15, 3));
    l.apply(*s, at_least(15, 6));

    BOOST_REQUIRE(tombstone() == l.search_tombstone_covering(*s, key({-1})));

    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({0})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({3})).timestamp);
    BOOST_REQUIRE(5 == l.search_tombstone_covering(*s, key({4})).timestamp);

    BOOST_REQUIRE(2 == l.search_tombstone_covering(*s, key({5})).timestamp);

    BOOST_REQUIRE(tombstone() == l.search_tombstone_covering(*s, key({7})));

    BOOST_REQUIRE(3 == l.search_tombstone_covering(*s, key({14})).timestamp);

    BOOST_REQUIRE(6 == l.search_tombstone_covering(*s, key({15})).timestamp);
    BOOST_REQUIRE(6 == l.search_tombstone_covering(*s, key({1000})).timestamp);
}

BOOST_AUTO_TEST_CASE(test_range_tombstone_to_prefix_tombstone_converter) {
    range_tombstone_list l(*s);

    auto rt1 = range_tombstone(key({1}), key({1}), {7, gc_now});
    auto rt2 = range_tombstone(key({1, 2}), key({1, 2}), {8, gc_now});
    l.apply(*s, rt1);
    l.apply(*s, rt2);

    range_tombstone_to_prefix_tombstone_converter converter;
    std::vector<clustering_key_prefix> keys = { key({1, 2}), key({1}) };
    auto it = keys.begin();
    for (auto&& rt : l) {
        auto prefix = converter.convert(*s, rt);
        if (prefix) {
            BOOST_REQUIRE(prefix->equal(*s, *it));
            ++it;
        }
    }
}

BOOST_AUTO_TEST_CASE(test_difference_with_self) {
    range_tombstone_list l(*s);
    l.apply(*s, rt(1, 1, 7));
    l.apply(*s, rt(3, 3, 8));

    BOOST_REQUIRE(l.difference(*s, l).empty());
}

BOOST_AUTO_TEST_CASE(test_difference_with_bigger_tombstone) {
    range_tombstone_list l1(*s);
    l1.apply(*s, rt(1, 2, 3));
    l1.apply(*s, rt(5, 7, 3));
    l1.apply(*s, rt(8, 11, 3));
    l1.apply(*s, rt(12, 14, 3));

    range_tombstone_list l2(*s);
    l2.apply(*s, rt(3, 4, 2));
    l2.apply(*s, rt(6, 9, 2));
    l2.apply(*s, rt(10, 13, 2));

    auto diff = l1.difference(*s, l2);
    auto it = diff.begin();
    assert_rt(rt(1, 2, 3), *it++);
    assert_rt(rtie(5, 6, 3), *it++);
    assert_rt(rt(6, 7, 3), *it++);
    assert_rt(rt(8, 9, 3), *it++);
    assert_rt(rtee(9, 10, 3), *it++);
    assert_rt(rt(10, 11, 3), *it++);
    assert_rt(rt(12, 13, 3), *it++);
    assert_rt(rtei(13, 14, 3), *it++);
    BOOST_REQUIRE(it == diff.end());
}

BOOST_AUTO_TEST_CASE(test_difference_with_smaller_tombstone) {
    range_tombstone_list l1(*s);
    l1.apply(*s, rt(1, 2, 1));
    l1.apply(*s, rt(5, 7, 1));
    l1.apply(*s, rt(8, 11, 1));
    l1.apply(*s, rt(12, 14, 1));

    range_tombstone_list l2(*s);
    l2.apply(*s, rt(3, 4, 2));
    l2.apply(*s, rt(6, 9, 2));
    l2.apply(*s, rt(10, 13, 2));

    auto diff = l1.difference(*s, l2);
    auto it = diff.begin();
    assert_rt(rt(1, 2, 1), *it++);
    assert_rt(rtie(5, 6, 1), *it++);
    assert_rt(rtee(9, 10, 1), *it++);
    assert_rt(rtei(13, 14, 1), *it++);
    BOOST_REQUIRE(it == diff.end());
}
