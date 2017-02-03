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


#include <seastar/core/thread.hh>
#include <seastar/tests/test-utils.hh>

#include "mutation_source_test.hh"
#include "streamed_mutation.hh"
#include "frozen_mutation.hh"
#include "tests/test_services.hh"
#include "schema_builder.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

void check_order_of_fragments(streamed_mutation sm)
{
    stdx::optional<position_in_partition> previous;
    position_in_partition::less_compare cmp(*sm.schema());
    auto mf = sm().get0();
    while (mf) {
        if (previous) {
            BOOST_REQUIRE(cmp(*previous, mf->position()));
        }
        previous = position_in_partition(mf->position());
        mf = sm().get0();
    }
}

SEASTAR_TEST_CASE(test_mutation_from_streamed_mutation_from_mutation) {
    return seastar::async([] {
        for_each_mutation([&] (const mutation& m) {
            auto get_sm = [&] {
                return streamed_mutation_from_mutation(mutation(m));
            };

            check_order_of_fragments(get_sm());
            auto mopt = mutation_from_streamed_mutation(get_sm()).get0();
            BOOST_REQUIRE(mopt);
            BOOST_REQUIRE_EQUAL(m, *mopt);
        });
    });
}

SEASTAR_TEST_CASE(test_abandoned_streamed_mutation_from_mutation) {
    return seastar::async([] {
        for_each_mutation([&] (const mutation& m) {
            auto sm = streamed_mutation_from_mutation(mutation(m));
            sm().get();
            sm().get();
            // We rely on AddressSanitizer telling us if nothing was leaked.
        });
    });
}

SEASTAR_TEST_CASE(test_mutation_merger) {
    return seastar::async([] {
        for_each_mutation_pair([&] (const mutation& m1, const mutation& m2, are_equal) {
            if (m1.schema()->version() != m2.schema()->version()) {
                return;
            }

            auto m12 = m1;
            m12.apply(m2);

            auto get_sm = [&] {
                std::vector<streamed_mutation> sms;
                sms.emplace_back(streamed_mutation_from_mutation(mutation(m1)));
                sms.emplace_back(streamed_mutation_from_mutation(mutation(m2.schema(), m1.decorated_key(), m2.partition())));
                return merge_mutations(std::move(sms));
            };

            check_order_of_fragments(get_sm());
            auto mopt = mutation_from_streamed_mutation(get_sm()).get0();
            BOOST_REQUIRE(mopt);
            BOOST_REQUIRE(m12.partition().difference(m1.schema(), mopt->partition()).empty());
            BOOST_REQUIRE(mopt->partition().difference(m1.schema(), m12.partition()).empty());
        });
    });
}

SEASTAR_TEST_CASE(test_freezing_streamed_mutations) {
    return seastar::async([] {
        storage_service_for_tests ssft;

        for_each_mutation([&] (const mutation& m) {
            auto fm = freeze(streamed_mutation_from_mutation(mutation(m))).get0();

            auto m1 = fm.unfreeze(m.schema());
            BOOST_REQUIRE_EQUAL(m, m1);

            auto fm1 = freeze(m);
            BOOST_REQUIRE(fm.representation() == fm1.representation());
        });
    });
}

SEASTAR_TEST_CASE(test_fragmenting_and_freezing_streamed_mutations) {
    return seastar::async([] {
        storage_service_for_tests ssft;

        for_each_mutation([&] (const mutation& m) {
            std::vector<frozen_mutation> fms;

            fragment_and_freeze(streamed_mutation_from_mutation(mutation(m)), [&] (auto fm, bool frag) {
                BOOST_REQUIRE(!frag);
                fms.emplace_back(std::move(fm));
                return make_ready_future<>();
            }, std::numeric_limits<size_t>::max()).get0();

            BOOST_REQUIRE_EQUAL(fms.size(), 1);

            auto m1 = fms.back().unfreeze(m.schema());
            BOOST_REQUIRE_EQUAL(m, m1);

            fms.clear();

            stdx::optional<bool> fragmented;
            fragment_and_freeze(streamed_mutation_from_mutation(mutation(m)), [&] (auto fm, bool frag) {
                BOOST_REQUIRE(!fragmented || *fragmented == frag);
                *fragmented = frag;
                fms.emplace_back(std::move(fm));
                return make_ready_future<>();
            }, 1).get0();

            auto expected_fragments = m.partition().clustered_rows().calculate_size()
                                      + m.partition().row_tombstones().size()
                                      + !m.partition().static_row().empty();
            BOOST_REQUIRE_EQUAL(fms.size(), std::max(expected_fragments, size_t(1)));
            BOOST_REQUIRE(expected_fragments < 2 || *fragmented);

            auto m2 = fms.back().unfreeze(m.schema());
            fms.pop_back();
            while (!fms.empty()) {
                m2.partition().apply(*m.schema(), fms.back().partition(), *m.schema());
                fms.pop_back();
            }
            BOOST_REQUIRE_EQUAL(m, m2);
        });
    });
}

SEASTAR_TEST_CASE(test_range_tombstones_stream) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
                .with_column("pk", int32_type, column_kind::partition_key)
                .with_column("ck1", int32_type, column_kind::clustering_key)
                .with_column("ck2", int32_type, column_kind::clustering_key)
                .with_column("r", int32_type)
                .build();

        auto pk = partition_key::from_single_value(*s, int32_type->decompose(0));
        auto create_ck = [&] (std::vector<int> v) {
            std::vector<bytes> vs;
            boost::transform(v, std::back_inserter(vs), [] (int x) { return int32_type->decompose(x); });
            return clustering_key_prefix::from_exploded(*s, std::move(vs));
        };

        tombstone t0(0, { });
        tombstone t1(1, { });

        auto rt1 = range_tombstone(create_ck({ 1 }), t0, bound_kind::incl_start, create_ck({ 1, 3 }), bound_kind::incl_end);
        auto rt2 = range_tombstone(create_ck({ 1, 1 }), t1, bound_kind::incl_start, create_ck({ 1, 3 }), bound_kind::excl_end);
        auto rt3 = range_tombstone(create_ck({ 1, 1 }), t0,  bound_kind::incl_start, create_ck({ 2 }), bound_kind::incl_end);
        auto rt4 = range_tombstone(create_ck({ 2 }), t0, bound_kind::excl_start, create_ck({ 2, 2 }), bound_kind::incl_end);

        mutation_fragment cr1 = clustering_row(create_ck({ 0, 0 }));
        mutation_fragment cr2 = clustering_row(create_ck({ 1, 0 }));
        mutation_fragment cr3 = clustering_row(create_ck({ 1, 1 }));
        auto cr4 = rows_entry(create_ck({ 1, 2 }));
        auto cr5 = rows_entry(create_ck({ 1, 3 }));

        range_tombstone_stream rts(*s);
        rts.apply(range_tombstone(rt1));
        rts.apply(range_tombstone(rt2));
        rts.apply(range_tombstone(rt4));

        mutation_fragment_opt mf = rts.get_next(cr1);
        BOOST_REQUIRE(!mf);

        mf = rts.get_next(cr2);
        BOOST_REQUIRE(mf && mf->is_range_tombstone());
        auto expected1 = range_tombstone(create_ck({ 1 }), t0, bound_kind::incl_start, create_ck({ 1, 1 }), bound_kind::excl_end);
        BOOST_REQUIRE(mf->as_range_tombstone().equal(*s, expected1));

        mf = rts.get_next(cr2);
        BOOST_REQUIRE(!mf);

        mf = rts.get_next(mutation_fragment(range_tombstone(rt3)));
        BOOST_REQUIRE(mf && mf->is_range_tombstone());
        BOOST_REQUIRE(mf->as_range_tombstone().equal(*s, rt2));

        mf = rts.get_next(cr3);
        BOOST_REQUIRE(!mf);

        mf = rts.get_next(cr4);
        BOOST_REQUIRE(!mf);

        mf = rts.get_next(cr5);
        BOOST_REQUIRE(mf && mf->is_range_tombstone());
        auto expected2 = range_tombstone(create_ck({ 1, 3 }), t0, bound_kind::incl_start, create_ck({ 1, 3 }), bound_kind::incl_end);
        BOOST_REQUIRE(mf->as_range_tombstone().equal(*s, expected2));

        mf = rts.get_next();
        BOOST_REQUIRE(mf && mf->is_range_tombstone());
        BOOST_REQUIRE(mf->as_range_tombstone().equal(*s, rt4));

        mf = rts.get_next();
        BOOST_REQUIRE(!mf);
    });
}

SEASTAR_TEST_CASE(test_mutation_hash) {
    return seastar::async([] {
        for_each_mutation_pair([] (auto&& m1, auto&& m2, are_equal eq) {
            auto get_hash = [] (streamed_mutation m) {
                md5_hasher h;
                m.key().feed_hash(h, *m.schema());

                mutation_hasher<md5_hasher> mh(*m.schema(), h);
                consume(m, std::move(mh)).get0();
                return h.finalize();
            };
            auto h1 = get_hash(streamed_mutation_from_mutation(mutation(m1)));
            auto h2 = get_hash(streamed_mutation_from_mutation(mutation(m2)));
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

