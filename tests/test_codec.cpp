#include "test_framework.hpp"

#include "codec/cbor.hpp"
#include "codec/manifest.hpp"

using namespace pime;

TEST(cbor_roundtrip) {
    codec::Value map = codec::Value::map_v();
    map.set("text", codec::Value::text_v("hello"));
    map.set("bytes", codec::Value::bytes("\x00\x01\x02"));
    map.set("count", codec::Value::uint_v(42));
    map.set("negative", codec::Value::int_v(-7));
    map.set("flag", codec::Value::boolean_v(true));
    codec::Value list = codec::Value::array_v();
    list.push(codec::Value::uint_v(1));
    list.push(codec::Value::text_v("two"));
    map.set("list", std::move(list));

    std::vector<uint8_t> encoded;
    codec::encode(map, &encoded);
    codec::Value decoded;
    CHECK(codec::decode(encoded.data(), encoded.size(), &decoded) == PIME_OK);
    CHECK(decoded.equals(map));
}

TEST(cbor_rejects_indefinite) {
    const uint8_t data[] = {0x9f, 0x01, 0xff};
    codec::Value value;
    CHECK(codec::decode(data, sizeof(data), &value) != PIME_OK);
}

TEST(cbor_canonical_key_order) {
    codec::Value map = codec::Value::map_v();
    map.set("b", codec::Value::uint_v(2));
    map.set("a", codec::Value::uint_v(1));
    std::vector<uint8_t> encoded;
    codec::encode_canonical(map, &encoded);
    const std::vector<uint8_t> expected = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x02};
    CHECK(encoded == expected);
}

TEST(manifest_parse_and_validate) {
    const std::string yaml =
        "id: test.ime\n"
        "version: 1.0.0\n"
        "abi: { major: 1, minor: 0 }\n"
        "min_core: \"1.0.0\"\n"
        "state_schema: 1\n"
        "runtime: native\n"
        "trusted: true\n"
        "role: parse\n"
        "platforms: [windows-x64, linux-x64, macos-arm64]\n"
        "entry: pime_process\n"
        "capabilities: [input.read, output.emit]\n"
        "requires: []\n"
        "limits:\n"
        "  state_kb: 64\n"
        "  workspace_kb: 256\n"
        "  max_frame_bytes: 65536\n";
    codec::Manifest manifest = codec::parse_manifest(yaml);
    CHECK(manifest.valid);
    CHECK(manifest.id == "test.ime");
    CHECK(manifest.role == "parse");
    CHECK(manifest.runtime == "native");
    CHECK(manifest.platforms.size() == 3);
    CHECK(manifest.limits.state_kb == 64);
    SemVer core{1, 0, 0, "", "", true};
    CHECK(codec::validate_manifest(manifest, core) == PIME_OK);
}

TEST(manifest_rejects_unknown_capability) {
    const std::string yaml =
        "id: test.ime\n"
        "version: 1.0.0\n"
        "abi: { major: 1, minor: 0 }\n"
        "min_core: \"1.0.0\"\n"
        "runtime: wasm\n"
        "role: parse\n"
        "platforms: [windows-x64]\n"
        "entry: pime_process\n"
        "capabilities: [not.a.capability]\n";
    codec::Manifest manifest = codec::parse_manifest(yaml);
    SemVer core{1, 0, 0, "", "", true};
    CHECK(codec::validate_manifest(manifest, core) == PIME_INVALID);
}
