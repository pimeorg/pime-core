#include "test_framework.hpp"

#include <pime/core.h>
#include <pime/plugin.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "codec/cbor.hpp"

using namespace pime;

namespace {

struct SinkState {
    int commits = 0;
    int compositions = 0;
    int hides = 0;
    int notifies = 0;
    std::string last_commit;
    std::string last_composition;
};

pime_status sink_commit(void* ud, const pime_text* text) {
    auto* state = static_cast<SinkState*>(ud);
    if (!state || !text) return PIME_INVALID;
    state->commits++;
    state->last_commit.assign(text->text ? text->text : "", text->text_len);
    return PIME_OK;
}

pime_status sink_composition(void* ud, const pime_composition* composition) {
    auto* state = static_cast<SinkState*>(ud);
    if (!state || !composition) return PIME_INVALID;
    state->compositions++;
    state->last_composition.assign(
        composition->text ? composition->text : "", composition->text_len);
    return PIME_OK;
}

pime_status sink_hide(void* ud) {
    if (!ud) return PIME_INVALID;
    static_cast<SinkState*>(ud)->hides++;
    return PIME_OK;
}

pime_status sink_notify(void* ud, pime_status) {
    if (!ud) return PIME_INVALID;
    static_cast<SinkState*>(ud)->notifies++;
    return PIME_OK;
}

pime_status sink_show(void* ud, const pime_candidate_list*) {
    (void)ud;
    return PIME_OK;
}

std::filesystem::path make_temp_dir() {
    std::random_device device;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("pime-core-test-" + std::to_string(stamp) + "-" +
         std::to_string(device()));
    std::filesystem::create_directories(dir);
    return dir;
}

#if defined(_WIN32)
constexpr const char* kPluginName = "plugin.dll";
#elif defined(__APPLE__)
constexpr const char* kPluginName = "plugin.dylib";
#else
constexpr const char* kPluginName = "plugin.so";
#endif

std::string manifest_text() {
    return
        "id: test.ime\n"
        "version: 1.0.0\n"
        "abi: { major: 1, minor: 0 }\n"
        "min_core: \"1.0.0\"\n"
        "state_schema: 1\n"
        "store_schema: 0\n"
        "runtime: native\n"
        "trusted: true\n"
        "role: parse\n"
        "platforms: [windows-x64, linux-x64, macos-arm64, macos-x64]\n"
        "entry: pime_process\n"
        "capabilities: [input.read, output.emit, log]\n"
        "requires: []\n"
        "limits:\n"
        "  state_kb: 64\n"
        "  workspace_kb: 256\n"
        "  max_frame_bytes: 65536\n";
}

std::vector<uint8_t> generation_config() {
    codec::Value plugin = codec::Value::map_v();
    plugin.set("id", codec::Value::text_v("test.ime"));
    plugin.set("role", codec::Value::text_v("parse"));
    plugin.set("settings", codec::Value::map_v());
    codec::Value list = codec::Value::array_v();
    list.push(std::move(plugin));
    codec::Value root = codec::Value::map_v();
    root.set("plugins", std::move(list));
    std::vector<uint8_t> encoded;
    codec::encode(root, &encoded);
    return encoded;
}

pime_text text_of(std::string& storage) {
    pime_text text{};
    text.size = sizeof(text);
    text.version = PIME_ABI_MAJOR;
    text.text = storage.data();
    text.text_len = static_cast<uint32_t>(storage.size());
    return text;
}

pime_key_event key_of(std::string& storage, uint32_t keycode) {
    pime_key_event key{};
    key.size = sizeof(key);
    key.version = PIME_ABI_MAJOR;
    key.keycode = keycode;
    key.action = PIME_KEY_DOWN;
    key.text = storage.data();
    key.text_len = static_cast<uint32_t>(storage.size());
    key.time_ms = 1;
    return key;
}

struct Fixture {
    std::filesystem::path base;
    std::filesystem::path package;
    std::filesystem::path plugins;
    std::filesystem::path data;
    SinkState sink;
    pime_core* core = nullptr;
    pime_session* session = nullptr;
    uint64_t generation = 0;

    bool setup() {
        base = make_temp_dir();
        package = base / "package";
        plugins = base / "plugins";
        data = base / "data";
        std::filesystem::create_directories(package);
        std::filesystem::copy_file(TEST_PLUGIN_PATH, package / kPluginName);
        {
            std::ofstream manifest(package / "manifest.yaml", std::ios::binary);
            manifest << manifest_text();
        }

        pime_core_config config{};
        config.size = sizeof(config);
        config.version = PIME_ABI_MAJOR;
        const std::string plugin_dir = plugins.string();
        const std::string data_dir = data.string();
        config.plugin_dir = plugin_dir.c_str();
        config.plugin_dir_len = static_cast<uint32_t>(plugin_dir.size());
        config.data_dir = data_dir.c_str();
        config.data_dir_len = static_cast<uint32_t>(data_dir.size());
        config.flags = PIME_FLAG_ALLOW_INSECURE;
        config.log_level = PIME_LOG_ERROR;
        config.memory_budget_mb = 64;

        pime_host_sink host_sink{};
        host_sink.size = sizeof(host_sink);
        host_sink.version = PIME_ABI_MAJOR;
        host_sink.set_composition = &sink_composition;
        host_sink.commit = &sink_commit;
        host_sink.show_candidates = &sink_show;
        host_sink.hide_candidates = &sink_hide;
        host_sink.notify = &sink_notify;

        core = pime_core_create(&config, &host_sink, &sink);
        if (!core) return false;
        const std::string package_dir = package.string();
        if (pime_plugin_load(core, package_dir.c_str(),
                             static_cast<uint32_t>(package_dir.size())) !=
            PIME_OK)
            return false;
        const std::vector<uint8_t> chain = generation_config();
        if (pime_generation_build(core, chain.data(),
                                  static_cast<uint32_t>(chain.size()),
                                  &generation) != PIME_OK)
            return false;
        if (pime_generation_activate(core, generation) != PIME_OK) return false;
        session = pime_session_create(core, nullptr);
        return session != nullptr;
    }

    ~Fixture() {
        if (session) pime_session_destroy(session);
        if (core) pime_core_destroy(core);
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

}  // namespace

TEST(core_native_plugin_end_to_end) {
    Fixture fixture;
    CHECK(fixture.setup());
    if (!fixture.session) return;

    pime_context context{};
    context.size = sizeof(context);
    context.version = PIME_ABI_MAJOR;
    context.field_type = PIME_FIELD_TEXT;
    context.input_type = PIME_INPUT_KEYBOARD;
    context.page_size = 5;
    context.host_caps = PIME_HOST_SURROUNDING | PIME_HOST_SECURE |
                        PIME_HOST_CANDIDATES;
    std::string locale = "zh-CN";
    context.locale = locale.data();
    context.locale_len = static_cast<uint32_t>(locale.size());
    CHECK(pime_core_context(fixture.session, &context) == PIME_OK);

    std::string a = "a";
    pime_key_event key = key_of(a, 'a');
    int handled = 0;
    CHECK(pime_core_key(fixture.session, &key, &handled) == PIME_OK);
    CHECK(handled == 1);
    CHECK_EQ(fixture.sink.commits, 1);
    CHECK_EQ(fixture.sink.last_commit, std::string("A"));
    CHECK_EQ(fixture.sink.compositions, 1);
    CHECK_EQ(fixture.sink.last_composition, std::string("A"));

    std::string b = "b";
    key = key_of(b, 'b');
    CHECK(pime_core_key(fixture.session, &key, &handled) == PIME_OK);
    CHECK(handled == 1);
    CHECK_EQ(fixture.sink.commits, 2);
    CHECK_EQ(fixture.sink.last_commit, std::string("B"));

    std::string hi = "hi";
    pime_text text = text_of(hi);
    CHECK(pime_core_text(fixture.session, &text) == PIME_OK);
    CHECK_EQ(fixture.sink.commits, 3);
    CHECK_EQ(fixture.sink.last_commit, std::string("HI"));

    CHECK(pime_core_secure(fixture.session, 1) == PIME_OK);
    std::string x = "x";
    key = key_of(x, 'x');
    handled = 1;
    CHECK(pime_core_key(fixture.session, &key, &handled) == PIME_OK);
    CHECK(handled == 0);
    CHECK_EQ(fixture.sink.commits, 3);

    CHECK(pime_core_secure(fixture.session, 0) == PIME_OK);
    CHECK(pime_core_reset(fixture.session) == PIME_OK);
    CHECK_EQ(fixture.sink.hides, 1);

    CHECK(pime_core_network_changed(fixture.core, PIME_NET_ONLINE) == PIME_OK);
    pime_core_pump(fixture.core);
    CHECK(pime_core_audio_focus(fixture.core, PIME_FOCUS_GAINED) == PIME_OK);
    CHECK(pime_core_memory_warning(fixture.core, PIME_MEMORY_WARNING) == PIME_OK);
    CHECK(pime_core_pause(fixture.core) == PIME_OK);
    CHECK(pime_core_resume(fixture.core) == PIME_OK);

    uint32_t required = 0;
    CHECK(pime_plugin_list(fixture.core, nullptr, 0, &required) ==
          PIME_BUFFER_TOO_SMALL);
    std::vector<uint8_t> list(required);
    CHECK(pime_plugin_list(fixture.core, list.data(), required, &required) ==
          PIME_OK);
    codec::Value decoded;
    CHECK(codec::decode(list.data(), list.size(), &decoded) == PIME_OK);
    CHECK(decoded.is_array());
    CHECK(decoded.array.size() == 1);
}

TEST(core_generation_swap) {
    Fixture fixture;
    CHECK(fixture.setup());
    if (!fixture.core) return;

    const std::vector<uint8_t> chain = generation_config();
    uint64_t second = 0;
    CHECK(pime_generation_build(fixture.core, chain.data(),
                                static_cast<uint32_t>(chain.size()),
                                &second) == PIME_OK);
    CHECK(second != fixture.generation);
    CHECK(pime_generation_activate(fixture.core, second) == PIME_OK);

    if (fixture.session) {
        std::string c = "c";
        pime_key_event key = key_of(c, 'c');
        int handled = 0;
        CHECK(pime_core_key(fixture.session, &key, &handled) == PIME_OK);
        CHECK(handled == 1);
        CHECK_EQ(fixture.sink.last_commit, std::string("C"));
    }

    CHECK(pime_generation_retire(fixture.core, fixture.generation) == PIME_OK);
    CHECK(pime_generation_retire(fixture.core, second) == PIME_BUSY);
    CHECK(pime_generation_activate(fixture.core, second) == PIME_OK);
}

TEST(core_plugin_unload_busy) {
    Fixture fixture;
    CHECK(fixture.setup());
    if (!fixture.core) return;
    pime_plugin_ref ref{};
    ref.size = sizeof(ref);
    ref.version = PIME_ABI_MAJOR;
    const std::string id = "test.ime";
    ref.id = id.data();
    ref.id_len = static_cast<uint32_t>(id.size());
    CHECK(pime_plugin_unload(fixture.core, &ref) == PIME_BUSY);
    CHECK(pime_plugin_enable(fixture.core, &ref, 0) == PIME_OK);
    CHECK(pime_plugin_enable(fixture.core, &ref, 1) == PIME_OK);
}

TEST(core_permission_and_validation) {
    Fixture fixture;
    CHECK(fixture.setup());
    if (!fixture.core) return;
    CHECK(pime_core_permission(fixture.core, "log", 3, 1) == PIME_OK);
    CHECK(pime_core_permission(fixture.core, "unknown.cap", 11, 1) ==
          PIME_INVALID);
    CHECK(pime_core_network_changed(fixture.core, 99) == PIME_INVALID);
    CHECK(pime_generation_activate(fixture.core, 12345) == PIME_NOT_FOUND);
    CHECK(pime_generation_retire(fixture.core, 12345) == PIME_NOT_FOUND);
}
