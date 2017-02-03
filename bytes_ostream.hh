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

#include <boost/range/iterator_range.hpp>

#include "bytes.hh"
#include "core/unaligned.hh"
#include "hashing.hh"
#include "seastar/core/simple-stream.hh"
/**
 * Utility for writing data into a buffer when its final size is not known up front.
 *
 * Internally the data is written into a chain of chunks allocated on-demand.
 * No resizing of previously written data happens.
 *
 */
class bytes_ostream {
public:
    using size_type = bytes::size_type;
    using value_type = bytes::value_type;
    static constexpr size_type max_chunk_size() { return 16 * 1024; }
private:
    static_assert(sizeof(value_type) == 1, "value_type is assumed to be one byte long");
    struct chunk {
        // FIXME: group fragment pointers to reduce pointer chasing when packetizing
        std::unique_ptr<chunk> next;
        ~chunk() {
            auto p = std::move(next);
            while (p) {
                // Avoid recursion when freeing chunks
                auto p_next = std::move(p->next);
                p = std::move(p_next);
            }
        }
        size_type offset; // Also means "size" after chunk is closed
        size_type size;
        value_type data[0];
        void operator delete(void* ptr) { free(ptr); }
    };
    // FIXME: consider increasing chunk size as the buffer grows
    static constexpr size_type chunk_size{512};
private:
    std::unique_ptr<chunk> _begin;
    chunk* _current;
    size_type _size;
public:
    class fragment_iterator : public std::iterator<std::input_iterator_tag, bytes_view> {
        chunk* _current;
    public:
        fragment_iterator(chunk* current) : _current(current) {}
        fragment_iterator(const fragment_iterator&) = default;
        fragment_iterator& operator=(const fragment_iterator&) = default;
        bytes_view operator*() const {
            return { _current->data, _current->offset };
        }
        bytes_view operator->() const {
            return *(*this);
        }
        fragment_iterator& operator++() {
            _current = _current->next.get();
            return *this;
        }
        fragment_iterator operator++(int) {
            fragment_iterator tmp(*this);
            ++(*this);
            return tmp;
        }
        bool operator==(const fragment_iterator& other) const {
            return _current == other._current;
        }
        bool operator!=(const fragment_iterator& other) const {
            return _current != other._current;
        }
    };
private:
    inline size_type current_space_left() const {
        if (!_current) {
            return 0;
        }
        return _current->size - _current->offset;
    }
    // Figure out next chunk size.
    //   - must be enough for data_size
    //   - must be at least chunk_size
    //   - try to double each time to prevent too many allocations
    //   - do not exceed max_chunk_size
    size_type next_alloc_size(size_t data_size) const {
        auto next_size = _current
                ? _current->size * 2
                : chunk_size;
        next_size = std::min(next_size, max_chunk_size());
        // FIXME: check for overflow?
        return std::max<size_type>(next_size, data_size + sizeof(chunk));
    }
    // Makes room for a contiguous region of given size.
    // The region is accounted for as already written.
    // size must not be zero.
    value_type* alloc(size_type size) {
        if (size <= current_space_left()) {
            auto ret = _current->data + _current->offset;
            _current->offset += size;
            _size += size;
            return ret;
        } else {
            auto alloc_size = next_alloc_size(size);
            auto space = malloc(alloc_size);
            if (!space) {
                throw std::bad_alloc();
            }
            auto new_chunk = std::unique_ptr<chunk>(new (space) chunk());
            new_chunk->offset = size;
            new_chunk->size = alloc_size - sizeof(chunk);
            if (_current) {
                _current->next = std::move(new_chunk);
                _current = _current->next.get();
            } else {
                _begin = std::move(new_chunk);
                _current = _begin.get();
            }
            _size += size;
            return _current->data;
        };
    }
public:
    bytes_ostream() noexcept
        : _begin()
        , _current(nullptr)
        , _size(0)
    { }

    bytes_ostream(bytes_ostream&& o) noexcept
        : _begin(std::move(o._begin))
        , _current(o._current)
        , _size(o._size)
    {
        o._current = nullptr;
        o._size = 0;
    }

    bytes_ostream(const bytes_ostream& o)
        : _begin()
        , _current(nullptr)
        , _size(0)
    {
        append(o);
    }

    bytes_ostream& operator=(const bytes_ostream& o) {
        if (this != &o) {
            auto x = bytes_ostream(o);
            *this = std::move(x);
        }
        return *this;
    }

    bytes_ostream& operator=(bytes_ostream&& o) noexcept {
        if (this != &o) {
            this->~bytes_ostream();
            new (this) bytes_ostream(std::move(o));
        }
        return *this;
    }

    template <typename T>
    struct place_holder {
        value_type* ptr;
        // makes the place_holder looks like a stream
        seastar::simple_output_stream get_stream() {
            return seastar::simple_output_stream(reinterpret_cast<char*>(ptr), sizeof(T));
        }
    };

    // Returns a place holder for a value to be written later.
    template <typename T>
    inline
    std::enable_if_t<std::is_fundamental<T>::value, place_holder<T>>
    write_place_holder() {
        return place_holder<T>{alloc(sizeof(T))};
    }

    value_type* write_place_holder(size_type size) {
        return alloc(size);
    }

    // Writes given sequence of bytes
    inline void write(bytes_view v) {
        if (v.empty()) {
            return;
        }

        auto this_size = std::min(v.size(), size_t(current_space_left()));
        if (this_size) {
            memcpy(_current->data + _current->offset, v.begin(), this_size);
            _current->offset += this_size;
            _size += this_size;
            v.remove_prefix(this_size);
        }

        while (!v.empty()) {
            auto this_size = std::min(v.size(), size_t(max_chunk_size()));
            std::copy_n(v.begin(), this_size, alloc(this_size));
            v.remove_prefix(this_size);
        }
    }

    void write(const char* ptr, size_t size) {
        write(bytes_view(reinterpret_cast<const signed char*>(ptr), size));
    }

    bool is_linearized() const {
        return !_begin || !_begin->next;
    }

    // Call only when is_linearized()
    bytes_view view() const {
        assert(is_linearized());
        if (!_current) {
            return bytes_view();
        }

        return bytes_view(_current->data, _size);
    }

    // Makes the underlying storage contiguous and returns a view to it.
    // Invalidates all previously created placeholders.
    bytes_view linearize() {
        if (is_linearized()) {
            return view();
        }

        auto space = malloc(_size + sizeof(chunk));
        if (!space) {
            throw std::bad_alloc();
        }

        auto new_chunk = std::unique_ptr<chunk>(new (space) chunk());
        new_chunk->offset = _size;
        new_chunk->size = _size;

        auto dst = new_chunk->data;
        auto r = _begin.get();
        while (r) {
            auto next = r->next.get();
            dst = std::copy_n(r->data, r->offset, dst);
            r = next;
        }

        _current = new_chunk.get();
        _begin = std::move(new_chunk);
        return bytes_view(_current->data, _size);
    }

    // Returns the amount of bytes written so far
    size_type size() const {
        return _size;
    }

    bool empty() const {
        return _size == 0;
    }

    void reserve(size_t size) {
        // FIXME: implement
    }

    void append(const bytes_ostream& o) {
        for (auto&& bv : o.fragments()) {
            write(bv);
        }
    }

    // begin() and end() form an input range to bytes_view representing fragments.
    // Any modification of this instance invalidates iterators.
    fragment_iterator begin() const { return { _begin.get() }; }
    fragment_iterator end() const { return { nullptr }; }

    boost::iterator_range<fragment_iterator> fragments() const {
        return { begin(), end() };
    }

    struct position {
        chunk* _chunk;
        size_type _offset;
    };

    position pos() const {
        return { _current, _current ? _current->offset : 0 };
    }

    // Returns the amount of bytes written since given position.
    // "pos" must be valid.
    size_type written_since(position pos) {
        chunk* c = pos._chunk;
        if (!c) {
            return _size;
        }
        size_type total = c->offset - pos._offset;
        c = c->next.get();
        while (c) {
            total += c->offset;
            c = c->next.get();
        }
        return total;
    }

    // Rollbacks all data written after "pos".
    // Invalidates all placeholders and positions created after "pos".
    void retract(position pos) {
        if (!pos._chunk) {
            *this = {};
            return;
        }
        _size -= written_since(pos);
        _current = pos._chunk;
        _current->next = nullptr;
        _current->offset = pos._offset;
    }

    void reduce_chunk_count() {
        // FIXME: This is a simplified version. It linearizes the whole buffer
        // if its size is below max_chunk_size. We probably could also gain
        // some read performance by doing "real" reduction, i.e. merging
        // all chunks until all but the last one is max_chunk_size.
        if (size() < max_chunk_size()) {
            linearize();
        }
    }

    bool operator==(const bytes_ostream& other) const {
        auto as = fragments().begin();
        auto as_end = fragments().end();
        auto bs = other.fragments().begin();
        auto bs_end = other.fragments().end();

        auto a = *as++;
        auto b = *bs++;
        while (!a.empty() || !b.empty()) {
            auto now = std::min(a.size(), b.size());
            if (!std::equal(a.begin(), a.begin() + now, b.begin(), b.begin() + now)) {
                return false;
            }
            a.remove_prefix(now);
            if (a.empty() && as != as_end) {
                a = *as++;
            }
            b.remove_prefix(now);
            if (b.empty() && bs != bs_end) {
                b = *bs++;
            }
        }
        return true;
    }

    bool operator!=(const bytes_ostream& other) const {
        return !(*this == other);
    }
};

template<>
struct appending_hash<bytes_ostream> {
    template<typename Hasher>
    void operator()(Hasher& h, const bytes_ostream& b) const {
        for (auto&& frag : b.fragments()) {
            feed_hash(h, frag);
        }
    }
};
