#include "yaml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "util/string_util.hpp"
#include "util/utf8.hpp"

namespace pime::codec {

namespace {

struct Line {
    int indent = 0;
    std::string text;
};

bool is_seq_marker(std::string_view s) {
    return s == "-" || (s.size() >= 2 && s[0] == '-' && std::isspace(static_cast<unsigned char>(s[1])));
}

std::string strip_comment(std::string_view line) {
    bool single = false, double_q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\'' && !double_q) single = !single;
        else if (c == '"' && !single) {
            if (i > 0 && line[i - 1] == '\\' && double_q) continue;
            double_q = !double_q;
        } else if (c == '#' && !single && !double_q) {
            if (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))
                return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

/* A flow collection ('[' or '{') only starts continuation folding when it
 * appears at a value position (document start, after ':' or after '-'), so
 * brackets inside plain text such as descriptions stay literal. */
bool opens_flow(std::string_view s, size_t at) {
    size_t i = at;
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1]))) --i;
    if (i == 0) return true;
    return s[i - 1] == ':' || s[i - 1] == '-';
}

/* Returns the flow nesting depth at the end of the line. Quotes are tracked
 * only inside a flow collection, so apostrophes in plain scalars elsewhere do
 * not swallow following lines. */
int scan_flow(std::string_view s, int depth, bool* single, bool* double_q) {
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (depth == 0) {
            if ((c == '[' || c == '{') && opens_flow(s, i)) depth = 1;
            continue;
        }
        if (c == '\'' && !*double_q) {
            *single = !*single;
        } else if (c == '"' && !*single) {
            if (!(i > 0 && s[i - 1] == '\\' && *double_q)) *double_q = !*double_q;
        } else if (*single || *double_q) {
            continue;
        } else if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
            if (--depth == 0) {
                *single = false;
                *double_q = false;
            }
        }
    }
    return depth;
}

pime_status preprocess(std::string_view input, std::vector<Line>* out,
                       std::string* error) {
    size_t start = 0;
    std::string pending;
    int pending_indent = 0;
    int depth = 0;
    bool single = false, double_q = false;
    while (start <= input.size()) {
        size_t end = input.find('\n', start);
        if (end == std::string_view::npos) end = input.size();
        std::string_view raw = input.substr(start, end - start);
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        int indent = 0;
        size_t i = 0;
        for (; i < raw.size(); ++i) {
            if (raw[i] == ' ') ++indent;
            else if (raw[i] == '\t') {
                *error = "tab indentation is not supported";
                return PIME_INVALID;
            } else break;
        }
        std::string content = strip_comment(raw.substr(i));
        while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back())))
            content.pop_back();
        if (!content.empty()) {
            if (pending.empty()) {
                pending_indent = indent;
                pending = content;
            } else {
                /* YAML folds a line break inside a flow collection into a
                 * single space. */
                pending.push_back(' ');
                pending += content;
            }
            depth = scan_flow(content, depth, &single, &double_q);
            if (depth == 0 && !single && !double_q) {
                out->push_back(Line{pending_indent, std::move(pending)});
                pending.clear();
            }
        }
        if (end == input.size()) break;
        start = end + 1;
    }
    if (!pending.empty()) {
        *error = "unterminated flow collection";
        return PIME_INVALID;
    }
    return PIME_OK;
}

pime_status parse_quoted(std::string_view s, size_t* pos, std::string* out);

pime_status parse_scalar(std::string_view raw, Value* out) {
    std::string_view s = trim_view(raw);
    if (s.empty()) {
        *out = Value::null();
        return PIME_OK;
    }
    if (s.front() == '"' || s.front() == '\'') {
        size_t pos = 0;
        std::string text;
        pime_status st = parse_quoted(s, &pos, &text);
        if (st != PIME_OK) return st;
        if (pos != s.size()) return PIME_INVALID;
        *out = Value::text_v(text);
        return PIME_OK;
    }
    if (s == "~" || s == "null" || s == "Null" || s == "NULL") {
        *out = Value::null();
        return PIME_OK;
    }
    if (s == "true" || s == "True" || s == "TRUE") {
        *out = Value::boolean_v(true);
        return PIME_OK;
    }
    if (s == "false" || s == "False" || s == "FALSE") {
        *out = Value::boolean_v(false);
        return PIME_OK;
    }
    {
        int64_t iv;
        if (parse_i64(s, &iv)) {
            *out = Value::int_v(iv);
            return PIME_OK;
        }
    }
    {
        std::string tmp(s);
        char* end = nullptr;
        double d = std::strtod(tmp.c_str(), &end);
        if (end && *end == '\0' && end != tmp.c_str() &&
            tmp.find_first_of("0123456789") != std::string::npos &&
            tmp.find('.') != std::string::npos) {
            *out = Value::float_v(d);
            return PIME_OK;
        }
    }
    *out = Value::text_v(s);
    return PIME_OK;
}

pime_status parse_quoted(std::string_view s, size_t* pos, std::string* out) {
    if (*pos >= s.size()) return PIME_INVALID;
    const char quote = s[*pos];
    if (quote != '"' && quote != '\'') return PIME_INVALID;
    ++*pos;
    out->clear();
    while (*pos < s.size()) {
        char c = s[*pos];
        if (c == quote) {
            if (quote == '\'' && *pos + 1 < s.size() && s[*pos + 1] == '\'') {
                out->push_back('\'');
                *pos += 2;
                continue;
            }
            ++*pos;
            return PIME_OK;
        }
        if (quote == '"' && c == '\\') {
            ++*pos;
            if (*pos >= s.size()) return PIME_INVALID;
            char e = s[*pos];
            switch (e) {
                case 'n': out->push_back('\n'); break;
                case 't': out->push_back('\t'); break;
                case 'r': out->push_back('\r'); break;
                case '0': out->push_back('\0'); break;
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'u': {
                    if (*pos + 4 >= s.size()) return PIME_INVALID;
                    char32_t cp = 0;
                    for (int i = 1; i <= 4; ++i) {
                        char h = s[*pos + i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        else return PIME_INVALID;
                    }
                    utf8::append(*out, cp);
                    *pos += 4;
                    break;
                }
                default: return PIME_INVALID;
            }
            ++*pos;
            continue;
        }
        out->push_back(c);
        ++*pos;
    }
    return PIME_INVALID;
}

class FlowParser {
public:
    FlowParser(std::string_view s, std::string* error) : s_(s), error_(error) {}

    pime_status parse(Value* out) {
        pos_ = 0;
        pime_status st = parse_value(out);
        if (st != PIME_OK) return st;
        skip_ws();
        return pos_ == s_.size() ? PIME_OK : PIME_INVALID;
    }

private:
    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])))
            ++pos_;
    }

    pime_status parse_value(Value* out) {
        skip_ws();
        if (pos_ >= s_.size()) return PIME_INVALID;
        char c = s_[pos_];
        if (c == '{') return parse_map(out);
        if (c == '[') return parse_seq(out);
        if (c == '"' || c == '\'') {
            std::string text;
            pime_status st = parse_quoted(s_, &pos_, &text);
            if (st != PIME_OK) return st;
            *out = Value::text_v(text);
            return PIME_OK;
        }
        size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' &&
               s_[pos_] != ']')
            ++pos_;
        return parse_scalar(s_.substr(start, pos_ - start), out);
    }

    pime_status parse_map(Value* out) {
        ++pos_; /* { */
        Value m = Value::map_v();
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            *out = std::move(m);
            return PIME_OK;
        }
        while (pos_ < s_.size()) {
            skip_ws();
            Value key;
            if (pos_ < s_.size() && (s_[pos_] == '"' || s_[pos_] == '\'')) {
                std::string text;
                pime_status st = parse_quoted(s_, &pos_, &text);
                if (st != PIME_OK) return st;
                key = Value::text_v(text);
            } else {
                size_t start = pos_;
                while (pos_ < s_.size() && s_[pos_] != ':' && s_[pos_] != ',' &&
                       s_[pos_] != '}')
                    ++pos_;
                pime_status st = parse_scalar(s_.substr(start, pos_ - start), &key);
                if (st != PIME_OK) return st;
                if (key.type != Type::Text) return PIME_INVALID;
            }
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return PIME_INVALID;
            ++pos_;
            Value value;
            pime_status st = parse_value(&value);
            if (st != PIME_OK) return st;
            m.set(key.text, std::move(value));
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                *out = std::move(m);
                return PIME_OK;
            }
            return PIME_INVALID;
        }
        return PIME_INVALID;
    }

    pime_status parse_seq(Value* out) {
        ++pos_;
        Value arr = Value::array_v();
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            *out = std::move(arr);
            return PIME_OK;
        }
        while (pos_ < s_.size()) {
            Value item;
            pime_status st = parse_value(&item);
            if (st != PIME_OK) return st;
            arr.push(std::move(item));
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                *out = std::move(arr);
                return PIME_OK;
            }
            return PIME_INVALID;
        }
        return PIME_INVALID;
    }

    std::string_view s_;
    std::string* error_;
    size_t pos_ = 0;
};

pime_status parse_inline(std::string_view text, Value* out) {
    std::string_view t = trim_view(text);
    if (t.empty()) {
        *out = Value::null();
        return PIME_OK;
    }
    if (t.front() == '{' || t.front() == '[') {
        std::string err;
        FlowParser parser(t, &err);
        return parser.parse(out);
    }
    return parse_scalar(t, out);
}

bool split_key_value(std::string_view text, std::string* key, std::string* value,
                     bool* has_value) {
    /* A flow collection is a whole value, never a 'key: value' item; the
     * first ':' inside it belongs to the flow mapping. */
    if (!text.empty() && (text.front() == '{' || text.front() == '['))
        return false;
    bool single = false, double_q = false;
    size_t i = 0;
    if (!text.empty() && (text.front() == '"' || text.front() == '\'')) {
        char q = text.front();
        size_t pos = 0;
        if (parse_quoted(text, &pos, key) != PIME_OK) return false;
        i = pos;
    } else {
        size_t colon = std::string_view::npos;
        for (; i < text.size(); ++i) {
            char c = text[i];
            if (c == ':' && (i + 1 == text.size() ||
                             std::isspace(static_cast<unsigned char>(text[i + 1])))) {
                colon = i;
                break;
            }
        }
        if (colon == std::string_view::npos) return false;
        *key = std::string(trim_view(text.substr(0, colon)));
        i = colon;
    }
    if (i >= text.size() || text[i] != ':') return false;
    ++i;
    std::string_view rest = trim_view(text.substr(i));
    *has_value = !rest.empty();
    *value = std::string(rest);
    return true;
}

struct Parser {
    std::vector<Line> lines;
    std::string error;

    pime_status parse_block(size_t* index, int indent, Value* out) {
        if (*index >= lines.size() || lines[*index].indent != indent)
            return PIME_INVALID;
        if (is_seq_marker(lines[*index].text)) return parse_sequence(index, indent, out);
        return parse_mapping(index, indent, out);
    }

    pime_status parse_mapping(size_t* index, int indent, Value* out) {
        Value m = Value::map_v();
        while (*index < lines.size() && lines[*index].indent == indent &&
               !is_seq_marker(lines[*index].text)) {
            std::string key, value;
            bool has_value = false;
            if (!split_key_value(lines[*index].text, &key, &value, &has_value)) {
                error = "expected 'key: value' at line " + std::to_string(*index + 1);
                return PIME_INVALID;
            }
            ++*index;
            Value v;
            if (has_value) {
                pime_status st = parse_inline(value, &v);
                if (st != PIME_OK) {
                    error = "invalid value at line " + std::to_string(*index);
                    return st;
                }
            } else if (*index < lines.size() && lines[*index].indent > indent) {
                pime_status st = parse_block(index, lines[*index].indent, &v);
                if (st != PIME_OK) return st;
            } else {
                v = Value::null();
            }
            m.set(key, std::move(v));
        }
        *out = std::move(m);
        return PIME_OK;
    }

    pime_status parse_sequence(size_t* index, int indent, Value* out) {
        Value arr = Value::array_v();
        while (*index < lines.size() && lines[*index].indent == indent &&
               is_seq_marker(lines[*index].text)) {
            std::string_view rest = trim_view(std::string_view(lines[*index].text).substr(1));
            ++*index;
            if (rest.empty()) {
                Value item;
                if (*index < lines.size() && lines[*index].indent > indent) {
                    pime_status st = parse_block(index, lines[*index].indent, &item);
                    if (st != PIME_OK) return st;
                } else {
                    item = Value::null();
                }
                arr.push(std::move(item));
                continue;
            }

            std::string key, value;
            bool has_value = false;
            if (split_key_value(rest, &key, &value, &has_value)) {
                Value item = Value::map_v();
                Value v;
                if (has_value) {
                    pime_status st = parse_inline(value, &v);
                    if (st != PIME_OK) return st;
                } else if (*index < lines.size() && lines[*index].indent > indent) {
                    /* Nested block belonging to the first key; consume it as
                     * the value at the following indentation. */
                    pime_status st = parse_block(index, lines[*index].indent, &v);
                    if (st != PIME_OK) return st;
                } else {
                    v = Value::null();
                }
                item.set(key, std::move(v));
                /* Remaining keys of the same item follow at a deeper indent. */
                if (*index < lines.size() && lines[*index].indent > indent &&
                    !is_seq_marker(lines[*index].text)) {
                    Value more;
                    pime_status st = parse_block(index, lines[*index].indent, &more);
                    if (st != PIME_OK) return st;
                    if (more.type == Type::Map) {
                        for (auto& kv : more.map) item.set(kv.first.text, std::move(kv.second));
                    } else {
                        return PIME_INVALID;
                    }
                }
                arr.push(std::move(item));
            } else {
                Value item;
                pime_status st = parse_inline(rest, &item);
                if (st != PIME_OK) return st;
                arr.push(std::move(item));
            }
        }
        *out = std::move(arr);
        return PIME_OK;
    }
};

void emit(const Value& v, int indent, std::string* out);

std::string quote_string(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string scalar_text(const Value& v) {
    switch (v.type) {
        case Type::Null: return "null";
        case Type::Bool: return v.boolean ? "true" : "false";
        case Type::UInt: return std::to_string(v.uint_value);
        case Type::Int: return std::to_string(v.int_value);
        case Type::Float: {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", v.float_value);
            return buf;
        }
        case Type::Text: return quote_string(v.text);
        case Type::Bytes: return quote_string(v.text);
        default: return std::string();
    }
}

void emit(const Value& v, int indent, std::string* out) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    if (v.type == Type::Map) {
        std::vector<const std::pair<Value, Value>*> items;
        for (const auto& kv : v.map) items.push_back(&kv);
        std::sort(items.begin(), items.end(), [](const auto* a, const auto* b) {
            return a->first.text < b->first.text;
        });
        for (const auto* kv : items) {
            out->append(pad);
            out->append(quote_string(kv->first.text));
            out->push_back(':');
            if (kv->second.type == Type::Map || kv->second.type == Type::Array) {
                out->push_back('\n');
                emit(kv->second, indent + 2, out);
            } else {
                out->push_back(' ');
                out->append(scalar_text(kv->second));
                out->push_back('\n');
            }
        }
    } else if (v.type == Type::Array) {
        for (const auto& item : v.array) {
            out->append(pad);
            out->push_back('-');
            if (item.type == Type::Map || item.type == Type::Array) {
                out->push_back('\n');
                emit(item, indent + 2, out);
            } else {
                out->push_back(' ');
                out->append(scalar_text(item));
                out->push_back('\n');
            }
        }
    }
}

}  // namespace

pime_status parse_yaml(std::string_view text, Value* out, std::string* error) {
    if (!out) return PIME_INVALID;
    std::string local_error;
    if (!error) error = &local_error;
    std::vector<Line> lines;
    pime_status st = preprocess(text, &lines, error);
    if (st != PIME_OK) return st;
    if (lines.empty()) {
        *error = "empty document";
        return PIME_INVALID;
    }
    Parser parser{std::move(lines), std::string()};
    size_t index = 0;
    st = parser.parse_block(&index, parser.lines[0].indent, out);
    if (st != PIME_OK) {
        *error = parser.error.empty() ? "invalid yaml" : parser.error;
        return st;
    }
    if (index != parser.lines.size()) {
        *error = "trailing content at line " + std::to_string(index + 1);
        return PIME_INVALID;
    }
    return PIME_OK;
}

std::string canonical_yaml(const Value& value, bool* ok) {
    if (ok) *ok = true;
    if (value.type != Type::Map) {
        if (ok) *ok = false;
        return {};
    }
    std::string out;
    emit(value, 0, &out);
    return out;
}

}  // namespace pime::codec
