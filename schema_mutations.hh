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

#pragma once

#include <vector>
#include "mutation.hh"
#include "schema.hh"
#include "canonical_mutation.hh"

// Commutative representation of table schema
class schema_mutations {
    mutation _columnfamilies;
    mutation _columns;
public:
    schema_mutations(mutation columnfamilies, mutation columns)
            : _columnfamilies(std::move(columnfamilies))
            , _columns(std::move(columns))
    { }
    schema_mutations(canonical_mutation columnfamilies, canonical_mutation columns, bool is_view);

    schema_mutations(schema_mutations&&) = default;
    schema_mutations& operator=(schema_mutations&&) = default;
    schema_mutations(const schema_mutations&) = default;
    schema_mutations& operator=(const schema_mutations&) = default;

    void copy_to(std::vector<mutation>& dst) const;

    const mutation& columnfamilies_mutation() const {
        return _columnfamilies;
    }

    const mutation& columns_mutation() const {
        return _columns;
    }

    canonical_mutation columnfamilies_canonical_mutation() const {
        return canonical_mutation(_columnfamilies);
    }

    canonical_mutation columns_canonical_mutation() const {
        return canonical_mutation(_columns);
    }

    bool is_view() const;

    table_schema_version digest() const;

    bool operator==(const schema_mutations&) const;
    bool operator!=(const schema_mutations&) const;

    // Returns true iff any mutations contain any live cells
    bool live() const;
};

