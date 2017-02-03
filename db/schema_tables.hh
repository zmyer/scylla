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
 * Modified by ScyllaDB
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

#include "service/storage_proxy.hh"
#include "mutation.hh"
#include "schema.hh"
#include "hashing.hh"
#include "schema_mutations.hh"

#include <vector>
#include <map>

namespace query {
class result_set;
}

namespace db {
namespace schema_tables {

using schema_result = std::map<sstring, lw_shared_ptr<query::result_set>>;
using schema_result_value_type = std::pair<sstring, lw_shared_ptr<query::result_set>>;

static constexpr auto KEYSPACES = "schema_keyspaces";
static constexpr auto COLUMNFAMILIES = "schema_columnfamilies";
static constexpr auto COLUMNS = "schema_columns";
static constexpr auto TRIGGERS = "schema_triggers";
static constexpr auto USERTYPES = "schema_usertypes";
static constexpr auto FUNCTIONS = "schema_functions";
static constexpr auto AGGREGATES = "schema_aggregates";
static constexpr auto VIEWS = "views";

extern std::vector<const char*> ALL;

std::vector<schema_ptr> all_tables();

future<> save_system_keyspace_schema();

future<utils::UUID> calculate_schema_digest(distributed<service::storage_proxy>& proxy);

future<std::vector<frozen_mutation>> convert_schema_to_mutations(distributed<service::storage_proxy>& proxy);

future<schema_result_value_type>
read_schema_partition_for_keyspace(distributed<service::storage_proxy>& proxy, const sstring& schema_table_name, const sstring& keyspace_name);

future<> merge_schema(distributed<service::storage_proxy>& proxy, std::vector<mutation> mutations);

future<> merge_schema(distributed<service::storage_proxy>& proxy, std::vector<mutation> mutations, bool do_flush);

future<> do_merge_schema(distributed<service::storage_proxy>& proxy, std::vector<mutation> mutations, bool do_flush);

future<std::set<sstring>> merge_keyspaces(distributed<service::storage_proxy>& proxy, schema_result&& before, schema_result&& after);

std::vector<mutation> make_create_keyspace_mutations(lw_shared_ptr<keyspace_metadata> keyspace, api::timestamp_type timestamp, bool with_tables_and_types_and_functions = true);

std::vector<mutation> make_drop_keyspace_mutations(lw_shared_ptr<keyspace_metadata> keyspace, api::timestamp_type timestamp);

lw_shared_ptr<keyspace_metadata> create_keyspace_from_schema_partition(const schema_result_value_type& partition);

std::vector<mutation> make_create_type_mutations(lw_shared_ptr<keyspace_metadata> keyspace, user_type type, api::timestamp_type timestamp);

std::vector<user_type> create_types_from_schema_partition(const schema_result_value_type& result);

std::vector<mutation> make_drop_type_mutations(lw_shared_ptr<keyspace_metadata> keyspace, user_type type, api::timestamp_type timestamp);

void add_type_to_schema_mutation(user_type type, api::timestamp_type timestamp, std::vector<mutation>& mutations);

std::vector<mutation> make_create_table_mutations(lw_shared_ptr<keyspace_metadata> keyspace, schema_ptr table, api::timestamp_type timestamp);

std::vector<mutation> make_update_table_mutations(
    lw_shared_ptr<keyspace_metadata> keyspace,
    schema_ptr old_table,
    schema_ptr new_table,
    api::timestamp_type timestamp,
    bool from_thrift);

future<std::map<sstring, schema_ptr>> create_tables_from_tables_partition(distributed<service::storage_proxy>& proxy, const schema_result::mapped_type& result);

std::vector<mutation> make_drop_table_mutations(lw_shared_ptr<keyspace_metadata> keyspace, schema_ptr table, api::timestamp_type timestamp);

future<schema_ptr> create_table_from_name(distributed<service::storage_proxy>& proxy, const sstring& keyspace, const sstring& table);

future<schema_ptr> create_table_from_table_row(distributed<service::storage_proxy>& proxy, const query::result_set_row& row);

void prepare_builder_from_table_row(schema_builder& builder, const query::result_set_row& table_row);

schema_ptr create_table_from_mutations(schema_mutations, std::experimental::optional<table_schema_version> version = {});

void drop_column_from_schema_mutation(schema_ptr table, const column_definition& column, long timestamp, std::vector<mutation>& mutations);

std::vector<column_definition> create_columns_from_column_rows(const query::result_set& rows,
                                                               const sstring& keyspace,
                                                               const sstring& table,/*,
                                                               AbstractType<?> rawComparator, */
                                                               bool is_super);

column_definition create_column_from_column_row(const query::result_set_row& row,
                                                sstring keyspace,
                                                sstring table, /*,
                                                AbstractType<?> rawComparator, */
                                                bool is_super);


void add_column_to_schema_mutation(schema_ptr table, const column_definition& column, api::timestamp_type timestamp, mutation& mutation);

view_ptr create_view_from_mutations(schema_mutations sm, std::experimental::optional<table_schema_version> version = {});

future<std::vector<view_ptr>> create_views_from_schema_partition(distributed<service::storage_proxy>& proxy, const schema_result::mapped_type& result);

schema_mutations make_schema_mutations(schema_ptr s, api::timestamp_type timestamp, bool with_columns);

void add_table_or_view_to_schema_mutation(schema_ptr view, api::timestamp_type timestamp, bool with_columns, std::vector<mutation>& mutations);

std::vector<mutation> make_create_view_mutations(lw_shared_ptr<keyspace_metadata> keyspace, view_ptr view, api::timestamp_type timestamp);

std::vector<mutation> make_update_view_mutations(lw_shared_ptr<keyspace_metadata> keyspace, view_ptr old_view, view_ptr new_view, api::timestamp_type timestamp);

std::vector<mutation> make_drop_view_mutations(lw_shared_ptr<keyspace_metadata> keyspace, view_ptr view, api::timestamp_type timestamp);

sstring serialize_kind(column_kind kind);
column_kind deserialize_kind(sstring kind);
data_type parse_type(sstring str);

schema_ptr columns();
schema_ptr columnfamilies();
schema_ptr views();

template<typename Hasher>
void feed_hash_for_schema_digest(Hasher& h, const mutation& m) {
    // Cassandra is skipping tombstones from digest calculation
    // to avoid disagreements due to tombstone GC.
    // See https://issues.apache.org/jira/browse/CASSANDRA-6862.
    // We achieve similar effect with compact_for_compaction().
    mutation m_compacted(m);
    m_compacted.partition().compact_for_compaction(*m.schema(), always_gc, gc_clock::time_point::max());
    feed_hash(h, m_compacted);
}

} // namespace schema_tables
} // namespace db
