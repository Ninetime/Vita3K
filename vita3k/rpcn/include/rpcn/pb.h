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

// Minimal protobuf wire-format codec for RPCN's np2_structs.proto messages.
// Messages are encoded and decoded by field number; see the .proto in the RPCN repository.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rpcn::pb {

enum WireType : uint8_t {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    Fixed32 = 5,
};

class Writer {
public:
    Writer &u64(uint32_t field, uint64_t value) {
        if (value) {
            tag(field, Varint);
            varint(value);
        }
        return *this;
    }
    Writer &u32(uint32_t field, uint32_t value) { return u64(field, value); }
    Writer &i32(uint32_t field, int32_t value) { return u64(field, static_cast<uint64_t>(static_cast<int64_t>(value))); }
    Writer &i64(uint32_t field, int64_t value) { return u64(field, static_cast<uint64_t>(value)); }
    Writer &boolean(uint32_t field, bool value) { return u64(field, value ? 1 : 0); }

    // Fields that are always written, even when zero (elements of repeated scalars)
    Writer &u64_always(uint32_t field, uint64_t value) {
        tag(field, Varint);
        varint(value);
        return *this;
    }

    Writer &bytes(uint32_t field, const void *data, size_t size) {
        tag(field, LengthDelimited);
        varint(size);
        const auto *p = static_cast<const uint8_t *>(data);
        buf.insert(buf.end(), p, p + size);
        return *this;
    }
    Writer &string(uint32_t field, std::string_view str) { return bytes(field, str.data(), str.size()); }
    Writer &message(uint32_t field, const Writer &msg) { return bytes(field, msg.buf.data(), msg.buf.size()); }

    // RPCN wraps u8/u16 values in { uint32 value = 1; } messages
    Writer &wrapped(uint32_t field, uint32_t value) {
        Writer w;
        w.u32(1, value);
        return message(field, w);
    }

    const std::vector<uint8_t> &data() const { return buf; }

private:
    void tag(uint32_t field, WireType type) { varint((static_cast<uint64_t>(field) << 3) | type); }
    void varint(uint64_t value) {
        while (value >= 0x80) {
            buf.push_back(static_cast<uint8_t>(value | 0x80));
            value >>= 7;
        }
        buf.push_back(static_cast<uint8_t>(value));
    }

    std::vector<uint8_t> buf;
};

class Reader {
public:
    struct Field {
        uint32_t number;
        WireType type;
        uint64_t value; // varint / fixed
        const uint8_t *data; // length-delimited
        size_t size;
    };

    Reader() = default;
    Reader(const uint8_t *data, size_t size) { parse(data, size); }
    explicit Reader(const std::vector<uint8_t> &data) { parse(data.data(), data.size()); }

    bool ok() const { return valid; }

    uint64_t u64(uint32_t field, uint64_t def = 0) const {
        const Field *f = last(field);
        return f && f->type != LengthDelimited ? f->value : def;
    }
    uint32_t u32(uint32_t field, uint32_t def = 0) const { return static_cast<uint32_t>(u64(field, def)); }
    int32_t i32(uint32_t field, int32_t def = 0) const { return static_cast<int32_t>(u64(field, static_cast<uint64_t>(def))); }
    bool boolean(uint32_t field) const { return u64(field) != 0; }

    std::string string(uint32_t field) const {
        const Field *f = last(field);
        return f && f->type == LengthDelimited ? std::string(reinterpret_cast<const char *>(f->data), f->size) : std::string{};
    }
    std::vector<uint8_t> bytes(uint32_t field) const {
        const Field *f = last(field);
        return f && f->type == LengthDelimited ? std::vector<uint8_t>(f->data, f->data + f->size) : std::vector<uint8_t>{};
    }
    bool has(uint32_t field) const { return last(field) != nullptr; }

    Reader message(uint32_t field) const {
        const Field *f = last(field);
        return f && f->type == LengthDelimited ? Reader(f->data, f->size) : Reader{};
    }
    // Value of a { uint32 value = 1; } wrapper message
    uint32_t wrapped(uint32_t field, uint32_t def = 0) const {
        return has(field) ? message(field).u32(1) : def;
    }

    std::vector<Reader> messages(uint32_t field) const {
        std::vector<Reader> out;
        for (const auto &f : fields)
            if (f.number == field && f.type == LengthDelimited)
                out.emplace_back(f.data, f.size);
        return out;
    }

    std::vector<std::string> strings(uint32_t field) const {
        std::vector<std::string> out;
        for (const auto &f : fields)
            if (f.number == field && f.type == LengthDelimited)
                out.emplace_back(reinterpret_cast<const char *>(f.data), f.size);
        return out;
    }

    // Repeated scalar, packed or not
    std::vector<uint64_t> repeated_u64(uint32_t field) const {
        std::vector<uint64_t> out;
        for (const auto &f : fields) {
            if (f.number != field)
                continue;
            if (f.type == Varint) {
                out.push_back(f.value);
            } else if (f.type == LengthDelimited) {
                size_t pos = 0;
                while (pos < f.size) {
                    uint64_t v;
                    if (!read_varint(f.data, f.size, pos, v))
                        break;
                    out.push_back(v);
                }
            }
        }
        return out;
    }

private:
    const Field *last(uint32_t field) const {
        for (auto it = fields.rbegin(); it != fields.rend(); ++it)
            if (it->number == field)
                return &*it;
        return nullptr;
    }

    static bool read_varint(const uint8_t *data, size_t size, size_t &pos, uint64_t &out) {
        out = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (pos >= size)
                return false;
            const uint8_t b = data[pos++];
            out |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80))
                return true;
        }
        return false;
    }

    void parse(const uint8_t *data, size_t size) {
        size_t pos = 0;
        while (pos < size) {
            uint64_t key;
            if (!read_varint(data, size, pos, key)) {
                valid = false;
                return;
            }
            Field f{ static_cast<uint32_t>(key >> 3), static_cast<WireType>(key & 7), 0, nullptr, 0 };
            switch (f.type) {
            case Varint:
                if (!read_varint(data, size, pos, f.value)) {
                    valid = false;
                    return;
                }
                break;
            case Fixed64:
                if (pos + 8 > size) {
                    valid = false;
                    return;
                }
                std::memcpy(&f.value, data + pos, 8);
                pos += 8;
                break;
            case Fixed32: {
                if (pos + 4 > size) {
                    valid = false;
                    return;
                }
                uint32_t v;
                std::memcpy(&v, data + pos, 4);
                f.value = v;
                pos += 4;
                break;
            }
            case LengthDelimited: {
                uint64_t len;
                if (!read_varint(data, size, pos, len) || pos + len > size) {
                    valid = false;
                    return;
                }
                f.data = data + pos;
                f.size = static_cast<size_t>(len);
                pos += f.size;
                break;
            }
            default:
                valid = false;
                return;
            }
            fields.push_back(f);
        }
    }

    std::vector<Field> fields;
    bool valid = true;
};

} // namespace rpcn::pb
