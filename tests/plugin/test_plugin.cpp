#include <pime/native_plugin.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "codec/cbor.hpp"

namespace {

using pime::codec::Value;

struct PluginState {
    uint64_t count = 0;
    bool secure = false;
};

std::string uppercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

pime_status write_output(const Value& out, uint8_t* buffer, uint32_t capacity,
                         uint32_t* out_len) {
    if (!out_len) return PIME_INVALID;
    std::vector<uint8_t> encoded;
    pime::codec::encode(out, &encoded);
    *out_len = static_cast<uint32_t>(encoded.size());
    if (encoded.size() > capacity) return PIME_BUFFER_TOO_SMALL;
    if (!encoded.empty()) {
        if (!buffer) return PIME_INVALID;
        std::copy(encoded.begin(), encoded.end(), buffer);
    }
    return PIME_OK;
}

pime_status init(const uint8_t*, uint32_t, void** state) {
    if (!state) return PIME_INVALID;
    *state = new PluginState();
    return PIME_OK;
}

pime_status process(void* raw, const uint8_t* in, uint32_t in_len,
                    uint8_t* out_buffer, uint32_t capacity, uint32_t* out_len) {
    auto* state = static_cast<PluginState*>(raw);
    if (!state) return PIME_INVALID;
    Value frame;
    pime_status status = pime::codec::decode(in, in_len, &frame);
    if (status != PIME_OK || !frame.is_map()) return PIME_CORRUPT;

    const Value* event = frame.find("event");
    const std::string type =
        event && event->is_map() && event->find("type")
            ? std::string(event->find("type")->as_text())
            : std::string();

    if (const Value* secure = frame.find("secure"))
        state->secure = secure->as_bool();

    Value out = Value::map_v();
    Value commands = Value::array_v();
    bool handled = false;

    if (type == "key") {
        const Value* text = event->find("text");
        if (text && text->is_text()) {
            const std::string value = uppercase(text->as_text());
            Value composition = Value::map_v();
            composition.set("type", Value::text_v("set_composition"));
            composition.set("text", Value::text_v(value));
            composition.set("cursor", Value::uint_v(value.size()));
            commands.push(std::move(composition));

            Value commit = Value::map_v();
            commit.set("type", Value::text_v("commit"));
            commit.set("text", Value::text_v(value));
            commands.push(std::move(commit));
            ++state->count;
            handled = true;
        }
    } else if (type == "text") {
        const Value* text = event->find("text");
        if (text && text->is_text()) {
            Value commit = Value::map_v();
            commit.set("type", Value::text_v("commit"));
            commit.set("text", Value::text_v(uppercase(text->as_text())));
            commands.push(std::move(commit));
            ++state->count;
            handled = true;
        }
    } else if (type == "reset") {
        Value hide = Value::map_v();
        hide.set("type", Value::text_v("hide_candidates"));
        commands.push(std::move(hide));
        state->count = 0;
    } else if (type == "candidate") {
        handled = true;
        ++state->count;
    }

    Value state_value = Value::map_v();
    state_value.set("count", Value::uint_v(state->count));
    state_value.set("secure", Value::boolean_v(state->secure));
    out.set("state", std::move(state_value));
    out.set("handled", Value::boolean_v(handled));
    if (!commands.array.empty()) out.set("commands", std::move(commands));
    return write_output(out, out_buffer, capacity, out_len);
}

pime_status on_event(void* raw, const uint8_t* event_data, uint32_t event_len,
                     uint8_t* out_buffer, uint32_t capacity,
                     uint32_t* out_len) {
    auto* state = static_cast<PluginState*>(raw);
    if (!state) return PIME_INVALID;
    Value frame;
    pime_status status = pime::codec::decode(event_data, event_len, &frame);
    if (status != PIME_OK || !frame.is_map()) return PIME_CORRUPT;

    Value out = Value::map_v();
    const Value* event = frame.find("event");
    const std::string type =
        event && event->is_map() && event->find("type")
            ? std::string(event->find("type")->as_text())
            : std::string();
    if (type == "timer") {
        Value notify = Value::map_v();
        notify.set("type", Value::text_v("notify"));
        notify.set("code", Value::uint_v(PIME_OK));
        Value commands = Value::array_v();
        commands.push(std::move(notify));
        out.set("commands", std::move(commands));
    }
    out.set("handled", Value::boolean_v(false));
    Value state_value = Value::map_v();
    state_value.set("count", Value::uint_v(state->count));
    state_value.set("secure", Value::boolean_v(state->secure));
    out.set("state", std::move(state_value));
    return write_output(out, out_buffer, capacity, out_len);
}

void shutdown(void* state) { delete static_cast<PluginState*>(state); }

}  // namespace

extern "C" PIME_PLUGIN_EXPORT pime_status
pime_native_plugin_query(uint32_t abi_major, pime_native_plugin* out) {
    if (!out) return PIME_INVALID;
    if (abi_major != PIME_ABI_MAJOR) return PIME_UNSUPPORTED;
    out->size = sizeof(pime_native_plugin);
    out->version = PIME_ABI_MAJOR;
    out->init = &init;
    out->process = &process;
    out->on_event = &on_event;
    out->shutdown = &shutdown;
    return PIME_OK;
}
