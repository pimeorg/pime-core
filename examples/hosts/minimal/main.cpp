/*
 * Minimal pime-core host.
 *
 * Demonstrates the native library lifecycle: create core -> load package ->
 * build and activate a generation -> open a session -> feed keys -> destroy.
 * It is a console harness, not a platform IME integration.
 *
 *   pime_minimal_host <package-dir> <plugin-id>
 *
 * Lines read from stdin are sent as key events with text; committed text is
 * printed to stdout by the sink.
 */
#include <pime/pime.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../../plugins/common/mini_cbor.hpp"

namespace {

struct HostState {
    std::string committed;
};

pime_status on_commit(void* ud, const pime_text* text) {
    auto* state = static_cast<HostState*>(ud);
    state->committed.assign(text->text, text->text_len);
    std::cout << state->committed << std::endl;
    return PIME_OK;
}

pime_status on_composition(void* ud, const pime_composition* composition) {
    (void)ud;
    std::cout << "\r[" << std::string(composition->text, composition->text_len)
              << "]" << std::flush;
    return PIME_OK;
}

pime_status on_show(void*, const pime_candidate_list*) { return PIME_OK; }
pime_status on_hide(void*) { return PIME_OK; }
pime_status on_notify(void*, pime_status) { return PIME_OK; }

void on_log(void*, const pime_log_record* record) {
    if (record && record->msg)
        std::fprintf(stderr, "[plugin] %.*s\n",
                     static_cast<int>(record->msg_len), record->msg);
}

std::vector<uint8_t> chain_config(const std::string& plugin_id) {
    using example::Node;
    Node plugin = Node::map_v()
                      .set("id", Node::text_v(plugin_id))
                      .set("role", Node::text_v("parse"))
                      .set("settings", Node::map_v());
    Node plugins = Node::array_v();
    plugins.push(std::move(plugin));
    Node root = Node::map_v();
    root.set("plugins", std::move(plugins));
    std::vector<uint8_t> encoded;
    example::encode(root, &encoded);
    return encoded;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <package-dir> <plugin-id> [data-dir]\n",
                     argv[0]);
        return 2;
    }
    const std::string package_dir = argv[1];
    const std::string plugin_id = argv[2];
    const std::string data_dir = argc > 3 ? argv[3] : "pime-data";
    const std::string plugin_store = data_dir + "/plugins";

    HostState state;

    pime_core_config config{};
    config.size = sizeof(config);
    config.version = PIME_ABI_MAJOR;
    config.plugin_dir = plugin_store.c_str();
    config.plugin_dir_len = static_cast<uint32_t>(plugin_store.size());
    config.data_dir = data_dir.c_str();
    config.data_dir_len = static_cast<uint32_t>(data_dir.size());
    config.flags = PIME_FLAG_ALLOW_INSECURE;
    config.log_level = PIME_LOG_INFO;
    config.memory_budget_mb = 64;

    pime_host_sink sink{};
    sink.size = sizeof(sink);
    sink.version = PIME_ABI_MAJOR;
    sink.set_composition = &on_composition;
    sink.commit = &on_commit;
    sink.show_candidates = &on_show;
    sink.hide_candidates = &on_hide;
    sink.notify = &on_notify;
    sink.on_log = &on_log;

    pime_core* core = pime_core_create(&config, &sink, &state);
    if (!core) {
        std::fprintf(stderr, "pime_core_create failed\n");
        return 1;
    }

    pime_status status =
        pime_plugin_load(core, package_dir.c_str(),
                         static_cast<uint32_t>(package_dir.size()));
    if (status != PIME_OK) {
        std::fprintf(stderr, "pime_plugin_load: %s\n",
                     pime_status_string(status));
        pime_core_destroy(core);
        return 1;
    }

    const std::vector<uint8_t> chain = chain_config(plugin_id);
    uint64_t generation = 0;
    status = pime_generation_build(core, chain.data(),
                                   static_cast<uint32_t>(chain.size()),
                                   &generation);
    if (status == PIME_OK) status = pime_generation_activate(core, generation);
    if (status != PIME_OK) {
        std::fprintf(stderr, "generation: %s\n", pime_status_string(status));
        pime_core_destroy(core);
        return 1;
    }

    pime_session_config session_config{};
    session_config.size = sizeof(session_config);
    session_config.version = PIME_ABI_MAJOR;
    pime_session* session = pime_session_create(core, &session_config);
    if (!session) {
        std::fprintf(stderr, "pime_session_create failed\n");
        pime_core_destroy(core);
        return 1;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        pime_key_event key{};
        key.size = sizeof(key);
        key.version = PIME_ABI_MAJOR;
        key.action = PIME_KEY_DOWN;
        key.text = line.data();
        key.text_len = static_cast<uint32_t>(line.size());
        int handled = 0;
        status = pime_core_key(session, &key, &handled);
        if (status != PIME_OK)
            std::fprintf(stderr, "pime_core_key: %s\n",
                         pime_status_string(status));
        pime_core_pump(core);
    }

    pime_session_destroy(session);
    pime_generation_retire(core, generation);
    pime_core_destroy(core);
    return 0;
}
