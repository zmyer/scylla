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

#include "mutation_reader.hh"

using populate_fn = std::function<mutation_source(schema_ptr s, const std::vector<mutation>&)>;

// Must be run in a seastar thread
void run_mutation_source_tests(populate_fn populate);

enum are_equal { no, yes };

// Calls the provided function on mutation pairs, equal and not equal. Is supposed
// to exercise all potential ways two mutations may differ.
void for_each_mutation_pair(std::function<void(const mutation&, const mutation&, are_equal)>);

// Calls the provided function on mutations. Is supposed to exercise as many differences as possible.
void for_each_mutation(std::function<void(const mutation&)>);

class random_mutation_generator {
    class impl;
    std::unique_ptr<impl> _impl;
public:
    random_mutation_generator();
    ~random_mutation_generator();
    mutation operator()();
    schema_ptr schema() const;
};

bytes make_blob(size_t blob_size);
