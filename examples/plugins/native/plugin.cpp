/*
 * Reference native plugin: uppercases key/text input and commits it.
 *
 * Native plugins are a trusted escape hatch; the same CBOR frames and host
 * API apply as for WASM/Lua plugins. Build with PimePlugin.cmake.
 */
#include <pime/native_plugin.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mini_cbor.hpp"

namespace {

using example::Node;

struct State {
    uint64_t commits = 0;
    const pime_native_host* host = nullptr;
};

std::string upper(const std::string& text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

pime_status write_frame(const Node& frame, uint8_t* out, uint32_t cap,
                        uint32_t* out_len) {
    if (!out_len) return PIME_INVALID;
    std::vector<uint8_t> encoded;
    example::encode(frame, &encoded);
    *out_len = static_cast<uint32_t>(encoded.size());
    if (encoded.size() > cap) return PIME_BUFFER_TOO_SMALL;
    if (!encoded.empty()) {
        if (!out) return PIME_INVALID;
        for (size_t i = 0; i < encoded.size(); ++i) out[i] = encoded[i];
    }
    return PIME_OK;
}

pime_status init(const uint8_t*, uint32_t, void** state) {
    if (!state) return PIME_INVALID;
    *state = new State();
    return PIME_OK;
}

pime_status process(void* raw, const uint8_t* in, uint32_t in_len,
                    uint8_t* out, uint32_t cap, uint32_t* out_len) {
    auto* state = static_cast<State*>(raw);
    if (!state) return PIME_INVALID;

    Node frame;
    if (!example::decode(in, in_len, &frame) || !frame.is_map())
        return PIME_CORRUPT;

    Node result = Node::map_v();
    bool handled = false;
    if (const Node* event = frame.find("event");
        event && event->is_map()) {
        const Node* type = event->find("type");
        if (type && type->is_text() &&
            (type->text == "key" || type->text == "text")) {
            if (const Node* text = event->find("text"); text && text->is_text()) {
                const std::string value = upper(text->text);
                Node commands = Node::array_v();
                commands.push(Node::map_v()
                                  .set("type", Node::text_v("commit"))
                                  .set("text", Node::text_v(value)));
                result.set("commands", std::move(commands));
                handled = true;
                ++state->commits;
            }
        }
    }

    result.set("handled", Node::boolean_v(handled));
    result.set("state",
               Node::map_v().set("commits", Node::uint_v(state->commits)));

    if (state->host && state->host->invoke) {
        Node request = Node::map_v()
                           .set("level", Node::uint_v(2 /* info */))
                           .set("tag", Node::text_v("uppercase"))
                           .set("message", Node::text_v("frame processed"));
        std::vector<uint8_t> encoded;
        example::encode(request, &encoded);
        uint8_t reply[256];
        uint32_t reply_len = 0;
        state->host->invoke(state->host->ud, 16 /* Log */, 0, encoded.data(),
                            static_cast<uint32_t>(encoded.size()), reply,
                            sizeof(reply), &reply_len);
    }
    return write_frame(result, out, cap, out_len);
}

pime_status on_event(void* raw, const uint8_t*, uint32_t, uint8_t* out,
                     uint32_t cap, uint32_t* out_len) {
    auto* state = static_cast<State*>(raw);
    if (!state) return PIME_INVALID;
    Node result = Node::map_v();
    result.set("handled", Node::boolean_v(false));
    result.set("state",
               Node::map_v().set("commits", Node::uint_v(state->commits)));
    return write_frame(result, out, cap, out_len);
}

void shutdown(void* raw) { delete static_cast<State*>(raw); }

pime_status set_host(void* raw, const pime_native_host* host) {
    if (!raw || !host) return PIME_INVALID;
    static_cast<State*>(raw)->host = host;
    return PIME_OK;
}

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
    out->set_host = &set_host;
    return PIME_OK;
}
