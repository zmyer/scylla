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

#pragma once

#include <seastar/core/shared_ptr.hh>

#include "cql3/statements/schema_altering_statement.hh"
#include "cql3/cf_name.hh"

#include "database.hh"

namespace cql3 {

namespace statements {

/** A <code>DROP MATERIALIZED VIEW</code> parsed from a CQL query statement. */
class drop_view_statement : public schema_altering_statement {
private:
    bool _if_exists;
public:
    drop_view_statement(::shared_ptr<cf_name> view_name, bool if_exists);

    virtual future<> check_access(const service::client_state& state) override;

    virtual void validate(distributed<service::storage_proxy>&, const service::client_state& state) override;

    virtual future<bool> announce_migration(distributed<service::storage_proxy>& proxy, bool is_local_only) override;

    virtual shared_ptr<transport::event::schema_change> change_event() override;

    virtual shared_ptr<prepared> prepare(database& db, cql_stats& stats) override;
};

}
}
