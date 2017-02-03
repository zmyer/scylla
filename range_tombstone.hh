/*
 * Copyright (C) 2016 ScyllaDB
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

#include <boost/intrusive/set.hpp>
#include <boost/range/algorithm.hpp>
#include <experimental/optional>
#include "hashing.hh"
#include "keys.hh"
#include "tombstone.hh"
#include "clustering_bounds_comparator.hh"

namespace bi = boost::intrusive;
namespace stdx = std::experimental;

class position_in_partition_view;

/**
 * Represents a ranged deletion operation. Can be empty.
 */
class range_tombstone final {
    bi::set_member_hook<bi::link_mode<bi::auto_unlink>> _link;
public:
    clustering_key_prefix start;
    bound_kind start_kind;
    clustering_key_prefix end;
    bound_kind end_kind;
    tombstone tomb;
    range_tombstone(clustering_key_prefix start, bound_kind start_kind, clustering_key_prefix end, bound_kind end_kind, tombstone tomb)
            : start(std::move(start))
            , start_kind(start_kind)
            , end(std::move(end))
            , end_kind(end_kind)
            , tomb(std::move(tomb))
    { }
    range_tombstone(bound_view start, bound_view end, tombstone tomb)
            : range_tombstone(start.prefix, start.kind, end.prefix, end.kind, std::move(tomb))
    { }
    range_tombstone(clustering_key_prefix&& start, clustering_key_prefix&& end, tombstone tomb)
            : range_tombstone(std::move(start), bound_kind::incl_start, std::move(end), bound_kind::incl_end, std::move(tomb))
    { }
    // IDL constructor
    range_tombstone(clustering_key_prefix&& start, tombstone tomb, bound_kind start_kind, clustering_key_prefix&& end, bound_kind end_kind)
            : range_tombstone(std::move(start), start_kind, std::move(end), end_kind, std::move(tomb))
    { }
    range_tombstone(range_tombstone&& rt) noexcept
            : range_tombstone(std::move(rt.start), rt.start_kind, std::move(rt.end), rt.end_kind, std::move(rt.tomb)) {
        update_node(rt._link);
    }
    struct without_link { };
    range_tombstone(range_tombstone&& rt, without_link) noexcept
            : range_tombstone(std::move(rt.start), rt.start_kind, std::move(rt.end), rt.end_kind, std::move(rt.tomb)) {
    }
    range_tombstone(const range_tombstone& rt)
            : range_tombstone(rt.start, rt.start_kind, rt.end, rt.end_kind, rt.tomb)
    { }
    range_tombstone& operator=(range_tombstone&& rt) noexcept {
        update_node(rt._link);
        move_assign(std::move(rt));
        return *this;
    }
    range_tombstone& operator=(const range_tombstone& rt) {
        start = rt.start;
        start_kind = rt.start_kind;
        end = rt.end;
        end_kind = rt.end_kind;
        tomb = rt.tomb;
        return *this;
    }
    const bound_view start_bound() const {
        return bound_view(start, start_kind);
    }
    const bound_view end_bound() const {
        return bound_view(end, end_kind);
    }
    position_in_partition_view position() const;
    bool empty() const {
        return !bool(tomb);
    }
    explicit operator bool() const {
        return bool(tomb);
    }
    bool equal(const schema& s, const range_tombstone& other) const {
        return tomb == other.tomb && start_bound().equal(s, other.start_bound()) && end_bound().equal(s, other.end_bound());
    }
    struct compare {
        bound_view::compare _c;
        compare(const schema& s) : _c(s) {}
        bool operator()(const range_tombstone& rt1, const range_tombstone& rt2) const {
            return _c(rt1.start_bound(), rt2.start_bound());
        }
    };
    template<typename Hasher>
    void feed_hash(Hasher& h, const schema& s) const {
        start.feed_hash(h, s);
        // For backward compatibility, don't consider new fields if
        // this could be an old-style, overlapping, range tombstone.
        if (!start.equal(s, end) || start_kind != bound_kind::incl_start || end_kind != bound_kind::incl_end) {
            ::feed_hash(h, start_kind);
            end.feed_hash(h, s);
            ::feed_hash(h, end_kind);
        }
        ::feed_hash(h, tomb);
    }
    friend void swap(range_tombstone& rt1, range_tombstone& rt2) {
        range_tombstone tmp(std::move(rt2), without_link());
        rt2.move_assign(std::move(rt1));
        rt1.move_assign(std::move(tmp));
    }
    friend std::ostream& operator<<(std::ostream& out, const range_tombstone& rt);
    using container_type = bi::set<range_tombstone,
            bi::member_hook<range_tombstone, bi::set_member_hook<bi::link_mode<bi::auto_unlink>>, &range_tombstone::_link>,
            bi::compare<range_tombstone::compare>,
            bi::constant_time_size<false>>;

    static bool is_single_clustering_row_tombstone(const schema& s, const clustering_key_prefix& start,
        bound_kind start_kind, const clustering_key_prefix& end, bound_kind end_kind)
    {
        return start.is_full(s) && start_kind == bound_kind::incl_start
            && end_kind == bound_kind::incl_end && start.equal(s, end);
    }

    // Applies src to this. The tombstones may be overlapping.
    // If the tombstone with larger timestamp has the smaller range the remainder
    // is returned, it guaranteed not to overlap with this.
    // The start bounds of this and src are required to be equal. The start bound
    // of this is not changed. The start bound of the remainder (if there is any)
    // is larger than the end bound of this.
    stdx::optional<range_tombstone> apply(const schema& s, range_tombstone&& src);

    size_t external_memory_usage() const {
        return start.external_memory_usage() + end.external_memory_usage();
    }

    size_t memory_usage() const {
        return sizeof(range_tombstone) + external_memory_usage();
    }

    // Flips start and end bound so that range tombstone can be used in reversed
    // streams.
    void flip() {
        std::swap(start, end);
        std::swap(start_kind, end_kind);
        start_kind = flip_bound_kind(start_kind);
        end_kind = flip_bound_kind(end_kind);
    }
private:
    void move_assign(range_tombstone&& rt) {
        start = std::move(rt.start);
        start_kind = rt.start_kind;
        end = std::move(rt.end);
        end_kind = rt.end_kind;
        tomb = std::move(rt.tomb);
    }
    void update_node(bi::set_member_hook<bi::link_mode<bi::auto_unlink>>& other_link) {
        if (other_link.is_linked()) {
            // Move the link in case we're being relocated by LSA.
            container_type::node_algorithms::replace_node(other_link.this_ptr(), _link.this_ptr());
            container_type::node_algorithms::init(other_link.this_ptr());
        }
    }
};

// This is a helper intended for accumulating tombstones from a streamed
// mutation and determining what is the tombstone for a given clustering row.
//
// After apply(rt) or tombstone_for_row(ck) are called there are followng
// restrictions for subsequent calls:
//  - apply(rt1) can be invoked only if rt.start_bound() < rt1.start_bound()
//    and ck < rt1.start_bound()
//  - tombstone_for_row(ck1) can be invoked only if rt.start_bound() < ck1
//    and ck < ck1
//
// In other words position in partition of the mutation fragments passed to the
// accumulator must be increasing.
class range_tombstone_accumulator {
    bound_view::compare _cmp;
    tombstone _partition_tombstone;
    std::deque<range_tombstone> _range_tombstones;
    tombstone _current_tombstone;
    bool _reversed;
private:
    void update_current_tombstone();
    void drop_unneeded_tombstones(const clustering_key_prefix& ck, int w = 0);
public:
    range_tombstone_accumulator(const schema& s, bool reversed)
        : _cmp(s), _reversed(reversed) { }

    void set_partition_tombstone(tombstone t) {
        _partition_tombstone = t;
        update_current_tombstone();
    }

    tombstone get_partition_tombstone() const {
        return _partition_tombstone;
    }

    tombstone tombstone_for_row(const clustering_key_prefix& ck) {
        drop_unneeded_tombstones(ck);
        return _current_tombstone;
    }

    const std::deque<range_tombstone>& range_tombstones_for_row(const clustering_key_prefix& ck) {
        drop_unneeded_tombstones(ck);
        return _range_tombstones;
    }

    void apply(const range_tombstone& rt);

    void clear();
};
