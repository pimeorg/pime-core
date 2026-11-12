#ifndef PIME_CODEC_YAML_HPP
#define PIME_CODEC_YAML_HPP

#include <pime/abi.h>

#include <string>
#include <string_view>

#include "cbor.hpp"

namespace pime::codec {

/*
 * Small YAML subset used by plugin manifests: block mappings and sequences,
 * flow maps/lists, quoted and plain scalars, comments. Not a general parser.
 */
pime_status parse_yaml(std::string_view text, Value* out, std::string* error);

/* Deterministic canonical form used for package signing. */
std::string canonical_yaml(const Value& value, bool* ok = nullptr);

}  // namespace pime::codec

#endif
