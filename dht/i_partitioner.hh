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

#include "core/shared_ptr.hh"
#include "core/sstring.hh"
#include "types.hh"
#include "keys.hh"
#include "utils/managed_bytes.hh"
#include "stdx.hh"
#include <memory>
#include <random>
#include <utility>
#include <vector>
#include <range.hh>

namespace sstables {

class key_view;

}

namespace dht {

//
// Origin uses a complex class hierarchy where Token is an abstract class,
// and various subclasses use different implementations (LongToken vs.
// BigIntegerToken vs. StringToken), plus other variants to to signify the
// the beginning of the token space etc.
//
// We'll fold all of that into the token class and push all of the variations
// into its users.

class decorated_key;
class token;
class ring_position;

using partition_range = nonwrapping_range<ring_position>;
using token_range = nonwrapping_range<token>;

using partition_range_vector = std::vector<partition_range>;
using token_range_vector = std::vector<token_range>;

class token {
public:
    enum class kind {
        before_all_keys,
        key,
        after_all_keys,
    };
    kind _kind;
    // _data can be interpreted as a big endian binary fraction
    // in the range [0.0, 1.0).
    //
    // So, [] == 0.0
    //     [0x00] == 0.0
    //     [0x80] == 0.5
    //     [0x00, 0x80] == 1/512
    //     [0xff, 0x80] == 1 - 1/512
    managed_bytes _data;

    token() : _kind(kind::before_all_keys) {
    }

    token(kind k, managed_bytes d) : _kind(std::move(k)), _data(std::move(d)) {
    }

    bool is_minimum() const {
        return _kind == kind::before_all_keys;
    }

    bool is_maximum() const {
        return _kind == kind::after_all_keys;
    }
};

token midpoint_unsigned(const token& t1, const token& t2);
token minimum_token();
token maximum_token();
bool operator==(const token& t1, const token& t2);
bool operator<(const token& t1, const token& t2);
int tri_compare(const token& t1, const token& t2);
inline bool operator!=(const token& t1, const token& t2) { return std::rel_ops::operator!=(t1, t2); }
inline bool operator>(const token& t1, const token& t2) { return std::rel_ops::operator>(t1, t2); }
inline bool operator<=(const token& t1, const token& t2) { return std::rel_ops::operator<=(t1, t2); }
inline bool operator>=(const token& t1, const token& t2) { return std::rel_ops::operator>=(t1, t2); }
std::ostream& operator<<(std::ostream& out, const token& t);

template <typename T>
inline auto get_random_number() {
    static thread_local std::random_device rd;
    static thread_local std::default_random_engine re(rd());
    static thread_local std::uniform_int_distribution<T> dist{};
    return dist(re);
}

// Wraps partition_key with its corresponding token.
//
// Total ordering defined by comparators is compatible with Origin's ordering.
class decorated_key {
public:
    dht::token _token;
    partition_key _key;

    struct less_comparator {
        schema_ptr s;
        less_comparator(schema_ptr s);
        bool operator()(const decorated_key& k1, const decorated_key& k2) const;
        bool operator()(const decorated_key& k1, const ring_position& k2) const;
        bool operator()(const ring_position& k1, const decorated_key& k2) const;
    };

    bool equal(const schema& s, const decorated_key& other) const;

    bool less_compare(const schema& s, const decorated_key& other) const;
    bool less_compare(const schema& s, const ring_position& other) const;

    // Trichotomic comparators defining total ordering on the union of
    // decorated_key and ring_position objects.
    int tri_compare(const schema& s, const decorated_key& other) const;
    int tri_compare(const schema& s, const ring_position& other) const;

    const dht::token& token() const {
        return _token;
    }

    const partition_key& key() const {
        return _key;
    }
};


class decorated_key_equals_comparator {
    const schema& _schema;
public:
    explicit decorated_key_equals_comparator(const schema& schema) : _schema(schema) {}
    bool operator()(const dht::decorated_key& k1, const dht::decorated_key& k2) const {
        return k1.equal(_schema, k2);
    }
};

using decorated_key_opt = std::experimental::optional<decorated_key>;

class i_partitioner {
public:
    virtual ~i_partitioner() {}

    /**
     * Transform key to object representation of the on-disk format.
     *
     * @param key the raw, client-facing key
     * @return decorated version of key
     */
    decorated_key decorate_key(const schema& s, const partition_key& key) {
        return { get_token(s, key), key };
    }

    /**
     * Transform key to object representation of the on-disk format.
     *
     * @param key the raw, client-facing key
     * @return decorated version of key
     */
    decorated_key decorate_key(const schema& s, partition_key&& key) {
        auto token = get_token(s, key);
        return { std::move(token), std::move(key) };
    }

    /**
     * Calculate a token representing the approximate "middle" of the given
     * range.
     *
     * @return The approximate midpoint between left and right.
     */
    virtual token midpoint(const token& left, const token& right) const = 0;

    /**
     * @return A token smaller than all others in the range that is being partitioned.
     * Not legal to assign to a node or key.  (But legal to use in range scans.)
     */
    token get_minimum_token() {
        return dht::minimum_token();
    }

    /**
     * @return a token that can be used to route a given key
     * (This is NOT a method to create a token from its string representation;
     * for that, use tokenFactory.fromString.)
     */
    virtual token get_token(const schema& s, partition_key_view key) = 0;
    virtual token get_token(const sstables::key_view& key) = 0;


    /**
     * @return a partitioner-specific string representation of this token
     */
    virtual sstring to_sstring(const dht::token& t) const = 0;

    /**
     * @return a token from its partitioner-specific string representation
     */
    virtual dht::token from_sstring(const sstring& t) const = 0;

    /**
     * @return a token from its partitioner-specific byte representation
     */
    virtual dht::token from_bytes(bytes_view bytes) const = 0;

    /**
     * @return a randomly generated token
     */
    virtual token get_random_token() = 0;

    // FIXME: token.tokenFactory
    //virtual token.tokenFactory gettokenFactory() = 0;

    /**
     * @return True if the implementing class preserves key order in the tokens
     * it generates.
     */
    virtual bool preserves_order() = 0;

    /**
     * Calculate the deltas between tokens in the ring in order to compare
     *  relative sizes.
     *
     * @param sortedtokens a sorted List of tokens
     * @return the mapping from 'token' to 'percentage of the ring owned by that token'.
     */
    virtual std::map<token, float> describe_ownership(const std::vector<token>& sorted_tokens) = 0;

    virtual data_type get_token_validator() = 0;

    /**
     * @return name of partitioner.
     */
    virtual const sstring name() = 0;

    /**
     * Calculates the shard that handles a particular token.
     */
    virtual unsigned shard_of(const token& t) const = 0;

    /**
     * Gets the first token greater than `t` that is not in the same shard as `t`.
     */
    virtual token token_for_next_shard(const token& t) const = 0;

    /**
     * Gets the first shard of the minimum token.
     */
    unsigned shard_of_minimum_token() const {
        return 0;  // hardcoded for now; unlikely to change
    }

    /**
     * @return bytes that represent the token as required by get_token_validator().
     */
    virtual bytes token_to_bytes(const token& t) const {
        return bytes(t._data.begin(), t._data.end());
    }

    /**
     * @return < 0 if if t1's _data array is less, t2's. 0 if they are equal, and > 0 otherwise. _kind comparison should be done separately.
     */
    virtual int tri_compare(const token& t1, const token& t2) const = 0;
    /**
     * @return true if t1's _data array is equal t2's. _kind comparison should be done separately.
     */
    bool is_equal(const token& t1, const token& t2) const {
        return tri_compare(t1, t2) == 0;
    }
    /**
     * @return true if t1's _data array is less then t2's. _kind comparison should be done separately.
     */
    bool is_less(const token& t1, const token& t2) const {
        return tri_compare(t1, t2) < 0;
    }

    friend bool operator==(const token& t1, const token& t2);
    friend bool operator<(const token& t1, const token& t2);
    friend int tri_compare(const token& t1, const token& t2);
};

//
// Represents position in the ring of partitions, where partitions are ordered
// according to decorated_key ordering (first by token, then by key value).
// Intended to be used for defining partition ranges.
//
// The 'key' part is optional. When it's absent, this object represents a position
// which is either before or after all keys sharing given token. That's determined
// by relation_to_keys().
//
// For example for the following data:
//
//   tokens: |    t1   | t2 |
//           +----+----+----+
//   keys:   | k1 | k2 | k3 |
//
// The ordering is:
//
//   ring_position(t1, token_bound::start) < ring_position(k1)
//   ring_position(k1)                     < ring_position(k2)
//   ring_position(k1)                     == decorated_key(k1)
//   ring_position(k2)                     == decorated_key(k2)
//   ring_position(k2)                     < ring_position(t1, token_bound::end)
//   ring_position(k2)                     < ring_position(k3)
//   ring_position(t1, token_bound::end)   < ring_position(t2, token_bound::start)
//
// Maps to org.apache.cassandra.db.RowPosition and its derivatives in Origin.
//
class ring_position {
public:
    enum class token_bound : int8_t { start = -1, end = 1 };
private:
    dht::token _token;
    token_bound _token_bound; // valid when !_key
    std::experimental::optional<partition_key> _key;
public:
    static ring_position starting_at(dht::token token) {
        return { std::move(token), token_bound::start };
    }

    static ring_position ending_at(dht::token token) {
        return { std::move(token), token_bound::end };
    }

    ring_position(dht::token token, token_bound bound)
        : _token(std::move(token))
        , _token_bound(bound)
    { }

    ring_position(dht::token token, partition_key key)
        : _token(std::move(token))
        , _key(std::experimental::make_optional(std::move(key)))
    { }

    ring_position(dht::token token, token_bound bound, std::experimental::optional<partition_key> key)
        : _token(std::move(token))
        , _token_bound(bound)
        , _key(std::move(key))
    { }

    ring_position(const dht::decorated_key& dk)
        : _token(dk._token)
        , _key(std::experimental::make_optional(dk._key))
    { }

    const dht::token& token() const {
        return _token;
    }

    // Valid when !has_key()
    token_bound bound() const {
        return _token_bound;
    }

    // Returns -1 if smaller than keys with the same token, +1 if greater.
    int relation_to_keys() const {
        return _key ? 0 : static_cast<int>(_token_bound);
    }

    const std::experimental::optional<partition_key>& key() const {
        return _key;
    }

    bool has_key() const {
        return bool(_key);
    }

    // Call only when has_key()
    dht::decorated_key as_decorated_key() const {
        return { _token, *_key };
    }

    bool equal(const schema&, const ring_position&) const;

    // Trichotomic comparator defining a total ordering on ring_position objects
    int tri_compare(const schema&, const ring_position&) const;

    // "less" comparator corresponding to tri_compare()
    bool less_compare(const schema&, const ring_position&) const;

    friend std::ostream& operator<<(std::ostream&, const ring_position&);
};

// Trichotomic comparator for ring_position
struct ring_position_comparator {
    const schema& s;
    ring_position_comparator(const schema& s_) : s(s_) {}
    int operator()(const ring_position& lh, const ring_position& rh) const;
};

// "less" comparator for ring_position
struct ring_position_less_comparator {
    const schema& s;
    ring_position_less_comparator(const schema& s_) : s(s_) {}
    bool operator()(const ring_position& lh, const ring_position& rh) const {
        return lh.less_compare(s, rh);
    }
};

struct token_comparator {
    // Return values are those of a trichotomic comparison.
    int operator()(const token& t1, const token& t2) const;
};

std::ostream& operator<<(std::ostream& out, const token& t);

std::ostream& operator<<(std::ostream& out, const decorated_key& t);

void set_global_partitioner(const sstring& class_name, unsigned ignore_msb = 0);
i_partitioner& global_partitioner();

unsigned shard_of(const token&);

struct ring_position_range_and_shard {
    dht::partition_range ring_range;
    unsigned shard;
};

class ring_position_range_sharder {
    const i_partitioner& _partitioner;
    dht::partition_range _range;
    bool _done = false;
public:
    explicit ring_position_range_sharder(nonwrapping_range<ring_position> rrp)
            : ring_position_range_sharder(global_partitioner(), std::move(rrp)) {}
    ring_position_range_sharder(const i_partitioner& partitioner, nonwrapping_range<ring_position> rrp)
            : _partitioner(partitioner), _range(std::move(rrp)) {}
    stdx::optional<ring_position_range_and_shard> next(const schema& s);
};

struct ring_position_range_and_shard_and_element : ring_position_range_and_shard {
    ring_position_range_and_shard_and_element(ring_position_range_and_shard&& rpras, unsigned element)
            : ring_position_range_and_shard(std::move(rpras)), element(element) {
    }
    unsigned element;
};

class ring_position_range_vector_sharder {
    using vec_type = dht::partition_range_vector;
    vec_type _ranges;
    vec_type::iterator _current_range;
    stdx::optional<ring_position_range_sharder> _current_sharder;
private:
    void next_range() {
        if (_current_range != _ranges.end()) {
            _current_sharder.emplace(std::move(*_current_range++));
        }
    }
public:
    explicit ring_position_range_vector_sharder(dht::partition_range_vector ranges);
    // results are returned sorted by index within the vector first, then within each vector item
    stdx::optional<ring_position_range_and_shard_and_element> next(const schema& s);
};

dht::partition_range to_partition_range(dht::token_range);

// Each shard gets a sorted, disjoint vector of ranges
std::map<unsigned, dht::partition_range_vector>
split_range_to_shards(dht::partition_range pr, const schema& s);

// If input ranges are sorted and disjoint then the ranges for each shard
// are also sorted and disjoint.
std::map<unsigned, dht::partition_range_vector>
split_ranges_to_shards(const dht::token_range_vector& ranges, const schema& s);

} // dht

namespace std {
template<>
struct hash<dht::token> {
    size_t operator()(const dht::token& t) const {
        size_t ret = 0;
        const auto& b = t._data;
        if (b.size() <= sizeof(ret)) { // practically always
            std::copy_n(b.data(), b.size(), reinterpret_cast<int8_t*>(&ret));
        } else {
            ret = hash_large_token(b);
        }
        return ret;
    }
private:
    size_t hash_large_token(const managed_bytes& b) const;
};

template <>
struct hash<dht::decorated_key> {
    size_t operator()(const dht::decorated_key& k) const {
        auto h_token = hash<dht::token>();
        return h_token(k.token());
    }
};


}


