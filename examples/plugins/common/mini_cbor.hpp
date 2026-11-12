/*
 * Minimal definite-length CBOR for the reference plugins.
 *
 * This is example code, not part of pime-core. It implements only the subset
 * the plugins use: uint/int, bool, null, text, arrays and string-keyed maps.
 * Plugins are free to use any CBOR library; nothing here is ABI.
 */
#ifndef PIME_EXAMPLE_MINI_CBOR_HPP
#define PIME_EXAMPLE_MINI_CBOR_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace example {

struct Node {
    enum class Type { Null, Bool, UInt, Int, Text, Array, Map };

    Type type = Type::Null;
    bool boolean = false;
    uint64_t uint_value = 0;
    int64_t int_value = 0;
    std::string text;
    std::vector<Node> array;
    std::vector<std::pair<std::string, Node>> map;

    static Node null() { return Node(); }
    static Node boolean_v(bool v) { Node n; n.type = Type::Bool; n.boolean = v; return n; }
    static Node uint_v(uint64_t v) { Node n; n.type = Type::UInt; n.uint_value = v; return n; }
    static Node int_v(int64_t v) { Node n; n.type = Type::Int; n.int_value = v; return n; }
    static Node text_v(std::string_view v) { Node n; n.type = Type::Text; n.text.assign(v.data(), v.size()); return n; }
    static Node array_v() { Node n; n.type = Type::Array; return n; }
    static Node map_v() { Node n; n.type = Type::Map; return n; }

    bool is_text() const { return type == Type::Text; }
    bool is_map() const { return type == Type::Map; }
    bool is_uint() const { return type == Type::UInt; }

    const Node* find(std::string_view key) const {
        if (type != Type::Map) return nullptr;
        for (const auto& entry : map)
            if (entry.first == key) return &entry.second;
        return nullptr;
    }

    Node& set(std::string_view key, Node value) {
        map.emplace_back(std::string(key), std::move(value));
        return *this;
    }

    Node& push(Node value) {
        array.push_back(std::move(value));
        return *this;
    }
};

namespace detail {

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool done() const { return pos_ == size_; }

    bool read(Node* out) {
        uint8_t initial = 0;
        if (!byte(&initial)) return false;
        const uint8_t major = initial >> 5;
        uint64_t value = 0;
        if (!length(initial & 0x1f, &value)) return false;

        switch (major) {
            case 0:
                *out = Node::uint_v(value);
                return true;
            case 1:
                *out = Node::int_v(-1 - static_cast<int64_t>(value));
                return true;
            case 2:
            case 3: {
                if (value > size_ - pos_) return false;
                Node node = Node::text_v(std::string_view(
                    reinterpret_cast<const char*>(data_ + pos_),
                    static_cast<size_t>(value)));
                pos_ += static_cast<size_t>(value);
                *out = std::move(node);
                return true;
            }
            case 4: {
                Node node = Node::array_v();
                for (uint64_t i = 0; i < value; ++i) {
                    Node item;
                    if (!read(&item)) return false;
                    node.array.push_back(std::move(item));
                }
                *out = std::move(node);
                return true;
            }
            case 5: {
                Node node = Node::map_v();
                for (uint64_t i = 0; i < value; ++i) {
                    Node key;
                    Node item;
                    if (!read(&key) || !read(&item)) return false;
                    node.map.emplace_back(
                        key.is_text() ? key.text : std::string(), std::move(item));
                }
                *out = std::move(node);
                return true;
            }
            case 6: {
                Node tagged;
                return read(&tagged) && (*out = std::move(tagged), true);
            }
            case 7:
                switch (initial & 0x1f) {
                    case 20: *out = Node::boolean_v(false); return true;
                    case 21: *out = Node::boolean_v(true); return true;
                    case 22: *out = Node::null(); return true;
                    default: return false;
                }
            default:
                return false;
        }
    }

private:
    bool byte(uint8_t* out) {
        if (pos_ >= size_) return false;
        *out = data_[pos_++];
        return true;
    }

    bool length(uint8_t info, uint64_t* out) {
        if (info < 24) {
            *out = info;
            return true;
        }
        const int count = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 0;
        if (!count || size_ - pos_ < static_cast<size_t>(count)) return false;
        uint64_t value = 0;
        for (int i = 0; i < count; ++i) value = (value << 8) | data_[pos_++];
        *out = value;
        return true;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

inline void write_head(std::vector<uint8_t>* out, uint8_t major, uint64_t value) {
    if (value < 24) {
        out->push_back(static_cast<uint8_t>((major << 5) | value));
    } else if (value <= 0xff) {
        out->push_back(static_cast<uint8_t>((major << 5) | 24));
        out->push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        out->push_back(static_cast<uint8_t>((major << 5) | 25));
        out->push_back(static_cast<uint8_t>(value >> 8));
        out->push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffffffffull) {
        out->push_back(static_cast<uint8_t>((major << 5) | 26));
        for (int shift = 24; shift >= 0; shift -= 8)
            out->push_back(static_cast<uint8_t>(value >> shift));
    } else {
        out->push_back(static_cast<uint8_t>((major << 5) | 27));
        for (int shift = 56; shift >= 0; shift -= 8)
            out->push_back(static_cast<uint8_t>(value >> shift));
    }
}

}  // namespace detail

inline bool decode(const uint8_t* data, size_t size, Node* out) {
    detail::Reader reader(data, size);
    return reader.read(out) && reader.done();
}

inline void encode(const Node& node, std::vector<uint8_t>* out) {
    switch (node.type) {
        case Node::Type::Null:
            out->push_back(0xf6);
            break;
        case Node::Type::Bool:
            out->push_back(node.boolean ? 0xf5 : 0xf4);
            break;
        case Node::Type::UInt:
            detail::write_head(out, 0, node.uint_value);
            break;
        case Node::Type::Int:
            detail::write_head(out, 1,
                               static_cast<uint64_t>(-1 - node.int_value));
            break;
        case Node::Type::Text:
            detail::write_head(out, 3, node.text.size());
            out->insert(out->end(), node.text.begin(), node.text.end());
            break;
        case Node::Type::Array:
            detail::write_head(out, 4, node.array.size());
            for (const Node& item : node.array) encode(item, out);
            break;
        case Node::Type::Map:
            detail::write_head(out, 5, node.map.size());
            for (const auto& entry : node.map) {
                detail::write_head(out, 3, entry.first.size());
                out->insert(out->end(), entry.first.begin(), entry.first.end());
                encode(entry.second, out);
            }
            break;
    }
}

}  // namespace example

#endif /* PIME_EXAMPLE_MINI_CBOR_HPP */
