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

#include "cql3/statements/drop_type_statement.hh"
#include "cql3/statements/prepared_statement.hh"

#include "boost/range/adaptor/map.hpp"

#include "service/migration_manager.hh"

namespace cql3 {

namespace statements {

drop_type_statement::drop_type_statement(const ut_name& name, bool if_exists)
    : _name{name}
    , _if_exists{if_exists}
{
}

void drop_type_statement::prepare_keyspace(const service::client_state& state)
{
    if (!_name.has_keyspace()) {
        _name.set_keyspace(state.get_keyspace());
    }
}

future<> drop_type_statement::check_access(const service::client_state& state)
{
    return state.has_keyspace_access(keyspace(), auth::permission::DROP);
}

void drop_type_statement::validate(distributed<service::storage_proxy>& proxy, const service::client_state& state)
{
    try {
        auto&& ks = proxy.local().get_db().local().find_keyspace(keyspace());
        auto&& all_types = ks.metadata()->user_types()->get_all_types();
        auto old = all_types.find(_name.get_user_type_name());
        if (old == all_types.end()) {
            if (_if_exists) {
                return;
            } else {
                throw exceptions::invalid_request_exception(sprint("No user type named %s exists.", _name.to_string()));
            }
        }
    } catch (no_such_keyspace& e) {
        throw exceptions::invalid_request_exception(sprint("Cannot drop type in unknown keyspace %s", keyspace()));
    }
}

shared_ptr<transport::event::schema_change> drop_type_statement::change_event()
{
    using namespace transport;

    return make_shared<transport::event::schema_change>(event::schema_change::change_type::DROPPED,
                                                        event::schema_change::target_type::TYPE,
                                                        keyspace(),
                                                        _name.get_string_type_name());
}

const sstring& drop_type_statement::keyspace() const
{
    return _name.get_keyspace();
}

future<bool> drop_type_statement::announce_migration(distributed<service::storage_proxy>& proxy, bool is_local_only)
{
    auto&& db = proxy.local().get_db().local();

    // Keyspace exists or we wouldn't have validated otherwise
    auto&& ks = db.find_keyspace(keyspace());

    auto&& all_types = ks.metadata()->user_types()->get_all_types();
    auto to_drop = all_types.find(_name.get_user_type_name());

    // Can happen with if_exists
    if (to_drop == all_types.end()) {
        return make_ready_future<bool>(false);
    }

    return service::get_local_migration_manager().announce_type_drop(to_drop->second, is_local_only).then([] { return true; });
}

shared_ptr<cql3::statements::prepared_statement>
drop_type_statement::prepare(database& db, cql_stats& stats) {
    return make_shared<prepared_statement>(make_shared<drop_type_statement>(*this));
}

}

}
