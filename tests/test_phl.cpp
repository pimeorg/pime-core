#include "test_framework.hpp"

#include <string>

#include "capabilities/workspace/workspace.hpp"
#include "phl/buffer_pool.hpp"
#include "phl/channel.hpp"
#include "phl/context_cache.hpp"
#include "phl/handle.hpp"
#include "phl/shared_buffer.hpp"

using namespace pime;

TEST(buffer_pool_acquire_write_read) {
    phl::BufferPool pool;
    phl::BufferView view;
    CHECK(pool.acquire(phl::BufferKind::Text, 7, 16, false, &view) == PIME_OK);
    CHECK(view.handle != 0);
    CHECK(pool.write(view.handle, 0, "abc", 3) == PIME_OK);
    char buffer[8] = {};
    uint32_t length = 0;
    CHECK(pool.read(view.handle, 0, buffer, sizeof(buffer), &length) == PIME_OK);
    CHECK(length == 3);
    CHECK(std::string(buffer, length) == "abc");
    CHECK(pool.release(view.handle) == PIME_OK);
    CHECK(pool.release(view.handle) == PIME_STALE);
}

TEST(buffer_pool_invalidate_invalidates_handles) {
    phl::BufferPool pool;
    phl::BufferView view;
    CHECK(pool.acquire(phl::BufferKind::Log, 1, 8, false, &view) == PIME_OK);
    pool.invalidate_all();
    uint32_t length = 0;
    CHECK(pool.read(view.handle, 0, nullptr, 0, &length) == PIME_STALE);
}

TEST(handle_registry_generation) {
    phl::HandleRegistry<int> registry;
    auto object = std::make_shared<int>(5);
    const uint64_t handle = registry.insert(object);
    CHECK(handle != 0);
    pime_status status = PIME_OK;
    CHECK(registry.get(handle, &status) != nullptr);
    CHECK(status == PIME_OK);
    CHECK(registry.erase(handle) == PIME_OK);
    CHECK(registry.get(handle, &status) == nullptr);
    CHECK(status == PIME_STALE);
}

TEST(channel_inline_roundtrip) {
    phl::Channel channel(2);
    phl::Message message;
    message.data = {1, 2, 3};
    CHECK(channel.send(message, 0) == PIME_OK);
    CHECK(channel.depth() == 1);
    phl::Message received;
    CHECK(channel.recv(&received, 0) == PIME_OK);
    CHECK(received.data == message.data);
    CHECK(channel.close(false) == PIME_OK);
    CHECK(channel.recv(&received, 0) == PIME_CLOSED);
}

TEST(workspace_scoping) {
    cap::Workspace workspace;
    workspace.set("parse", "syllables", codec::Value::int_v(3));
    CHECK(workspace.commit() == PIME_OK);
    codec::Value value;
    CHECK(workspace.get_self("parse", "syllables", &value));
    CHECK(value.as_int() == 3);
    CHECK(!workspace.get_self("rank", "syllables", &value));
    CHECK(workspace.get("plugin/parse/syllables", &value));
    CHECK(!workspace.get("plugin/rank/syllables", &value));
}

TEST(context_cache_data) {
    phl::ContextCache cache;
    uint64_t handle = 0;
    CHECK(cache.acquire("parse", "table", phl::ContextKind::Data, 2, "hash",
                        PIME_CTX_CREATE, &handle) == PIME_OK);
    CHECK(cache.set_data(handle, "abc", 3) == PIME_OK);
    std::vector<uint8_t> data;
    CHECK(cache.get_data(handle, &data) == PIME_OK);
    CHECK(std::string(data.begin(), data.end()) == "abc");
    CHECK(cache.release(handle) == PIME_OK);
    CHECK(cache.set_data(handle, "x", 1) == PIME_OK);
    CHECK(cache.drop("parse", "table", phl::ContextKind::Data) == PIME_OK);
    CHECK(cache.set_data(handle, "y", 1) == PIME_STALE);
}

TEST(shared_buffer_roundtrip) {
    phl::SharedBuffer buffer(8, false);
    CHECK(buffer.write(0, "abcd", 4) == PIME_OK);
    char out[8] = {};
    uint32_t length = 0;
    CHECK(buffer.read(0, out, sizeof(out), &length) == PIME_OK);
    CHECK(length == 8);
    CHECK(std::string(out, 4) == "abcd");
    void* pointer = nullptr;
    uint32_t size = 0;
    CHECK(buffer.map(&pointer, &size) == PIME_OK);
    CHECK(size == 8);
    CHECK(buffer.resize(16) == PIME_BUSY);
    CHECK(buffer.unmap() == PIME_OK);
    CHECK(buffer.resize(16) == PIME_OK);
}
