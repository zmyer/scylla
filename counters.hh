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

#include <boost/range/algorithm/find_if.hpp>

#include "atomic_cell_or_collection.hh"
#include "types.hh"

#include "stdx.hh"

class mutation;

class mutation;

class counter_id {
    int64_t _least_significant;
    int64_t _most_significant;
public:
    counter_id() = default;
    explicit counter_id(utils::UUID uuid) noexcept
        : _least_significant(uuid.get_least_significant_bits())
        , _most_significant(uuid.get_most_significant_bits())
    { }

    utils::UUID to_uuid() const {
        return utils::UUID(_most_significant, _least_significant);
    }

    bool operator<(const counter_id& other) const {
        return to_uuid() < other.to_uuid();
    }
    bool operator==(const counter_id& other) const {
        return to_uuid() == other.to_uuid();
    }
    bool operator!=(const counter_id& other) const {
        return !(*this == other);
    }
public:
    static counter_id local();

    // For tests.
    static counter_id generate_random() {
        return counter_id(utils::make_random_uuid());
    }
};
static_assert(std::is_pod<counter_id>::value, "counter_id should be a POD type");

std::ostream& operator<<(std::ostream& os, const counter_id& id);

class counter_shard_view {
    enum class offset : unsigned {
        id = 0u,
        value = unsigned(id) + sizeof(counter_id),
        logical_clock = unsigned(value) + sizeof(int64_t),
        total_size = unsigned(logical_clock) + sizeof(int64_t),
    };
private:
    bytes_view::const_pointer _base;
private:
    template<typename T>
    T read(offset off) const {
        T value;
        std::copy_n(_base + static_cast<unsigned>(off), sizeof(T), reinterpret_cast<char*>(&value));
        return value;
    }
public:
    static constexpr auto size = size_t(offset::total_size);
public:
    counter_shard_view() = default;
    explicit counter_shard_view(bytes_view::const_pointer ptr) noexcept
        : _base(ptr) { }

    counter_id id() const { return read<counter_id>(offset::id); }
    int64_t value() const { return read<int64_t>(offset::value); }
    int64_t logical_clock() const { return read<int64_t>(offset::logical_clock); }

    struct less_compare_by_id {
        bool operator()(const counter_shard_view& x, const counter_shard_view& y) const {
            return x.id() < y.id();
        }
    };
};

std::ostream& operator<<(std::ostream& os, counter_shard_view csv);

class counter_shard {
    counter_id _id;
    int64_t _value;
    int64_t _logical_clock;
private:
    template<typename T>
    static void write(const T& value, bytes::iterator& out) {
        out = std::copy_n(reinterpret_cast<const char*>(&value), sizeof(T), out);
    }
public:
    counter_shard(counter_id id, int64_t value, int64_t logical_clock) noexcept
        : _id(id)
        , _value(value)
        , _logical_clock(logical_clock)
    { }

    explicit counter_shard(counter_shard_view csv) noexcept
        : _id(csv.id())
        , _value(csv.value())
        , _logical_clock(csv.logical_clock())
    { }

    counter_id id() const { return _id; }
    int64_t value() const { return _value; }
    int64_t logical_clock() const { return _logical_clock; }

    counter_shard& update(int64_t value_delta, int64_t clock_increment) noexcept {
        _value += value_delta;
        _logical_clock += clock_increment;
        return *this;
    }

    counter_shard& apply(counter_shard_view other) noexcept {
        auto other_clock = other.logical_clock();
        if (_logical_clock < other_clock) {
            _logical_clock = other_clock;
            _value = other.value();
        }
        return *this;
    }

    static size_t serialized_size() {
        return counter_shard_view::size;
    }
    void serialize(bytes::iterator& out) const {
        write(_id, out);
        write(_value, out);
        write(_logical_clock, out);
    }
};

class counter_cell_builder {
    std::vector<counter_shard> _shards;
public:
    counter_cell_builder() = default;
    counter_cell_builder(size_t shard_count) {
        _shards.reserve(shard_count);
    }

    void add_shard(const counter_shard& cs) {
        _shards.emplace_back(cs);
    }

    size_t serialized_size() const {
        return _shards.size() * counter_shard::serialized_size();
    }
    void serialize(bytes::iterator& out) const {
        for (auto&& cs : _shards) {
            cs.serialize(out);
        }
    }

    bool empty() const {
        return _shards.empty();
    }

    atomic_cell build(api::timestamp_type timestamp) const {
        bytes b(bytes::initialized_later(), serialized_size());
        auto out = b.begin();
        serialize(out);
        return atomic_cell::make_live(timestamp, b);
    }

    class inserter_iterator : public std::iterator<std::output_iterator_tag, counter_shard> {
        counter_cell_builder* _builder;
    public:
        explicit inserter_iterator(counter_cell_builder& b) : _builder(&b) { }
        inserter_iterator& operator=(const counter_shard& cs) {
            _builder->add_shard(cs);
            return *this;
        }
        inserter_iterator& operator=(const counter_shard_view& csv) {
            return operator=(counter_shard(csv));
        }
        inserter_iterator& operator++() { return *this; }
        inserter_iterator& operator++(int) { return *this; }
        inserter_iterator& operator*() { return *this; };
    };

    inserter_iterator inserter() {
        return inserter_iterator(*this);
    }
};

// <counter_id>   := <int64_t><int64_t>
// <shard>        := <counter_id><int64_t:value><int64_t:logical_clock>
// <counter_cell> := <shard>*
class counter_cell_view {
    atomic_cell_view _cell;
private:
    class shard_iterator : public std::iterator<std::input_iterator_tag, const counter_shard_view> {
        bytes_view::const_pointer _current;
        counter_shard_view _current_view;
    public:
        shard_iterator() = default;
        shard_iterator(bytes_view::const_pointer ptr) noexcept
            : _current(ptr), _current_view(ptr) { }

        const counter_shard_view& operator*() const noexcept {
            return _current_view;
        }
        const counter_shard_view* operator->() const noexcept {
            return &_current_view;
        }
        shard_iterator& operator++() noexcept {
            _current += counter_shard_view::size;
            _current_view = counter_shard_view(_current);
            return *this;
        }
        shard_iterator operator++(int) noexcept {
            auto it = *this;
            operator++();
            return it;
        }
        bool operator==(const shard_iterator& other) const noexcept {
            return _current == other._current;
        }
        bool operator!=(const shard_iterator& other) const noexcept {
            return !(*this == other);
        }
    };
public:
    boost::iterator_range<shard_iterator> shards() const {
        auto bv = _cell.value();
        auto begin = shard_iterator(bv.data());
        auto end = shard_iterator(bv.data() + bv.size());
        return boost::make_iterator_range(begin, end);
    }

    size_t shard_count() const {
        return _cell.value().size() / counter_shard_view::size;
    }
public:
    // ac must be a live counter cell
    explicit counter_cell_view(atomic_cell_view ac) noexcept : _cell(ac) {
        assert(_cell.is_live());
        assert(!_cell.is_counter_update());
    }

    api::timestamp_type timestamp() const { return _cell.timestamp(); }

    static data_type total_value_type() { return long_type; }

    int64_t total_value() const {
        return boost::accumulate(shards(), int64_t(0), [] (int64_t v, counter_shard_view cs) {
            return v + cs.value();
        });
    }

    stdx::optional<counter_shard_view> get_shard(const counter_id& id) const {
        auto it = boost::range::find_if(shards(), [&id] (counter_shard_view csv) {
            return csv.id() == id;
        });
        if (it == shards().end()) {
            return { };
        }
        return *it;
    }

    stdx::optional<counter_shard_view> local_shard() const {
        // TODO: consider caching local shard position
        return get_shard(counter_id::local());
    }

    // Reversibly applies two counter cells, at least one of them must be live.
    // Returns true iff dst was modified.
    static bool apply_reversibly(atomic_cell_or_collection& dst, atomic_cell_or_collection& src);

    // Reverts apply performed by apply_reversible().
    static void revert_apply(atomic_cell_or_collection& dst, atomic_cell_or_collection& src);

    // Computes a counter cell containing minimal amount of data which, when
    // applied to 'b' returns the same cell as 'a' and 'b' applied together.
    static stdx::optional<atomic_cell> difference(atomic_cell_view a, atomic_cell_view b);

    friend std::ostream& operator<<(std::ostream& os, counter_cell_view ccv);
};

// Transforms mutation dst from counter updates to counter shards using state
// stored in current_state.
// If current_state is present it has to be in the same schema as dst.
void transform_counter_updates_to_shards(mutation& dst, const mutation* current_state, uint64_t clock_offset);

template<>
struct appending_hash<counter_shard_view> {
    template<typename Hasher>
    void operator()(Hasher& h, const counter_shard_view& cshard) const {
        ::feed_hash(h, cshard.id().to_uuid());
        ::feed_hash(h, cshard.value());
        ::feed_hash(h, cshard.logical_clock());
    }
};

template<>
struct appending_hash<counter_cell_view> {
    template<typename Hasher>
    void operator()(Hasher& h, const counter_cell_view& cell) const {
        ::feed_hash(h, true); // is_live
        ::feed_hash(h, cell.timestamp());
        for (auto&& csv : cell.shards()) {
            ::feed_hash(h, csv);
        }
    }
};
