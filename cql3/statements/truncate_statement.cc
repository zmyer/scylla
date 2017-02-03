/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright (C) 2014 ScyllaDB
 *
 * Modified by ScyllaDB
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

#include "cql3/statements/truncate_statement.hh"
#include "cql3/statements/prepared_statement.hh"
#include "cql3/cql_statement.hh"

#include <experimental/optional>

namespace cql3 {

namespace statements {

truncate_statement::truncate_statement(::shared_ptr<cf_name> name)
    : cf_statement{std::move(name)}
{
}

uint32_t truncate_statement::get_bound_terms()
{
    return 0;
}

::shared_ptr<prepared_statement> truncate_statement::prepare(database& db,cql_stats& stats)
{
    return ::make_shared<prepared>(this->shared_from_this());
}

bool truncate_statement::uses_function(const sstring& ks_name, const sstring& function_name) const
{
    return parsed_statement::uses_function(ks_name, function_name);
}

bool truncate_statement::depends_on_keyspace(const sstring& ks_name) const
{
    return false;
}

bool truncate_statement::depends_on_column_family(const sstring& cf_name) const
{
    return false;
}

future<> truncate_statement::check_access(const service::client_state& state)
{
    return state.has_column_family_access(keyspace(), column_family(), auth::permission::MODIFY);
}

void truncate_statement::validate(distributed<service::storage_proxy>&, const service::client_state& state)
{
    warn(unimplemented::cause::VALIDATION);
#if 0
    ThriftValidation.validateColumnFamily(keyspace(), columnFamily());
#endif
}

future<::shared_ptr<transport::messages::result_message>>
truncate_statement::execute(distributed<service::storage_proxy>& proxy, service::query_state& state, const query_options& options)
{
    return service::get_local_storage_proxy().truncate_blocking(keyspace(), column_family()).handle_exception([](auto ep) {
        throw exceptions::truncate_exception(ep);
    }).then([] {
        return ::shared_ptr<transport::messages::result_message>{};
    });
}

future<::shared_ptr<transport::messages::result_message>>
truncate_statement::execute_internal(distributed<service::storage_proxy>& proxy, service::query_state& state, const query_options& options)
{
    throw std::runtime_error("unsupported operation");
}

}

}
