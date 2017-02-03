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

#include "i_partitioner.hh"
#include "core/reactor.hh"
#include "murmur3_partitioner.hh"
#include "utils/class_registrator.hh"
#include "types.hh"
#include "utils/murmur_hash.hh"
#include <boost/range/adaptor/map.hpp>
#include <boost/range/irange.hpp>
#include <boost/range/adaptor/transformed.hpp>

namespace dht {

token
minimum_token() {
    return { token::kind::before_all_keys, {} };
}

token
maximum_token() {
    return { token::kind::after_all_keys, {} };
}

// result + overflow bit
std::pair<bytes, bool>
add_bytes(bytes_view b1, bytes_view b2, bool carry = false) {
    auto sz = std::max(b1.size(), b2.size());
    auto expand = [sz] (bytes_view b) {
        bytes ret(bytes::initialized_later(), sz);
        auto bsz = b.size();
        auto p = std::copy(b.begin(), b.end(), ret.begin());
        std::fill_n(p, sz - bsz, 0);
        return ret;
    };
    auto eb1 = expand(b1);
    auto eb2 = expand(b2);
    auto p1 = eb1.begin();
    auto p2 = eb2.begin();
    unsigned tmp = carry;
    for (size_t idx = 0; idx < sz; ++idx) {
        tmp += uint8_t(p1[sz - idx - 1]);
        tmp += uint8_t(p2[sz - idx - 1]);
        p1[sz - idx - 1] = tmp;
        tmp >>= std::numeric_limits<uint8_t>::digits;
    }
    return { std::move(eb1), bool(tmp) };
}

bytes
shift_right(bool carry, bytes b) {
    unsigned tmp = carry;
    auto sz = b.size();
    auto p = b.begin();
    for (size_t i = 0; i < sz; ++i) {
        auto lsb = p[i] & 1;
        p[i] = (tmp << std::numeric_limits<uint8_t>::digits) | uint8_t(p[i]) >> 1;
        tmp = lsb;
    }
    return b;
}

token
midpoint_unsigned_tokens(const token& t1, const token& t2) {
    // calculate the average of the two tokens.
    // before_all_keys is implicit 0, after_all_keys is implicit 1.
    bool c1 = t1._kind == token::kind::after_all_keys;
    bool c2 = t1._kind == token::kind::after_all_keys;
    if (c1 && c2) {
        // both end-of-range tokens?
        return t1;
    }
    // we can ignore beginning-of-range, since their representation is 0.0
    auto sum_carry = add_bytes(t1._data, t2._data);
    auto& sum = sum_carry.first;
    // if either was end-of-range, we added 0.0, so pretend we added 1.0 and
    // and got a carry:
    bool carry = sum_carry.second || c1 || c2;
    auto avg = shift_right(carry, std::move(sum));
    if (t1 > t2) {
        // wrap around the ring.  We really want (t1 + (t2 + 1.0)) / 2, so add 0.5.
        // example: midpoint(0.9, 0.2) == midpoint(0.9, 1.2) == 1.05 == 0.05
        //                             == (0.9 + 0.2) / 2 + 0.5 (mod 1)
        if (avg.size() > 0) {
            avg[0] ^= 0x80;
        }
    }
    return token{token::kind::key, std::move(avg)};
}

static inline unsigned char get_byte(bytes_view b, size_t off) {
    if (off < b.size()) {
        return b[off];
    } else {
        return 0;
    }
}

int tri_compare(const token& t1, const token& t2) {
    if (t1._kind == t2._kind) {
        return global_partitioner().tri_compare(t1, t2);
    } else if (t1._kind < t2._kind) {
        return -1;
    }
    return 1;
}

bool operator==(const token& t1, const token& t2)
{
    if (t1._kind != t2._kind) {
        return false;
    } else if (t1._kind == token::kind::key) {
        return global_partitioner().is_equal(t1, t2);
    }
    return true;
}

bool operator<(const token& t1, const token& t2)
{
    if (t1._kind < t2._kind) {
        return true;
    } else if (t1._kind == token::kind::key && t2._kind == token::kind::key) {
        return global_partitioner().is_less(t1, t2);
    }
    return false;
}

std::ostream& operator<<(std::ostream& out, const token& t) {
    if (t._kind == token::kind::after_all_keys) {
        out << "maximum token";
    } else if (t._kind == token::kind::before_all_keys) {
        out << "minimum token";
    } else {
        out << global_partitioner().to_sstring(t);
    }
    return out;
}

std::ostream& operator<<(std::ostream& out, const decorated_key& dk) {
    return out << "{key: " << dk._key << ", token:" << dk._token << "}";
}

// FIXME: make it per-keyspace
std::unique_ptr<i_partitioner> default_partitioner { new murmur3_partitioner };

void set_global_partitioner(const sstring& class_name, unsigned ignore_msb)
{
    try {
        default_partitioner = create_object<i_partitioner, const unsigned&, const unsigned&>(class_name, smp::count, ignore_msb);
    } catch (std::exception& e) {
        auto supported_partitioners = ::join(", ", class_registry<i_partitioner>::classes() |
                boost::adaptors::map_keys);
        throw std::runtime_error(sprint("Partitioner %s is not supported, supported partitioners = { %s } : %s",
                class_name, supported_partitioners, e.what()));
    }
}

i_partitioner&
global_partitioner() {
    return *default_partitioner;
}

bool
decorated_key::equal(const schema& s, const decorated_key& other) const {
    if (_token == other._token) {
        return _key.legacy_equal(s, other._key);
    }
    return false;
}

int
decorated_key::tri_compare(const schema& s, const decorated_key& other) const {
    auto r = dht::tri_compare(_token, other._token);
    if (r != 0) {
        return r;
    } else {
        return _key.legacy_tri_compare(s, other._key);
    }
}

int
decorated_key::tri_compare(const schema& s, const ring_position& other) const {
    auto r = dht::tri_compare(_token, other.token());
    if (r != 0) {
        return r;
    } else if (other.has_key()) {
        return _key.legacy_tri_compare(s, *other.key());
    }
    return -other.relation_to_keys();
}

bool
decorated_key::less_compare(const schema& s, const ring_position& other) const {
    return tri_compare(s, other) < 0;
}

bool
decorated_key::less_compare(const schema& s, const decorated_key& other) const {
    return tri_compare(s, other) < 0;
}

decorated_key::less_comparator::less_comparator(schema_ptr s)
    : s(std::move(s))
{ }

bool
decorated_key::less_comparator::operator()(const decorated_key& lhs, const decorated_key& rhs) const {
    return lhs.less_compare(*s, rhs);
}

bool
decorated_key::less_comparator::operator()(const ring_position& lhs, const decorated_key& rhs) const {
    return rhs.tri_compare(*s, lhs) > 0;
}

bool
decorated_key::less_comparator::operator()(const decorated_key& lhs, const ring_position& rhs) const {
    return lhs.tri_compare(*s, rhs) < 0;
}

std::ostream& operator<<(std::ostream& out, const ring_position& pos) {
    out << "{" << pos.token();
    if (pos.has_key()) {
        out << ", " << *pos.key();
    } else {
        out << ", " << ((pos.relation_to_keys() < 0) ? "start" : "end");
    }
    return out << "}";
}

unsigned shard_of(const token& t) {
    return global_partitioner().shard_of(t);
}

stdx::optional<ring_position_range_and_shard>
ring_position_range_sharder::next(const schema& s) {
    if (_done) {
        return {};
    }
    auto shard = _range.start() ? shard_of(_range.start()->value().token()) : global_partitioner().shard_of_minimum_token();
    auto shard_boundary_token = _partitioner.token_for_next_shard(_range.start() ? _range.start()->value().token() : minimum_token());
    auto shard_boundary = ring_position::starting_at(shard_boundary_token);
    if ((!_range.end() || shard_boundary.less_compare(s, _range.end()->value()))
            && shard_boundary_token != maximum_token()) {
        // split the range at end_of_shard
        auto start = _range.start();
        auto end = range_bound<ring_position>(shard_boundary, false);
        _range = dht::partition_range(
                range_bound<ring_position>(std::move(shard_boundary), true),
                std::move(_range.end()));
        return ring_position_range_and_shard{dht::partition_range(std::move(start), std::move(end)), shard};
    }
    _done = true;
    return ring_position_range_and_shard{std::move(_range), shard};
}

ring_position_range_vector_sharder::ring_position_range_vector_sharder(dht::partition_range_vector ranges)
        : _ranges(std::move(ranges))
        , _current_range(_ranges.begin()) {
    next_range();
}

stdx::optional<ring_position_range_and_shard_and_element>
ring_position_range_vector_sharder::next(const schema& s) {
    if (!_current_sharder) {
        return stdx::nullopt;
    }
    auto range_and_shard = _current_sharder->next(s);
    while (!range_and_shard && _current_range != _ranges.end()) {
        next_range();
        range_and_shard = _current_sharder->next(s);
    }
    auto ret = stdx::optional<ring_position_range_and_shard_and_element>();
    if (range_and_shard) {
        ret.emplace(std::move(*range_and_shard), _current_range - _ranges.begin() - 1);
    }
    return ret;
}

int ring_position_comparator::operator()(const ring_position& lh, const ring_position& rh) const {
    return lh.tri_compare(s, rh);
}

int token_comparator::operator()(const token& t1, const token& t2) const {
    return tri_compare(t1, t2);
}

bool ring_position::equal(const schema& s, const ring_position& other) const {
    return tri_compare(s, other) == 0;
}

bool ring_position::less_compare(const schema& s, const ring_position& other) const {
    return tri_compare(s, other) < 0;
}

int ring_position::tri_compare(const schema& s, const ring_position& o) const {
    if (_token != o._token) {
        return _token < o._token ? -1 : 1;
    }

    if (_key && o._key) {
        return _key->legacy_tri_compare(s, *o._key);
    }

    if (!_key && !o._key) {
        return relation_to_keys() - o.relation_to_keys();
    } else if (!_key) {
        return relation_to_keys();
    } else {
        return -o.relation_to_keys();
    }
}

dht::partition_range
to_partition_range(dht::token_range r) {
    using bound_opt = std::experimental::optional<dht::partition_range::bound>;
    auto start = r.start()
                 ? bound_opt(dht::ring_position(r.start()->value(),
                                                r.start()->is_inclusive()
                                                ? dht::ring_position::token_bound::start
                                                : dht::ring_position::token_bound::end))
                 : bound_opt();

    auto end = r.end()
               ? bound_opt(dht::ring_position(r.end()->value(),
                                              r.end()->is_inclusive()
                                              ? dht::ring_position::token_bound::end
                                              : dht::ring_position::token_bound::start))
               : bound_opt();

    return { std::move(start), std::move(end) };
}

std::map<unsigned, dht::partition_range_vector>
split_range_to_shards(dht::partition_range pr, const schema& s) {
    std::map<unsigned, dht::partition_range_vector> ret;
    auto sharder = dht::ring_position_range_sharder(std::move(pr));
    auto rprs = sharder.next(s);
    while (rprs) {
        ret[rprs->shard].emplace_back(rprs->ring_range);
        rprs = sharder.next(s);
    }
    return ret;
}

std::map<unsigned, dht::partition_range_vector>
split_ranges_to_shards(const dht::token_range_vector& ranges, const schema& s) {
    std::map<unsigned, dht::partition_range_vector> ret;
    for (const auto& range : ranges) {
        auto pr = dht::to_partition_range(range);
        auto sharder = dht::ring_position_range_sharder(std::move(pr));
        auto rprs = sharder.next(s);
        while (rprs) {
            ret[rprs->shard].emplace_back(rprs->ring_range);
            rprs = sharder.next(s);
        }
    }
    return ret;
}

}

namespace std {

size_t
hash<dht::token>::hash_large_token(const managed_bytes& b) const {
    auto read_bytes = boost::irange<size_t>(0, b.size())
            | boost::adaptors::transformed([&b] (size_t idx) { return b[idx]; });
    std::array<uint64_t, 2> result;
    utils::murmur_hash::hash3_x64_128(read_bytes.begin(), b.size(), 0, result);
    return result[0];
}

}
