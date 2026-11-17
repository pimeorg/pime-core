#ifndef PIME_CODEC_CBOR_HPP
#define PIME_CODEC_CBOR_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pime::codec {

enum class Type : uint8_t { Null, Bool, UInt, Int, Float, Bytes, Text, Array, Map };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    uint64_t uint_value = 0;
    int64_t int_value = 0;
    double float_value = 0.0;
    std::string text; /* Text or Bytes */
    std::vector<Value> array;
    std::vector<std::pair<Value, Value>> map;

    Value() = default;

    static Value null() { return Value(); }
    static Value boolean_v(bool v);
    static Value uint_v(uint64_t v);
    static Value int_v(int64_t v);
    static Value float_v(double v);
    static Value bytes(std::string_view v);
    static Value text_v(std::string_view v);
    static Value array_v();
    static Value map_v();

    bool is_null() const { return type == Type::Null; }
    bool is_map() const { return type == Type::Map; }
    bool is_array() const { return type == Type::Array; }
    bool is_text() const { return type == Type::Text; }
    bool is_bytes() const { return type == Type::Bytes; }
    bool is_number() const {
        return type == Type::UInt || type == Type::Int || type == Type::Float;
    }

    /* Text map access. Returns nullptr when absent or not a map. */
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool has(std::string_view key) const { return find(key) != nullptr; }

    std::string_view as_text() const {
        return type == Type::Text || type == Type::Bytes
                   ? std::string_view(text)
                   : std::string_view{};
    }
    std::string_view as_bytes() const { return as_text(); }
    int64_t as_int() const;
    uint64_t as_uint() const;
    double as_double() const;
    bool as_bool() const;

    /* Map/array builders. */
    Value& set(std::string_view key, Value v);
    Value& push(Value v);

    /* Deep equality; map order insensitive. */
    bool equals(const Value& other) const;
};

pime_status decode(const uint8_t* data, size_t len, Value* out);
pime_status decode(std::string_view data, Value* out);

void encode(const Value& value, std::vector<uint8_t>* out);

/* Appends one encoded value to `out` instead of replacing its contents. Lets
 * callers assemble a frame around a pre-encoded workspace without copying it. */
void encode_append(const Value& value, std::vector<uint8_t>* out);
std::vector<uint8_t> encode_to_vector(const Value& value);

/* Canonical key-sorted encoding (RFC 8949 core deterministic subset). */
void encode_canonical(const Value& value, std::vector<uint8_t>* out);

std::string to_debug_string(const Value& value);

}  // namespace pime::codec

#endif
