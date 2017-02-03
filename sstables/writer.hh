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

#include "core/iostream.hh"
#include "core/fstream.hh"
#include "types.hh"
#include "compress.hh"
#include <seastar/core/byteorder.hh>

namespace sstables {

class file_writer {
    output_stream<char> _out;
    size_t _offset = 0;
public:
    file_writer(file f, file_output_stream_options options)
        : _out(make_file_output_stream(std::move(f), std::move(options))) {}

    file_writer(output_stream<char>&& out)
        : _out(std::move(out)) {}

    virtual ~file_writer() = default;
    file_writer(file_writer&&) = default;

    future<> write(const char* buf, size_t n) {
        _offset += n;
        return _out.write(buf, n);
    }
    future<> write(const bytes& s) {
        _offset += s.size();
        return _out.write(s);
    }
    future<> flush() {
        return _out.flush();
    }
    future<> close() {
        return _out.close();
    }
    size_t offset() {
        return _offset;
    }
};


class sizing_data_sink : public data_sink_impl {
    uint64_t& _size;
public:
    explicit sizing_data_sink(uint64_t& dest) : _size(dest) {
        _size = 0;
    }
    virtual temporary_buffer<char> allocate_buffer(size_t size) {
        return temporary_buffer<char>(size);
    }
    virtual future<> put(net::packet data) override {
        _size += data.len();
        return make_ready_future<>();
    }
    virtual future<> put(std::vector<temporary_buffer<char>> data) override {
        _size += boost::accumulate(data | boost::adaptors::transformed(std::mem_fn(&temporary_buffer<char>::size)), 0);
        return make_ready_future<>();
    }
    virtual future<> put(temporary_buffer<char> buf) override {
        _size += buf.size();
        return make_ready_future<>();
    }
    virtual future<> flush() override {
        return make_ready_future<>();
    }
    virtual future<> close() override {
        return make_ready_future<>();
    }
};

inline
output_stream<char>
make_sizing_output_stream(uint64_t& dest) {
    return output_stream<char>(data_sink(std::make_unique<sizing_data_sink>(std::ref(dest))), 4096);
}

// Must be called from a thread
template <typename T>
uint64_t
serialized_size(const T& object) {
    uint64_t size = 0;
    auto writer = file_writer(make_sizing_output_stream(size));
    write(writer, object);
    writer.flush().get();
    writer.close().get();
    return size;
}

output_stream<char> make_checksummed_file_output_stream(file f, struct checksum& cinfo, uint32_t& full_file_checksum, bool checksum_file, file_output_stream_options options);

class checksummed_file_writer : public file_writer {
    checksum _c;
    uint32_t _full_checksum;
public:
    checksummed_file_writer(file f, file_output_stream_options options, bool checksum_file = false)
            : file_writer(make_checksummed_file_output_stream(std::move(f), _c, _full_checksum, checksum_file, options))
            , _c({uint32_t(std::min(size_t(DEFAULT_CHUNK_SIZE), size_t(options.buffer_size)))})
            , _full_checksum(init_checksum_adler32()) {}

    // Since we are exposing a reference to _full_checksum, we delete the move
    // constructor.  If it is moved, the reference will refer to the old
    // location.
    checksummed_file_writer(checksummed_file_writer&&) = delete;
    checksummed_file_writer(const checksummed_file_writer&) = default;

    checksum& finalize_checksum() {
        return _c;
    }
    uint32_t full_checksum() {
        return _full_checksum;
    }
};

class checksummed_file_data_sink_impl : public data_sink_impl {
    output_stream<char> _out;
    struct checksum& _c;
    uint32_t& _full_checksum;
    bool _checksum_file;
public:
    checksummed_file_data_sink_impl(file f, struct checksum& c, uint32_t& full_file_checksum, bool checksum_file, file_output_stream_options options)
            : _out(make_file_output_stream(std::move(f), std::move(options)))
            , _c(c)
            , _full_checksum(full_file_checksum)
            , _checksum_file(checksum_file)
            {}

    future<> put(net::packet data) { abort(); }
    virtual future<> put(temporary_buffer<char> buf) override {
        // bufs will usually be a multiple of chunk size, but this won't be the case for
        // the last buffer being flushed.

        if (!_checksum_file) {
            _full_checksum = checksum_adler32(_full_checksum, buf.begin(), buf.size());
        } else {
            for (size_t offset = 0; offset < buf.size(); offset += _c.chunk_size) {
                size_t size = std::min(size_t(_c.chunk_size), buf.size() - offset);
                uint32_t per_chunk_checksum = init_checksum_adler32();

                per_chunk_checksum = checksum_adler32(per_chunk_checksum, buf.begin() + offset, size);
                _full_checksum = checksum_adler32_combine(_full_checksum, per_chunk_checksum, size);
                _c.checksums.push_back(per_chunk_checksum);
            }
        }
        auto f = _out.write(buf.begin(), buf.size());
        return f.then([buf = std::move(buf)] {});
    }

    virtual future<> close() {
        // Nothing to do, because close at the file_stream level will call flush on us.
        return _out.close();
    }
};

class checksummed_file_data_sink : public data_sink {
public:
    checksummed_file_data_sink(file f, struct checksum& cinfo, uint32_t& full_file_checksum, bool checksum_file, file_output_stream_options options)
        : data_sink(std::make_unique<checksummed_file_data_sink_impl>(std::move(f), cinfo, full_file_checksum, checksum_file, std::move(options))) {}
};

inline
output_stream<char> make_checksummed_file_output_stream(file f, struct checksum& cinfo, uint32_t& full_file_checksum, bool checksum_file, file_output_stream_options options) {
    auto buffer_size = options.buffer_size;
    return output_stream<char>(checksummed_file_data_sink(std::move(f), cinfo, full_file_checksum, checksum_file, std::move(options)), buffer_size, true);
}

// compressed_file_data_sink_impl works as a filter for a file output stream,
// where the buffer flushed will be compressed and its checksum computed, then
// the result passed to a regular output stream.
class compressed_file_data_sink_impl : public data_sink_impl {
    output_stream<char> _out;
    sstables::compression* _compression_metadata;
    size_t _pos = 0;
public:
    compressed_file_data_sink_impl(file f, sstables::compression* cm, file_output_stream_options options)
            : _out(make_file_output_stream(std::move(f), options))
            , _compression_metadata(cm) {}

    future<> put(net::packet data) { abort(); }
    virtual future<> put(temporary_buffer<char> buf) override {
        auto output_len = _compression_metadata->compress_max_size(buf.size());
        // account space for checksum that goes after compressed data.
        temporary_buffer<char> compressed(output_len + 4);

        // compress flushed data.
        auto len = _compression_metadata->compress(buf.get(), buf.size(), compressed.get_write(), output_len);
        if (len > output_len) {
            throw std::runtime_error("possible overflow during compression");
        }

        _compression_metadata->offsets.elements.push_back(_pos);
        // account compressed data + 32-bit checksum.
        _pos += len + 4;
        _compression_metadata->set_compressed_file_length(_pos);
        // total length of the uncompressed data.
        _compression_metadata->data_len += buf.size();

        // compute 32-bit checksum for compressed data.
        uint32_t per_chunk_checksum = checksum_adler32(compressed.get(), len);
        _compression_metadata->update_full_checksum(per_chunk_checksum, len);

        // write checksum into buffer after compressed data.
        write_be<uint32_t>(compressed.get_write() + len, per_chunk_checksum);

        compressed.trim(len + 4);

        auto f = _out.write(compressed.get(), compressed.size());
        return f.then([compressed = std::move(compressed)] {});
    }
    virtual future<> close() {
        return _out.close();
    }
};

class compressed_file_data_sink : public data_sink {
public:
    compressed_file_data_sink(file f, sstables::compression* cm, file_output_stream_options options)
        : data_sink(std::make_unique<compressed_file_data_sink_impl>(
                std::move(f), cm, options)) {}
};

static inline output_stream<char> make_compressed_file_output_stream(file f, file_output_stream_options options, sstables::compression* cm) {
    // buffer of output stream is set to chunk length, because flush must
    // happen every time a chunk was filled up.
    auto outer_buffer_size = cm->uncompressed_chunk_length();
    return output_stream<char>(compressed_file_data_sink(std::move(f), cm, options), outer_buffer_size, true);
}

}
