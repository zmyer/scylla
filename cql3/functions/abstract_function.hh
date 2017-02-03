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

#pragma once

#include "types.hh"
#include <vector>
#include <iosfwd>
#include <boost/functional/hash.hpp>

namespace cql3 {

namespace functions {

/**
 * Base class for our native/hardcoded functions.
 */
class abstract_function : public virtual function {
protected:
    function_name _name;
    std::vector<data_type> _arg_types;
    data_type _return_type;

    abstract_function(function_name name, std::vector<data_type> arg_types, data_type return_type)
            : _name(std::move(name)), _arg_types(std::move(arg_types)), _return_type(std::move(return_type)) {
    }

public:
    virtual const function_name& name() const override {
        return _name;
    }

    virtual const std::vector<data_type>&  arg_types() const override {
        return _arg_types;
    }

    virtual data_type return_type() const {
        return _return_type;
    }

    bool operator==(const abstract_function& x) const {
        return _name == x._name
            && _arg_types == x._arg_types
            && _return_type == x._return_type;
    }

    virtual bool uses_function(const sstring& ks_name, const sstring& function_name) override {
        return _name.keyspace == ks_name && _name.name == function_name;
    }

    virtual bool has_reference_to(function& f) override {
        return false;
    }

    virtual void print(std::ostream& os) const override;
};

inline
void
abstract_function::print(std::ostream& os) const {
    os << _name << " : (";
    for (size_t i = 0; i < _arg_types.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << _arg_types[i]->name(); // FIXME: asCQL3Type()
    }
    os << ") -> " << _return_type->name(); // FIXME: asCQL3Type()
}

}
}

namespace std {

template <>
struct hash<cql3::functions::abstract_function> {
    size_t operator()(const cql3::functions::abstract_function& f) const {
        using namespace cql3::functions;
        size_t v = 0;
        boost::hash_combine(v, std::hash<function_name>()(f.name()));
        boost::hash_combine(v, boost::hash_value(f.arg_types()));
        // FIXME: type hash
        //boost::hash_combine(v, std::hash<shared_ptr<abstract_type>>()(f.return_type()));
        return v;
    }
};

}
