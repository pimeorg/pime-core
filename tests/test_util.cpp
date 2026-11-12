#include "test_framework.hpp"

#include <string>
#include <vector>

#include "util/base64.hpp"
#include "util/semver.hpp"
#include "util/sha256.hpp"
#include "util/utf8.hpp"

using namespace pime;

TEST(semver_parse_and_compare) {
    SemVer a = parse_semver("1.2.3");
    SemVer b = parse_semver("1.3.0");
    CHECK(a.valid);
    CHECK(b.valid);
    CHECK(compare_semver(a, b) < 0);
    CHECK(compare_semver(parse_semver("2.0.0-rc1"),
                         parse_semver("2.0.0")) < 0);
    CHECK(!parse_semver("1.2").valid);
}

TEST(semver_satisfies) {
    SemVer version = parse_semver("1.4.2");
    CHECK(version_satisfies(version, "^1.2.0"));
    CHECK(version_satisfies(version, "~1.4.0"));
    CHECK(version_satisfies(version, ">=1.0.0 <2.0.0"));
    CHECK(!version_satisfies(version, "^2.0.0"));
    CHECK(version_satisfies(version, "1.4.2"));
    CHECK(version_satisfies(version, "*"));
}

TEST(sha256_known_vector) {
    const auto digest = crypto_hash::sha256("abc");
    CHECK(crypto_hash::hex(digest) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(utf8_roundtrip) {
    const std::string text = "a\xE4\xB8\xAD\xF0\x9F\x98\x80";
    CHECK(utf8::valid(text));
    CHECK(utf8::count(text) == 3);
    CHECK(utf8::offset_of(text, 1) == 1);
    CHECK(utf8::offset_of(text, 2) == 4);
    const std::string built = utf8::from_cp(0x4E2D);
    CHECK(built == "\xE4\xB8\xAD");
}

TEST(base64_roundtrip) {
    const std::string data = "hello world";
    const std::string encoded =
        base64_encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    CHECK(encoded == "aGVsbG8gd29ybGQ=");
    std::vector<uint8_t> decoded;
    CHECK(base64_decode(encoded, &decoded));
    CHECK(std::string(decoded.begin(), decoded.end()) == data);
}
