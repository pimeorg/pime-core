#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "codec/cbor.hpp"
#include "codec/manifest.hpp"
#include "util/sha256.hpp"

namespace {

using Clock = std::chrono::steady_clock;

pime::codec::Value sample_frame() {
    using pime::codec::Value;
    Value frame = Value::map_v();
    Value event = Value::map_v();
    event.set("type", Value::text_v("key"));
    event.set("keycode", Value::uint_v(0x61));
    event.set("action", Value::uint_v(0));
    event.set("modifiers", Value::uint_v(0));
    frame.set("event", event);
    frame.set("state", Value::bytes("\x01\x02\x03\x04"));
    Value workspace = Value::map_v();
    workspace.set("parse/syllables", Value::uint_v(3));
    workspace.set("parse/pending", Value::text_v("nihao"));
    frame.set("workspace", workspace);
    Value config = Value::map_v();
    config.set("page_size", Value::uint_v(9));
    frame.set("config", config);
    return frame;
}

constexpr const char* kManifest = R"(
id: pime-pinyin
version: 1.4.0
abi: { major: 1, minor: 0 }
min_core: "1.0.0"
state_schema: 3
store_schema: 2
runtime: wasm
role: parse
platforms: [windows-x64, macos-arm64]
entry: pime_process
capabilities: [input.read, output.emit, context.read]
limits: { memory_mb: 32, time_ms: 5 }
budget: { sync_us: 50, async_ms: 20 }
)";

void report(const char* name, size_t bytes, size_t iterations, Clock::duration elapsed) {
    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    const double per_op = seconds / static_cast<double>(iterations);
    const double throughput =
        seconds > 0 ? static_cast<double>(bytes) / seconds / 1e6 : 0;
    std::printf("%-24s %9zu ops  %9.2f ns/op  %8.1f MB/s\n", name, iterations,
                per_op * 1e9, throughput);
}

}  // namespace

int main(int argc, char** argv) {
    size_t iterations = 100000;
    if (argc > 1) iterations = static_cast<size_t>(std::stoull(argv[1]));
    if (iterations == 0) iterations = 1;

    const pime::codec::Value frame = sample_frame();
    std::vector<uint8_t> encoded;
    pime::codec::encode(frame, &encoded);
    std::printf("frame bytes: %zu\n", encoded.size());

    {
        size_t total = 0;
        const auto start = Clock::now();
        for (size_t i = 0; i < iterations; ++i) {
            encoded.clear();
            pime::codec::encode(frame, &encoded);
            total += encoded.size();
        }
        report("cbor encode", total, iterations, Clock::now() - start);
    }

    {
        size_t total = 0;
        const auto start = Clock::now();
        for (size_t i = 0; i < iterations; ++i) {
            pime::codec::Value decoded;
            pime::codec::decode(encoded.data(), encoded.size(), &decoded);
            total += decoded.map.size();
        }
        report("cbor decode", encoded.size() * iterations, iterations,
               Clock::now() - start);
        (void)total;
    }

    {
        const auto start = Clock::now();
        size_t parsed = 0;
        for (size_t i = 0; i < iterations; ++i) {
            pime::codec::Manifest manifest = pime::codec::parse_manifest(kManifest);
            if (manifest.valid) ++parsed;
        }
        report("manifest parse", std::strlen(kManifest) * iterations, iterations,
               Clock::now() - start);
        if (parsed != iterations) std::fputs("manifest parse failed\n", stderr);
    }

    {
        const auto start = Clock::now();
        for (size_t i = 0; i < iterations; ++i) {
            auto digest = pime::crypto_hash::sha256(encoded.data(), encoded.size());
            (void)digest;
        }
        report("sha256", encoded.size() * iterations, iterations,
               Clock::now() - start);
    }

    return 0;
}
