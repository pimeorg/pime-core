# pime-core

Cross-platform input-method **core** as a native library (`dll` / `so` / `dylib` /
`framework`). Hosts — TSF, InputMethodKit, InputMethodService, IME Kit,
IBus/Fcitx5 — load the core and implement the HAL callbacks.

The core owns no input-method business logic. Tokenization, dictionaries,
language models, ranking, candidates, state machines, learning, punctuation,
key maps and schema semantics all live in plugins. The core provides:

- a stable C ABI (`include/pime/`) for hosts;
- a plugin host (native / WASM / Lua) with CBOR frames and capability APIs;
- generations: immutable plugin chains that are built, activated and retired;
- PHL: pooled buffers, context cache, channels, sync primitives, shared memory;
- capabilities: net, crypto, audio, ML, store, asset, config, workspace, log,
  metric, time, timer, caps, permission;
- packaging: manifests, signatures, hashes, install.

## Build

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

Options:

| option | default | meaning |
|---|---|---|
| `PIME_BUILD_SHARED` | ON | shared library instead of static |
| `PIME_BUILD_WASM` | OFF | WAMR plugin runtime (requires WAMR) |
| `PIME_BUILD_LUA` | OFF | Lua 5.4 plugin runtime (requires Lua) |
| `PIME_BUILD_TESTS` | OFF | contract tests |
| `PIME_BUILD_TOOLS` | OFF | pimepack / pimelint / pimeabi / pimebench |
| `PIME_SANITIZE_ADDRESS` / `PIME_SANITIZE_UNDEFINED` | OFF | sanitizers |

Optional third-party capabilities (`OpenSSL`, `curl`, `libwebsockets`, `Opus`)
compile to an explicit `PIME_UNSUPPORTED` when absent — never a silent fallback.

## Layout

```
include/pime/   host-side C ABI
src/hal/        C ABI entry points, validation, sink dispatch
src/core/       core, sessions, generations, pipeline, host bridge
src/host/       native / WASM / Lua plugin hosts, package manager
src/capabilities/  capability services
src/phl/        buffers, context cache, channel, sync, shared memory, threads
src/codec/      CBOR, YAML subset, manifest, package, signature
src/platform/   OS abstraction (windows / posix)
sdk/            plugin-author headers, Lua stubs, CMake helper
schema/         manifest JSON Schema, CBOR CDDL, ABI baseline
tools/          packaging / lint / ABI / benchmark CLIs
examples/       reference plugins (native / WASM / Lua) and a minimal host
tests/          contract tests
```

## Examples

`examples/` builds independently of the core tree. Build the core first, then:

```sh
cmake -S examples -B examples/build -DPIME_CORE_LIBRARY_DIR=<core-build-dir>
cmake --build examples/build -j 8
cmake --build examples/build --target pime_uppercase_package
printf 'hello\n' | examples/build/hosts/minimal/pime_minimal_host \
    examples/build/plugins/native/package/pime-plugin-pime-uppercase-1.0.0 \
    pime-uppercase
```

The native, WASM and Lua plugins implement the same uppercase behavior; the
minimal host shows the core lifecycle and sink callbacks. See
`examples/README.md` for packages, toolchains and packaging targets.

## Status

See `PROGRESS.md` and `TASK.md`. Design references: `docs/core.md`,
`docs/plugin.md`, `docs/structure.md`.

## License

Apache License 2.0, see `LICENSE`.

Third-party components keep their own licenses; the bundled WAMR sources used by
the WASM backend are Apache 2.0 as well.
