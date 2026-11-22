// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

// Little-endian byte stream helpers matching RPCN's raw (non-protobuf) encodings.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rpcn {

class StreamReader {
public:
    explicit StreamReader(const std::vector<uint8_t> &data, size_t start = 0)
        : buf(data)
        , pos(start) {}

    template <typename T>
    T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        if (error || pos + sizeof(T) > buf.size()) {
            error = true;
            return T{};
        }
        T value;
        std::memcpy(&value, buf.data() + pos, sizeof(T)); // RPCN is little-endian, as are all Vita3K hosts
        pos += sizeof(T);
        return value;
    }

    // Null-terminated string. RPCN rejects empty strings unless empty is allowed.
    std::string get_string(bool empty_allowed) {
        std::string out;
        while (!error) {
            if (pos >= buf.size()) {
                error = true;
                break;
            }
            const char c = static_cast<char>(buf[pos++]);
            if (c == '\0')
                break;
            out.push_back(c);
        }
        if (!empty_allowed && out.empty())
            error = true;
        return out;
    }

    // u32 length followed by raw bytes.
    std::vector<uint8_t> get_rawdata() {
        const uint32_t size = get<uint32_t>();
        if (error || pos + size > buf.size()) {
            error = true;
            return {};
        }
        std::vector<uint8_t> out(buf.begin() + pos, buf.begin() + pos + size);
        pos += size;
        return out;
    }

    // Fixed 12-byte communication id ("NPWR01781_00").
    std::string get_com_id() {
        if (error || pos + 12 > buf.size()) {
            error = true;
            return {};
        }
        std::string out(reinterpret_cast<const char *>(buf.data() + pos), 12);
        pos += 12;
        return out;
    }

    bool is_error() const { return error; }
    size_t position() const { return pos; }
    size_t remaining() const { return error ? 0 : buf.size() - pos; }

private:
    const std::vector<uint8_t> &buf;
    size_t pos;
    bool error = false;
};

class StreamWriter {
public:
    template <typename T>
    StreamWriter &put(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(T));
        return *this;
    }

    StreamWriter &put_string(std::string_view str) {
        data.insert(data.end(), str.begin(), str.end());
        data.push_back(0);
        return *this;
    }

    StreamWriter &put_bytes(const void *src, size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(src);
        data.insert(data.end(), bytes, bytes + size);
        return *this;
    }

    // Writes a communication id as the 12 bytes "XXXXYYYYY_NN" RPCN expects.
    StreamWriter &put_com_id(std::string_view com_id) {
        char tmp[12]{};
        std::memcpy(tmp, com_id.data(), std::min<size_t>(com_id.size(), sizeof(tmp)));
        return put_bytes(tmp, sizeof(tmp));
    }

    std::vector<uint8_t> data;
};

} // namespace rpcn
