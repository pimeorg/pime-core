# pime-core examples

Reference plugins and a console host. Examples build independently of the core
tree and only use `include/pime/`, `sdk/` and the built core library.

```
plugins/common/   minimal CBOR helper used by the native and WASM examples
plugins/native/   trusted native plugin (dll / so / dylib)
plugins/lua/      Lua 5.4 plugin package
plugins/wasm/     WASM plugin (requires a wasm32/WASI toolchain)
hosts/minimal/    console host: load package, build generation, feed keys
```

## Build

```sh
# 1. build the core (from the repository root)
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j 8

# 2. build examples against it
cmake -S examples -B examples/build -DPIME_CORE_LIBRARY_DIR=build
cmake --build examples/build -j 8
```

Each plugin has a `<target>_package` target that assembles the directory layout
the core expects (`pime-plugin-<id>-<version>/manifest.yaml`, `plugin.dll|so|
dylib`, `plugin.wasm` or `plugin.lua`).

```sh
cmake --build examples/build --target pime_uppercase_package
cmake --build examples/build --target pime_uppercase_lua_package
```

The WASM example is skipped unless the CMake toolchain targets `wasm32`; see
the comment at the top of `plugins/wasm/plugin.cpp` for the equivalent clang
invocation.

## Run

```sh
printf 'hello\nWorld\n' |
  examples/build/hosts/minimal/pime_minimal_host \
    examples/build/plugins/native/package/pime-plugin-pime-uppercase-1.0.0 \
    pime-uppercase
```

The plugin commits the uppercase form of each line; the host prints it through
the `commit` sink callback.

## Packages

The example packages are unsigned, so load them with
`PIME_FLAG_ALLOW_INSECURE`. Sign production packages with `pimepack sign` and
verify them with `pimelint` before installing.
