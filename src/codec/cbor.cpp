#include "cbor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace pime::codec {

Value Value::boolean_v(bool v) {
    Value x;
    x.type = Type::Bool;
    x.boolean = v;
    return x;
}

Value Value::uint_v(uint64_t v) {
    Value x;
    x.type = Type::UInt;
    x.uint_value = v;
    return x;
}

Value Value::int_v(int64_t v) {
    Value x;
    if (v >= 0) {
        x.type = Type::UInt;
        x.uint_value = static_cast<uint64_t>(v);
    } else {
        x.type = Type::Int;
        x.int_value = v;
    }
    return x;
}

Value Value::float_v(double v) {
    Value x;
    x.type = Type::Float;
    x.float_value = v;
    return x;
}

Value Value::bytes(std::string_view v) {
    Value x;
    x.type = Type::Bytes;
    x.text.assign(v.data(), v.size());
    return x;
}

Value Value::text_v(std::string_view v) {
    Value x;
    x.type = Type::Text;
    x.text.assign(v.data(), v.size());
    return x;
}

Value Value::array_v() {
    Value x;
    x.type = Type::Array;
    return x;
}

Value Value::map_v() {
    Value x;
    x.type = Type::Map;
    return x;
}

const Value* Value::find(std::string_view key) const {
    if (type != Type::Map) return nullptr;
    for (const auto& kv : map) {
        if (kv.first.type == Type::Text && kv.first.text == key) return &kv.second;
    }
    return nullptr;
}

Value* Value::find(std::string_view key) {
    if (type != Type::Map) return nullptr;
    for (auto& kv : map) {
        if (kv.first.type == Type::Text && kv.first.text == key) return &kv.second;
    }
    return nullptr;
}

int64_t Value::as_int() const {
    switch (type) {
        case Type::UInt: return static_cast<int64_t>(uint_value);
        case Type::Int: return int_value;
        case Type::Float: return static_cast<int64_t>(float_value);
        case Type::Bool: return boolean ? 1 : 0;
        default: return 0;
    }
}

uint64_t Value::as_uint() const {
    switch (type) {
        case Type::UInt: return uint_value;
        case Type::Int: return static_cast<uint64_t>(int_value);
        case Type::Float: return static_cast<uint64_t>(float_value);
        case Type::Bool: return boolean ? 1 : 0;
        default: return 0;
    }
}

double Value::as_double() const {
    switch (type) {
        case Type::UInt: return static_cast<double>(uint_value);
        case Type::Int: return static_cast<double>(int_value);
        case Type::Float: return float_value;
        case Type::Bool: return boolean ? 1.0 : 0.0;
        default: return 0.0;
    }
}

bool Value::as_bool() const {
    switch (type) {
        case Type::Bool: return boolean;
        case Type::UInt: return uint_value != 0;
        case Type::Int: return int_value != 0;
        case Type::Float: return float_value != 0.0;
        case Type::Null: return false;
        default: return !text.empty() || !array.empty() || !map.empty();
    }
}

Value& Value::set(std::string_view key, Value v) {
    if (type != Type::Map) {
        type = Type::Map;
        map.clear();
    }
    for (auto& kv : map) {
        if (kv.first.type == Type::Text && kv.first.text == key) {
            kv.second = std::move(v);
            return *this;
        }
    }
    map.emplace_back(Value::text_v(key), std::move(v));
    return *this;
}

Value& Value::push(Value v) {
    if (type != Type::Array) {
        type = Type::Array;
        array.clear();
    }
    array.push_back(std::move(v));
    return *this;
}

bool Value::equals(const Value& other) const {
    if (type != other.type) return false;
    switch (type) {
        case Type::Null: return true;
        case Type::Bool: return boolean == other.boolean;
        case Type::UInt: return uint_value == other.uint_value;
        case Type::Int: return int_value == other.int_value;
        case Type::Float: return float_value == other.float_value;
        case Type::Text:
        case Type::Bytes: return text == other.text;
        case Type::Array:
            if (array.size() != other.array.size()) return false;
            for (size_t i = 0; i < array.size(); ++i)
                if (!array[i].equals(other.array[i])) return false;
            return true;
        case Type::Map: {
            if (map.size() != other.map.size()) return false;
            for (const auto& kv : map) {
                if (kv.first.type != Type::Text) return false;
                const Value* v = other.find(kv.first.text);
                if (!v || !kv.second.equals(*v)) return false;
            }
            return true;
        }
    }
    return false;
}

namespace {

constexpr size_t kMaxDepth = 64;
constexpr uint64_t kMaxContainer = 16u * 1024u * 1024u;

class Decoder {
public:
    Decoder(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    pime_status run(Value* out) {
        pos_ = 0;
        Value v;
        pime_status s = parse(&v, 0);
        if (s != PIME_OK) return s;
        if (pos_ != len_) return PIME_CORRUPT;
        *out = std::move(v);
        return PIME_OK;
    }

private:
    pime_status read_u8(uint8_t* v) {
        if (pos_ >= len_) return PIME_CORRUPT;
        *v = data_[pos_++];
        return PIME_OK;
    }

    pime_status read_arg(uint8_t info, uint64_t* out) {
        if (info < 24) {
            *out = info;
            return PIME_OK;
        }
        size_t bytes = 0;
        switch (info) {
            case 24: bytes = 1; break;
            case 25: bytes = 2; break;
            case 26: bytes = 4; break;
            case 27: bytes = 8; break;
            default: return PIME_CORRUPT; /* indefinite or reserved */
        }
        if (pos_ + bytes > len_) return PIME_CORRUPT;
        uint64_t v = 0;
        for (size_t i = 0; i < bytes; ++i) v = (v << 8) | data_[pos_++];
        *out = v;
        return PIME_OK;
    }

    pime_status parse(Value* out, size_t depth) {
        if (depth > kMaxDepth) return PIME_CORRUPT;
        uint8_t initial;
        pime_status s = read_u8(&initial);
        if (s != PIME_OK) return s;
        const uint8_t major = initial >> 5;
        const uint8_t info = initial & 0x1F;
        uint64_t arg = 0;

        switch (major) {
            case 0:
                if ((s = read_arg(info, &arg)) != PIME_OK) return s;
                *out = Value::uint_v(arg);
                return PIME_OK;
            case 1:
                if ((s = read_arg(info, &arg)) != PIME_OK) return s;
                if (arg > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    return PIME_CORRUPT;
                *out = Value::int_v(-1 - static_cast<int64_t>(arg));
                return PIME_OK;
            case 2:
            case 3: {
                if ((s = read_arg(info, &arg)) != PIME_OK) return s;
                if (arg > kMaxContainer || arg > len_ - pos_) return PIME_CORRUPT;
                Value v = major == 2
                              ? Value::bytes(std::string_view(
                                    reinterpret_cast<const char*>(data_ + pos_),
                                    static_cast<size_t>(arg)))
                              : Value::text_v(std::string_view(
                                    reinterpret_cast<const char*>(data_ + pos_),
                                    static_cast<size_t>(arg)));
                pos_ += static_cast<size_t>(arg);
                *out = std::move(v);
                return PIME_OK;
            }
            case 4: {
                if ((s = read_arg(info, &arg)) != PIME_OK) return s;
                if (arg > kMaxContainer) return PIME_CORRUPT;
                Value v = Value::array_v();
                v.array.reserve(static_cast<size_t>(arg));
                for (uint64_t i = 0; i < arg; ++i) {
                    Value item;
                    if ((s = parse(&item, depth + 1)) != PIME_OK) return s;
                    v.array.push_back(std::move(item));
                }
                *out = std::move(v);
                return PIME_OK;
            }
            case 5: {
                if ((s = read_arg(info, &arg)) != PIME_OK) return s;
                if (arg > kMaxContainer) return PIME_CORRUPT;
                Value v = Value::map_v();
                v.map.reserve(static_cast<size_t>(arg));
                for (uint64_t i = 0; i < arg; ++i) {
                    Value k, val;
                    if ((s = parse(&k, depth + 1)) != PIME_OK) return s;
                    if ((s = parse(&val, depth + 1)) != PIME_OK) return s;
                    v.map.emplace_back(std::move(k), std::move(val));
                }
                *out = std::move(v);
                return PIME_OK;
            }
            case 6: {
                /* Tags are not used by the wire protocol; skip the tag and
                 * decode the content unchanged. */
                if ((s = read_arg(info, &arg)) != PIME_OK) return s;
                return parse(out, depth + 1);
            }
            case 7: {
                if (info == 20) { *out = Value::boolean_v(false); return PIME_OK; }
                if (info == 21) { *out = Value::boolean_v(true); return PIME_OK; }
                if (info == 22) { *out = Value::null(); return PIME_OK; }
                if (info == 25) {
                    if (pos_ + 2 > len_) return PIME_CORRUPT;
                    uint16_t half = static_cast<uint16_t>(data_[pos_++] << 8);
                    half |= data_[pos_++];
                    const int exp = (half >> 10) & 0x1F;
                    const int mant = half & 0x3FF;
                    double val;
                    if (exp == 0) val = mant * 5.960464477539063e-8;
                    else if (exp == 31) val = mant ? std::numeric_limits<double>::quiet_NaN()
                                                   : std::numeric_limits<double>::infinity();
                    else val = (mant + 1024) * std::pow(2.0, exp - 25);
                    *out = Value::float_v(val);
                    return PIME_OK;
                }
                if (info == 26) {
                    if (pos_ + 4 > len_) return PIME_CORRUPT;
                    uint32_t bits = 0;
                    for (int i = 0; i < 4; ++i) bits = (bits << 8) | data_[pos_++];
                    float f;
                    std::memcpy(&f, &bits, sizeof(f));
                    *out = Value::float_v(f);
                    return PIME_OK;
                }
                if (info == 27) {
                    if (pos_ + 8 > len_) return PIME_CORRUPT;
                    uint64_t bits = 0;
                    for (int i = 0; i < 8; ++i) bits = (bits << 8) | data_[pos_++];
                    double d;
                    std::memcpy(&d, &bits, sizeof(d));
                    *out = Value::float_v(d);
                    return PIME_OK;
                }
                return PIME_CORRUPT;
            }
            default:
                return PIME_CORRUPT;
        }
    }

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
};

void write_head(std::vector<uint8_t>* out, uint8_t major, uint64_t value) {
    const uint8_t m = static_cast<uint8_t>(major << 5);
    if (value < 24) {
        out->push_back(static_cast<uint8_t>(m | value));
    } else if (value < 0x100) {
        out->push_back(static_cast<uint8_t>(m | 24));
        out->push_back(static_cast<uint8_t>(value));
    } else if (value < 0x10000) {
        out->push_back(static_cast<uint8_t>(m | 25));
        out->push_back(static_cast<uint8_t>(value >> 8));
        out->push_back(static_cast<uint8_t>(value));
    } else if (value < 0x100000000ull) {
        out->push_back(static_cast<uint8_t>(m | 26));
        for (int i = 3; i >= 0; --i) out->push_back(static_cast<uint8_t>(value >> (i * 8)));
    } else {
        out->push_back(static_cast<uint8_t>(m | 27));
        for (int i = 7; i >= 0; --i) out->push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

void encode_impl(const Value& v, std::vector<uint8_t>* out);

std::vector<const std::pair<Value, Value>*> sorted_map(const Value& v) {
    std::vector<const std::pair<Value, Value>*> items;
    items.reserve(v.map.size());
    for (const auto& kv : v.map) items.push_back(&kv);
    std::sort(items.begin(), items.end(), [](const auto* a, const auto* b) {
        if (a->first.type != b->first.type)
            return static_cast<int>(a->first.type) < static_cast<int>(b->first.type);
        if (a->first.type == Type::Text) return a->first.text < b->first.text;
        if (a->first.type == Type::UInt) return a->first.uint_value < b->first.uint_value;
        if (a->first.type == Type::Int) return a->first.int_value < b->first.int_value;
        return false;
    });
    return items;
}

void encode_canonical_impl(const Value& v, std::vector<uint8_t>* out) {
    switch (v.type) {
        case Type::Map: {
            auto items = sorted_map(v);
            write_head(out, 5, items.size());
            for (const auto* kv : items) {
                encode_canonical_impl(kv->first, out);
                encode_canonical_impl(kv->second, out);
            }
            return;
        }
        case Type::Array:
            write_head(out, 4, v.array.size());
            for (const auto& item : v.array) encode_canonical_impl(item, out);
            return;
        default:
            encode_impl(v, out);
            return;
    }
}

void encode_impl(const Value& v, std::vector<uint8_t>* out) {
    switch (v.type) {
        case Type::Null:
            out->push_back(0xF6);
            break;
        case Type::Bool:
            out->push_back(v.boolean ? 0xF5 : 0xF4);
            break;
        case Type::UInt:
            write_head(out, 0, v.uint_value);
            break;
        case Type::Int: {
            if (v.int_value >= 0) {
                write_head(out, 0, static_cast<uint64_t>(v.int_value));
            } else {
                write_head(out, 1, static_cast<uint64_t>(-1 - v.int_value));
            }
            break;
        }
        case Type::Float: {
            out->push_back(0xFB);
            uint64_t bits;
            std::memcpy(&bits, &v.float_value, sizeof(bits));
            for (int i = 7; i >= 0; --i) out->push_back(static_cast<uint8_t>(bits >> (i * 8)));
            break;
        }
        case Type::Bytes:
            write_head(out, 2, v.text.size());
            out->insert(out->end(), v.text.begin(), v.text.end());
            break;
        case Type::Text:
            write_head(out, 3, v.text.size());
            out->insert(out->end(), v.text.begin(), v.text.end());
            break;
        case Type::Array:
            write_head(out, 4, v.array.size());
            for (const auto& item : v.array) encode_impl(item, out);
            break;
        case Type::Map:
            write_head(out, 5, v.map.size());
            for (const auto& kv : v.map) {
                encode_impl(kv.first, out);
                encode_impl(kv.second, out);
            }
            break;
    }
}

}  // namespace

pime_status decode(const uint8_t* data, size_t len, Value* out) {
    if (!out || (!data && len != 0)) return PIME_INVALID;
    Decoder decoder(data ? data : reinterpret_cast<const uint8_t*>(""), len);
    return decoder.run(out);
}

pime_status decode(std::string_view data, Value* out) {
    return decode(reinterpret_cast<const uint8_t*>(data.data()), data.size(), out);
}

void encode(const Value& value, std::vector<uint8_t>* out) {
    out->clear();
    encode_impl(value, out);
}

std::vector<uint8_t> encode_to_vector(const Value& value) {
    std::vector<uint8_t> out;
    encode(value, &out);
    return out;
}

void encode_canonical(const Value& value, std::vector<uint8_t>* out) {
    out->clear();
    encode_canonical_impl(value, out);
}

std::string to_debug_string(const Value& value) {
    switch (value.type) {
        case Type::Null: return "null";
        case Type::Bool: return value.boolean ? "true" : "false";
        case Type::UInt: return std::to_string(value.uint_value);
        case Type::Int: return std::to_string(value.int_value);
        case Type::Float: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", value.float_value);
            return buf;
        }
        case Type::Bytes: {
            std::string out = "h'";
            static const char* digits = "0123456789abcdef";
            for (unsigned char c : value.text) {
                out.push_back(digits[c >> 4]);
                out.push_back(digits[c & 0xF]);
            }
            out.push_back('\'');
            return out;
        }
        case Type::Text: return "\"" + value.text + "\"";
        case Type::Array: {
            std::string out = "[";
            for (size_t i = 0; i < value.array.size(); ++i) {
                if (i) out += ",";
                out += to_debug_string(value.array[i]);
            }
            return out + "]";
        }
        case Type::Map: {
            std::string out = "{";
            for (size_t i = 0; i < value.map.size(); ++i) {
                if (i) out += ",";
                out += to_debug_string(value.map[i].first);
                out += ":";
                out += to_debug_string(value.map[i].second);
            }
            return out + "}";
        }
    }
    return "?";
}

}  // namespace pime::codec
