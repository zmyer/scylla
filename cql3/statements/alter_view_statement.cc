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

#include "cql3/statements/alter_view_statement.hh"
#include "cql3/statements/prepared_statement.hh"
#include "service/migration_manager.hh"
#include "validation.hh"

namespace cql3 {

namespace statements {

alter_view_statement::alter_view_statement(::shared_ptr<cf_name> view_name, ::shared_ptr<cf_prop_defs> properties)
        : schema_altering_statement{std::move(view_name)}
        , _properties{std::move(properties)}
{
}

future<> alter_view_statement::check_access(const service::client_state& state)
{
    try {
        auto&& s = service::get_local_storage_proxy().get_db().local().find_schema(keyspace(), column_family());
        if (s->is_view())  {
            return state.has_column_family_access(keyspace(), s->view_info()->base_name(), auth::permission::ALTER);
        }
    } catch (const no_such_column_family& e) {
        // Will be validated afterwards.
    }
    return make_ready_future<>();
}

void alter_view_statement::validate(distributed<service::storage_proxy>&, const service::client_state& state)
{
    // validated in announce_migration()
}

future<bool> alter_view_statement::announce_migration(distributed<service::storage_proxy>& proxy, bool is_local_only)
{
    auto&& db = proxy.local().get_db().local();
    schema_ptr schema = validation::validate_column_family(db, keyspace(), column_family());
    if (!schema->is_view()) {
        throw exceptions::invalid_request_exception("Cannot use ALTER MATERIALIZED VIEW on Table");
    }

    if (!_properties) {
        throw exceptions::invalid_request_exception("ALTER MATERIALIZED VIEW WITH invoked, but no parameters found");
    }

    _properties->validate();

    auto builder = schema_builder(schema);
    _properties->apply_to_builder(builder);

    if (builder.get_gc_grace_seconds() == 0) {
        throw exceptions::invalid_request_exception(
                "Cannot alter gc_grace_seconds of a materialized view to 0, since this "
                "value is used to TTL undelivered updates. Setting gc_grace_seconds too "
                "low might cause undelivered updates to expire before being replayed.");
    }

    return service::get_local_migration_manager().announce_view_update(view_ptr(builder.build()), is_local_only).then([] {
        return true;
    });
}

shared_ptr<transport::event::schema_change> alter_view_statement::change_event()
{
    using namespace transport;

    return make_shared<event::schema_change>(event::schema_change::change_type::UPDATED,
                                             event::schema_change::target_type::TABLE,
                                             keyspace(),
                                             column_family());
}

shared_ptr<cql3::statements::prepared_statement>
alter_view_statement::prepare(database& db, cql_stats& stats) {
    return make_shared<prepared_statement>(make_shared<alter_view_statement>(*this));
}

}

}
