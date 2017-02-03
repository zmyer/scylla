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
 * Copyright (C) 2015 ScyllaDB
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

#include "db/consistency_level_type.hh"
#include "db/write_type.hh"
#include <stdexcept>
#include "core/sstring.hh"
#include "core/print.hh"
#include "bytes.hh"

namespace exceptions {

enum class exception_code : int32_t {
    SERVER_ERROR    = 0x0000,
    PROTOCOL_ERROR  = 0x000A,

    BAD_CREDENTIALS = 0x0100,

    // 1xx: problem during request execution
    UNAVAILABLE     = 0x1000,
    OVERLOADED      = 0x1001,
    IS_BOOTSTRAPPING= 0x1002,
    TRUNCATE_ERROR  = 0x1003,
    WRITE_TIMEOUT   = 0x1100,
    READ_TIMEOUT    = 0x1200,

    // 2xx: problem validating the request
    SYNTAX_ERROR    = 0x2000,
    UNAUTHORIZED    = 0x2100,
    INVALID         = 0x2200,
    CONFIG_ERROR    = 0x2300,
    ALREADY_EXISTS  = 0x2400,
    UNPREPARED      = 0x2500
};

class cassandra_exception : public std::exception {
private:
    exception_code _code;
    sstring _msg;
protected:
    template<typename... Args>
    static inline sstring prepare_message(const char* fmt, Args&&... args) noexcept {
        try {
            return sprint(fmt, std::forward<Args>(args)...);
        } catch (...) {
            return sstring();
        }
    }
public:
    cassandra_exception(exception_code code, sstring msg) noexcept
        : _code(code)
        , _msg(std::move(msg))
    { }
    virtual const char* what() const noexcept override { return _msg.begin(); }
    exception_code code() const { return _code; }
    sstring get_message() const { return what(); }
};

class protocol_exception : public cassandra_exception {
public:
    protocol_exception(sstring msg) noexcept
        : exceptions::cassandra_exception{exceptions::exception_code::PROTOCOL_ERROR, std::move(msg)}
    { }
};

struct unavailable_exception : cassandra_exception {
    db::consistency_level consistency;
    int32_t required;
    int32_t alive;

    unavailable_exception(db::consistency_level cl, int32_t required, int32_t alive) noexcept
        : exceptions::cassandra_exception(exceptions::exception_code::UNAVAILABLE, prepare_message("Cannot achieve consistency level for cl %s. Requires %ld, alive %ld", cl, required, alive))
        , consistency(cl)
        , required(required)
        , alive(alive)
    {}
};

class request_execution_exception : public cassandra_exception {
public:
    request_execution_exception(exception_code code, sstring msg) noexcept
        : cassandra_exception(code, std::move(msg))
    { }
};

class truncate_exception : public request_execution_exception
{
public:
    truncate_exception(std::exception_ptr ep);
};

class request_timeout_exception : public cassandra_exception {
public:
    db::consistency_level consistency;
    int32_t received;
    int32_t block_for;

    request_timeout_exception(exception_code code, const sstring& ks, const sstring& cf, db::consistency_level consistency, int32_t received, int32_t block_for) noexcept
        : cassandra_exception{code, prepare_message("Operation timed out for %s.%s - received only %d responses from %d CL=%s.", ks, cf, received, block_for, consistency)}
        , consistency{consistency}
        , received{received}
        , block_for{block_for}
    { }
};

class read_timeout_exception : public request_timeout_exception {
public:
    bool data_present;

    read_timeout_exception(const sstring& ks, const sstring& cf, db::consistency_level consistency, int32_t received, int32_t block_for, bool data_present) noexcept
        : request_timeout_exception{exception_code::READ_TIMEOUT, ks, cf, consistency, received, block_for}
        , data_present{data_present}
    { }
};

struct mutation_write_timeout_exception : public request_timeout_exception {
    db::write_type type;
    mutation_write_timeout_exception(const sstring& ks, const sstring& cf, db::consistency_level consistency, int32_t received, int32_t block_for, db::write_type type) noexcept :
        request_timeout_exception(exception_code::WRITE_TIMEOUT, ks, cf, consistency, received, block_for)
        , type{std::move(type)}
    { }
};

struct overloaded_exception : public cassandra_exception {
    overloaded_exception(size_t c) noexcept :
        cassandra_exception(exception_code::OVERLOADED, prepare_message("Too many in flight hints: %lu", c)) {}
};

class request_validation_exception : public cassandra_exception {
public:
    using cassandra_exception::cassandra_exception;
};

class unauthorized_exception: public request_validation_exception {
public:
    unauthorized_exception(sstring msg) noexcept
                    : request_validation_exception(exception_code::UNAUTHORIZED,
                                    std::move(msg)) {
    }
};

class authentication_exception: public request_validation_exception {
public:
    authentication_exception(sstring msg) noexcept
                    : request_validation_exception(exception_code::BAD_CREDENTIALS,
                                    std::move(msg)) {
    }
};

class invalid_request_exception : public request_validation_exception {
public:
    invalid_request_exception(sstring cause) noexcept
        : request_validation_exception(exception_code::INVALID, std::move(cause))
    { }
};

class keyspace_not_defined_exception : public invalid_request_exception {
public:
    keyspace_not_defined_exception(std::string cause) noexcept
        : invalid_request_exception(std::move(cause))
    { }
};

class prepared_query_not_found_exception : public request_validation_exception {
public:
    bytes id;

    prepared_query_not_found_exception(bytes id) noexcept
        : request_validation_exception{exception_code::UNPREPARED, prepare_message("No prepared statement with ID %s found.", id)}
        , id{id}
    { }
};

class syntax_exception : public request_validation_exception {
public:
    syntax_exception(sstring msg) noexcept
        : request_validation_exception(exception_code::SYNTAX_ERROR, std::move(msg))
    { }
};

class configuration_exception : public request_validation_exception {
public:
    configuration_exception(sstring msg) noexcept
        : request_validation_exception{exception_code::CONFIG_ERROR, std::move(msg)}
    { }

    configuration_exception(exception_code code, sstring msg) noexcept
        : request_validation_exception{code, std::move(msg)}
    { }
};

class already_exists_exception : public configuration_exception {
public:
    const sstring ks_name;
    const sstring cf_name;
private:
    already_exists_exception(sstring ks_name_, sstring cf_name_, sstring msg)
        : configuration_exception{exception_code::ALREADY_EXISTS, msg}
        , ks_name{ks_name_}
        , cf_name{cf_name_}
    { }
public:
    already_exists_exception(sstring ks_name_, sstring cf_name_)
        : already_exists_exception{ks_name_, cf_name_, sprint("Cannot add already existing table \"%s\" to keyspace \"%s\"", cf_name_, ks_name_)}
    { }

    already_exists_exception(sstring ks_name_)
        : already_exists_exception{ks_name_, "", sprint("Cannot add existing keyspace \"%s\"", ks_name_)}
    { }
};

class recognition_exception : public std::runtime_error {
public:
    recognition_exception(const std::string& msg) : std::runtime_error(msg) {};
};

class unsupported_operation_exception : public std::runtime_error {
public:
    unsupported_operation_exception() : std::runtime_error("unsupported operation") {}
    unsupported_operation_exception(const sstring& msg) : std::runtime_error("unsupported operation: " + msg) {}
};

}
