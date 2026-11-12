# pime-core 进度

更新：2026-11-12

## 环境与需求

用户在 Windows 11 + MinGW-w64 (GCC 15.2.0) 环境下，要求按 `docs/core.md`、
`docs/plugin.md`、`docs/structure.md` 完成整个 core 的原生库实现：完整、无遗漏，
允许自主设计，但不过度设计。仓库内没有第三方运行时（OpenSSL / curl / lws /
Opus / WAMR / Lua），因此这些能力在构建时编译为显式 `PIME_UNSUPPORTED`，
不静默降级。

## 已完成

| 模块 | 内容 |
|---|---|
| 公共 ABI | 35 个导出符号；struct `size/version`；句柄 `(generation<<32)|index` |
| util | 状态名、SemVer 与约束、base64、SHA-256、UTF-8 标量、安全相对路径 |
| codec | CBOR 编解码与 canonical 编码、YAML 子集、清单校验、包哈希、签名文本 |
| phl | 缓冲池、上下文缓存、channel、mutex/condvar、shared_buffer、插件线程 |
| platform | 文件/目录、动态库、单调/墙钟时间、随机、路径 |
| capabilities | 14 个能力域；OpenSSL/curl/lws/Opus 存在时启用，否则 UNSUPPORTED |
| host | 包扫描/验签/安装；native 插件 vtable、host 回调（check/invoke/emit） |
| host/wasm | WAMR 适配：模块加载/实例化、scratch 与池化输出缓冲、全部 host import |
| host/lua | Lua 5.4 适配：受限分配器与指令预算、CBOR↔table、全部 `pime.*` 能力 |
| core | 会话、插件链、代、命令、异步事件、emit 队列、停止时清理 |
| hal | 全部 C 入口、结构校验、UTF-8/NUL 校验、sink 分发、异常拦截 |
| tools | pimepack（pack/sign/verify/info）、pimelint、pimeabi（基线检查）、pimebench |
| SDK / schema | native、WASM、Lua 头/桩、PimePlugin.cmake、JSON Schema、CDDL、ABI 基线 |
| examples | native / WASM / Lua 三个同行为参考插件 + 最小控制台宿主 |
| tests | 21 项契约测试全部通过（含 native 插件端到端与代切换） |

## 运行方式

```sh
cmake -S . -B build/check -G "MinGW Makefiles" -DPIME_BUILD_TESTS=ON
cmake --build build/check -j 8
PATH="/d/mingw64/bin:$PATH" ./build/check/tests/pime_tests.exe

cmake -S . -B build/tools -G "MinGW Makefiles" -DPIME_BUILD_TOOLS=ON
cmake --build build/tools -j 8
./build/tools/tools/pimeabi.exe check schema/abi.txt

# 示例（独立构建，链接已构建的 core）
cmake -S examples -B build/examples -G "MinGW Makefiles" \
      -DPIME_CORE_LIBRARY_DIR=build/check
cmake --build build/examples -j 8
cmake --build build/examples --target pime_uppercase_package
printf 'hello\n' | ./build/examples/hosts/minimal/pime_minimal_host.exe \
    build/examples/plugins/native/package/pime-plugin-pime-uppercase-1.0.0 \
    pime-uppercase build/examples/data
```

## 待完成

- WASM / Lua 适配层已完整实现，但本机没有 WAMR 与 Lua 5.4 源码或库：
  仅用与 `wasm_export.h` / Lua 5.4 API 一致的桩头做过编译检查，
  尚未在真实第三方库上链接与运行。构建选项 `PIME_BUILD_WASM` /
  `PIME_BUILD_LUA` 在缺少依赖时会明确报错，不会静默降级。
- 各平台宿主桥接（TSF、InputMethodKit、JNI、Node-API、IBus/Fcitx5）按
  `docs/structure.md` 属于宿主仓库或 `examples/hosts/`，当前只提供可移植的
  控制台最小宿主。

## 设计取舍

- 插件链用 `std::deque` 保存 `ChainItem`，保证 `Manifest*` 在代生命周期内稳定。
- 异步事件（timer/net/ml）无 session 字段，按插件 alias 记录最近会话进行路由。
- 上下文缓存按会话隔离，会话销毁即失效；句柄代际由 PHL 统一校验。
- 包格式为目录（非压缩包），与签名负载的 `path\0size\0sha256` 列表一致。
- `pimeabi` 以 `schema/abi.txt` 为基线，公共头或结构布局变化必须显式更新。
