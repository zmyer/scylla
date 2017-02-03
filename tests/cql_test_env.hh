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

#pragma once

#include <functional>
#include <vector>

#include <core/distributed.hh>
#include "core/sstring.hh"
#include "core/future.hh"
#include "core/shared_ptr.hh"

#include "transport/messages/result_message_base.hh"
#include "cql3/query_options_fwd.hh"
#include "cql3/values.hh"
#include "bytes.hh"
#include "schema.hh"

class database;

namespace cql3 {
    class query_processor;
}

class not_prepared_exception : public std::runtime_error {
public:
    not_prepared_exception(const bytes& id) : std::runtime_error(sprint("Not prepared: %s", id)) {}
};

namespace db {
    class config;
}

class cql_test_env {
public:
    virtual ~cql_test_env() {};

    virtual future<::shared_ptr<transport::messages::result_message>> execute_cql(const sstring& text) = 0;

    virtual future<::shared_ptr<transport::messages::result_message>> execute_cql(
        const sstring& text, std::unique_ptr<cql3::query_options> qo) = 0;

    virtual future<bytes> prepare(sstring query) = 0;

    virtual future<::shared_ptr<transport::messages::result_message>> execute_prepared(
        bytes id, std::vector<cql3::raw_value> values) = 0;

    virtual future<> create_table(std::function<schema(const sstring&)> schema_maker) = 0;

    virtual future<> require_keyspace_exists(const sstring& ks_name) = 0;

    virtual future<> require_table_exists(const sstring& ks_name, const sstring& cf_name) = 0;

    virtual future<> require_column_has_value(
        const sstring& table_name,
        std::vector<data_value> pk,
        std::vector<data_value> ck,
        const sstring& column_name,
        data_value expected) = 0;

    virtual future<> stop() = 0;

    virtual database& local_db() = 0;

    virtual cql3::query_processor& local_qp() = 0;

    virtual distributed<database>& db() = 0;

    virtual distributed<cql3::query_processor> & qp() = 0;
};

future<> do_with_cql_env(std::function<future<>(cql_test_env&)> func);
future<> do_with_cql_env(std::function<future<>(cql_test_env&)> func, const db::config&);
future<> do_with_cql_env_thread(std::function<void(cql_test_env&)> func);
future<> do_with_cql_env_thread(std::function<void(cql_test_env&)> func, const db::config&);
