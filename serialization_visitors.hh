/*
 * Copyright 2016 ScylaDB
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

#include "bytes_ostream.hh"
#include "serializer.hh"

namespace ser {

// frame represents a place holder for object size which will be known later

template<typename Output>
struct place_holder { };

template<typename Output>
struct frame { };

template<>
struct place_holder<bytes_ostream> {
    bytes_ostream::place_holder<size_type> ph;

    place_holder(bytes_ostream::place_holder<size_type> ph) : ph(ph) { }

    void set(bytes_ostream& out, size_type v) {
        auto stream = ph.get_stream();
        serialize(stream, v);
    }
};

template<>
struct frame<bytes_ostream> : public place_holder<bytes_ostream> {
    bytes_ostream::size_type offset;

    frame(bytes_ostream::place_holder<size_type> ph, bytes_ostream::size_type offset)
        : place_holder(ph), offset(offset) { }

    void end(bytes_ostream& out) {
        set(out, out.size() - offset);
    }
};

struct vector_position {
    bytes_ostream::position pos;
    size_type count;
};

//empty frame, behave like a place holder, but is used when no place holder is needed
template<typename Output>
struct empty_frame {
    void end(Output&) {}
    empty_frame() = default;
    empty_frame(const frame<Output>&){}
};

inline place_holder<bytes_ostream> start_place_holder(bytes_ostream& out) {
    auto size_ph = out.write_place_holder<size_type>();
    return { size_ph};
}

inline frame<bytes_ostream> start_frame(bytes_ostream& out) {
    auto offset = out.size();
    auto size_ph = out.write_place_holder<size_type>();
    {
        auto out = size_ph.get_stream();
        serialize(out, (size_type)0);
    }
    return frame<bytes_ostream> { size_ph, offset };
}

template<typename Input>
size_type read_frame_size(Input& in) {
    auto sz = deserialize(in, boost::type<size_type>());
    if (sz < sizeof(size_type)) {
        throw std::runtime_error("Truncated frame");
    }
    return sz - sizeof(size_type);
}


template<>
struct place_holder<seastar::measuring_output_stream> {
    void set(seastar::measuring_output_stream&, size_type) { }
};

template<>
struct frame<seastar::measuring_output_stream> : public place_holder<seastar::measuring_output_stream> {
    void end(seastar::measuring_output_stream& out) { }
};

inline place_holder<seastar::measuring_output_stream> start_place_holder(seastar::measuring_output_stream& out) {
    serialize(out, size_type());
    return { };
}

inline frame<seastar::measuring_output_stream> start_frame(seastar::measuring_output_stream& out) {
    serialize(out, size_type());
    return { };
}

}
