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
 * Copyright 2016 ScyllaDB
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

#include "authorization_statement.hh"
#include "transport/messages/result_message.hh"

uint32_t cql3::statements::authorization_statement::get_bound_terms() {
    return 0;
}

::shared_ptr<cql3::statements::prepared_statement> cql3::statements::authorization_statement::prepare(
                database& db, cql_stats& stats) {
    return ::make_shared<parsed_statement::prepared>(this->shared_from_this());
}

bool cql3::statements::authorization_statement::uses_function(
                const sstring& ks_name, const sstring& function_name) const {
    return parsed_statement::uses_function(ks_name, function_name);
}

bool cql3::statements::authorization_statement::depends_on_keyspace(
                const sstring& ks_name) const {
    return false;
}

bool cql3::statements::authorization_statement::depends_on_column_family(
                const sstring& cf_name) const {
    return false;
}

void cql3::statements::authorization_statement::validate(
                distributed<service::storage_proxy>&,
                const service::client_state& state) {
}

future<> cql3::statements::authorization_statement::check_access(const service::client_state& state) {
    return make_ready_future<>();
}

future<::shared_ptr<transport::messages::result_message>> cql3::statements::authorization_statement::execute_internal(
                distributed<service::storage_proxy>& proxy,
                service::query_state& state, const query_options& options) {
    // Internal queries are exclusively on the system keyspace and makes no sense here
    throw std::runtime_error("unsupported operation");
}

void cql3::statements::authorization_statement::mayme_correct_resource(auth::data_resource& resource, const service::client_state& state) {
    if (resource.is_column_family_level() && resource.keyspace().empty()) {
        resource = auth::data_resource(state.get_keyspace(), resource.column_family());
    }
}

