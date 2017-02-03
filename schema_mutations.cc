/*
 * Copyright 2015 ScyllaDB
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

#include "schema_mutations.hh"
#include "canonical_mutation.hh"
#include "db/schema_tables.hh"
#include "md5_hasher.hh"

schema_mutations::schema_mutations(canonical_mutation columnfamilies, canonical_mutation columns, bool is_view)
    : _columnfamilies(columnfamilies.to_mutation(is_view ? db::schema_tables::views() : db::schema_tables::columnfamilies()))
      , _columns(columns.to_mutation(db::schema_tables::columns()))
{
}

void schema_mutations::copy_to(std::vector<mutation>& dst) const {
    dst.push_back(_columnfamilies);
    dst.push_back(_columns);
}

table_schema_version schema_mutations::digest() const {
    md5_hasher h;
    db::schema_tables::feed_hash_for_schema_digest(h, _columnfamilies);
    db::schema_tables::feed_hash_for_schema_digest(h, _columns);
    return utils::UUID_gen::get_name_UUID(h.finalize());
}

bool schema_mutations::operator==(const schema_mutations& other) const {
    return _columnfamilies == other._columnfamilies
           && _columns == other._columns;
}

bool schema_mutations::operator!=(const schema_mutations& other) const {
    return !(*this == other);
}

bool schema_mutations::live() const {
    return _columnfamilies.live_row_count() > 0 || _columns.live_row_count() > 0;
}

bool schema_mutations::is_view() const {
    return _columnfamilies.schema() == db::schema_tables::views();
}