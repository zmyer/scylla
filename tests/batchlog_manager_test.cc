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


#include <boost/range/irange.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/test/unit_test.hpp>
#include <stdint.h>

#include "tests/test-utils.hh"
#include "tests/cql_test_env.hh"
#include "tests/cql_assertions.hh"

#include "core/future-util.hh"
#include "core/shared_ptr.hh"
#include "transport/messages/result_message.hh"
#include "cql3/query_processor.hh"
#include "db/batchlog_manager.hh"

#include "disk-error-handler.hh"
#include "message/messaging_service.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

static atomic_cell make_atomic_cell(bytes value) {
    return atomic_cell::make_live(0, std::move(value));
};

SEASTAR_TEST_CASE(test_execute_batch) {
    return do_with_cql_env([] (auto& e) {
        auto& qp = e.local_qp();
        auto bp = make_lw_shared<db::batchlog_manager>(qp);

        return e.execute_cql("create table cf (p1 varchar, c1 int, r1 int, PRIMARY KEY (p1, c1));").discard_result().then([&qp, &e, bp] {
            auto& db = e.local_db();
            auto s = db.find_schema("ks", "cf");

            const column_definition& r1_col = *s->get_column_definition("r1");
            auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(1)});

            mutation m(key, s);
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type->decompose(100)));

            using namespace std::chrono_literals;

            auto version = net::messaging_service::current_version;
            auto bm = bp->get_batch_log_mutation_for({ m }, s->id(), version, db_clock::now() - db_clock::duration(3h));

            return qp.proxy().local().mutate_locally(bm).then([bp] {
                return bp->count_all_batches().then([](auto n) {
                    BOOST_CHECK_EQUAL(n, 1);
                }).then([bp] {
                    return bp->do_batch_log_replay();
                });
            });
        }).then([&qp, bp] {
            return qp.execute_internal("select * from ks.cf where p1 = ? and c1 = ?;", { sstring("key1"), 1 }).then([](auto rs) {
                BOOST_REQUIRE(!rs->empty());
                auto i = rs->one().template get_as<int32_t>("r1");
                BOOST_CHECK_EQUAL(i, int32_t(100));
            });
        });
    });
}

