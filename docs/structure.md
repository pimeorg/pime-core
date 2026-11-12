# pime-core 工程结构

> 本文只描述工程结构、构建与依赖；行为规格以 `core.md` 与 `plugin.md` 为准。

## 1 原则

- **公共 ABI 与实现分离**：只有 `include/pime/` 是 host 侧稳定 C ABI，插件侧 ABI 在 `sdk/`；其余头文件是内部 C++。
- **平台隔离**：平台头文件与系统调用只出现在 `src/platform/<os>/`，其他目录不得出现 `#ifdef _WIN32` 一类分支；`backends/` 的平台构建适配除外。
- **核心不含业务**：分词、词典、排序、候选、状态机只存在于插件；core 目录不得出现输入法业务代码。
- **依赖单向**：`hal → core → host/capabilities/phl/codec`；`host/capabilities/phl → platform`；反向依赖禁止。
- **第三方集中**：第三方源码只放在 `third_party/`；只有 `src/host/{wasm,lua}`、`src/capabilities/{net,crypto,audio}` 的直接适配层与 `backends/` 可以 include 第三方头，其他目录通过 wrapper 使用。
- **桥接层归宿主**：JNI、Node-API、TSF/InputMethodKit 等桥接属于平台宿主，不进入 core 产物；core 只导出 C ABI。
- **可测试**：公共 C ABI 可被 host smoke test 驱动；内部模块有 unit/fuzz/conformance。
- **可复现**：第三方版本、编译选项、生成头全部由 CMake 固定；不在仓库提交生成物。

## 2 顶层结构

```text
pime-core/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── LICENSE
├── .clang-format
├── .clang-tidy
├── .editorconfig
├── .gitignore
├── .gitattributes
├── cmake/
│   ├── PimeVersion.cmake          # core SemVer、ABI major/minor
│   ├── PimeOptions.cmake          # 功能开关
│   ├── PimeDependencies.cmake     # 第三方依赖与补丁
│   ├── PimePlatform.cmake         # 平台/架构/工具链选择
│   ├── PimeWarnings.cmake         # 编译告警与 -Werror
│   ├── PimeSanitizers.cmake       # ASan/UBSan/MSan
│   ├── PimeInstall.cmake          # 安装布局与导出
│   └── toolchains/                # android/ios/harmony/...
├── include/pime/                  # core 公共 C ABI
│   ├── pime.h                     # 汇总头
│   ├── abi.h                      # 版本、导出、pime_status、pime_buffer
│   ├── hal.h                      # 配置、会话、事件、sink
│   ├── core.h                     # create/destroy/pump/version/输入/会话
│   ├── plugin.h                   # pime_plugin_ref、插件与代管理
│   └── backend.h                  # pime_ml_backend ABI
├── src/
│   ├── hal/                       # C ABI 实现：参数校验、事件规整、sink 分发
│   ├── core/                      # pime_core、session、generation、pipeline、registry
│   ├── host/                      # WASM / Lua / native 插件宿主
│   │   ├── wasm/
│   │   ├── lua/
│   │   └── native/
│   ├── capabilities/
│   │   ├── net/                   # net.http / net.sse / net.ws / net.wss
│   │   ├── crypto/                # digest / MAC / cipher / AEAD / PKEY / KDF / X.509 / CSR / random
│   │   ├── audio/                 # audio.pcm / audio.opus / capture / playback
│   │   ├── ml/                    # ml.infer 宿主、backend loader、job
│   │   ├── store/                 # store.kv
│   │   ├── asset/                 # asset.read
│   │   ├── config/                # config.read
│   │   ├── workspace/             # 链内 workspace
│   │   ├── log/                   # 结构化日志
│   │   ├── metric/                # 指标
│   │   ├── time/                  # 单调/墙钟/时区
│   │   ├── timer/                 # after/every/cancel
│   │   ├── caps/                  # caps.query
│   │   └── permission/            # 授权状态与能力检查
│   ├── phl/                       # thread / context_cache / channel / sync / shared_buffer
│   ├── codec/                     # CBOR / manifest / package / signature
│   ├── platform/
│   │   ├── common/                # 无 OS 分支的公共接口
│   │   ├── windows/
│   │   ├── android/
│   │   ├── ios/
│   │   ├── macos/
│   │   ├── harmony/
│   │   └── linux/
│   └── util/                      # 内部工具，不含业务
├── backends/
│   ├── ml_ncnn/
│   ├── ml_mnn/
│   ├── ml_tflite/
│   └── ml_ort/
├── sdk/
│   ├── wasm/                      # plugin_api.h、导出宏、CBOR schema 头
│   ├── lua/                       # pime.lua 与类型桩
│   ├── native/                    # native 插件 vtable 与 host 函数
│   └── cmake/                     # PimePlugin.cmake
├── schema/
│   ├── manifest.schema.json       # 清单校验
│   └── cbor/                      # event / command / host 请求响应 CDDL
├── examples/
│   ├── plugins/
│   │   ├── wasm/                  # 参考 WASM 插件
│   │   ├── lua/                   # 参考 Lua 插件
│   │   └── native/                # 可信 native 插件
│   └── hosts/
│       ├── windows/               # TSF 最小宿主
│       ├── android/               # InputMethodService + JNI
│       ├── ios/                   # InputMethodKit/扩展
│       ├── macos/                 # InputMethodKit
│       ├── harmony/               # IME Kit + Node-API
│       └── linux/                 # IBus/Fcitx5
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── conformance/
│   ├── fuzz/
│   ├── bench/
│   └── fixtures/
├── tools/
│   ├── pimepack/                  # 打包、签名、验签
│   ├── pimelint/                  # 清单、能力、schema 校验
│   ├── pimeabi/                   # ABI/struct 布局检查
│   └── pimebench/
├── third_party/                   # curl / openssl / libwebsockets / opus / resampler / wamr / lua
├── ci/
└── docs/
    ├── core.md
    ├── plugin.md
    └── structure.md
```

## 3 目录职责

| 目录 | 职责 | 允许依赖 |
|---|---|---|
| `include/pime/` | host 侧稳定 C ABI；不含 C++ 类型、STL、平台头 | 无 |
| `src/hal/` | C ABI 入口：参数/长度校验、input/context/surrounding/output 事件规整、sink 调用、异常拦截 | `core`、`include/pime/` |
| `src/core/` | `pime_core` 单例、会话、代、插件链、命令转发 | `host`、`capabilities`、`phl`、`codec` |
| `src/host/` | 插件包校验、WASM/Lua/native 实例、沙箱、host 函数分发 | `codec`、`capabilities`、`phl`、`platform` |
| `src/capabilities/` | net、crypto、audio、ml、store、asset、config、workspace、log、metric、time、timer、caps、permission | `platform`、`codec`、`phl` |
| `src/phl/` | 线程、上下文缓存、channel、mutex/condvar、shared_buffer | `platform` |
| `src/codec/` | CBOR、清单、包、签名结构（不验签）、CBOR golden schema | `util` |
| `src/platform/` | OS 抽象：线程、时间、文件、动态库、socket、共享内存、音频设备 | 系统 SDK |
| `src/util/` | 字符串、整数、UTF-8、错误、无业务工具 | 无 |
| `backends/` | 推理引擎模块，只依赖 `include/pime/backend.h` 与对应 Runtime | Runtime SDK |
| `sdk/` | 插件作者头/桩/构建助手，不进入 core 链接 | `include/pime/` 的 ABI 常量 |
| `schema/` | 清单与 CBOR 的机器可读 schema，供 lint/CI 使用 | 无 |
| `examples/` | 参考插件与宿主，独立构建 | `sdk/`、`include/pime/` |
| `tests/` | 单元、集成、一致性、fuzz、基准 | 公共 ABI；内部测试可加 `src/` |
| `tools/` | 打包、签名、校验、ABI 检查 | `codec`、`capabilities/crypto`、`include/pime/` |
| `third_party/` | 固定版本源码与补丁 | 无 |
| `ci/` | 构建、测试、发布流水线 | 无 |

## 4 公共头文件

| 头文件 | 内容 |
|---|---|
| `pime.h` | 汇总头；只 include 其他公共头 |
| `abi.h` | `PIME_ABI_MAJOR/MINOR`、导出宏、`pime_status`、`pime_buffer`、`size/version` 规则 |
| `version.h` | 构建生成：`PIME_CORE_VERSION_MAJOR/MINOR/PATCH` |
| `hal.h` | `pime_core_config`、`pime_session_config`、事件结构、`pime_host_sink` |
| `core.h` | `pime_core_create/destroy/pump/abi/version`、输入函数、会话、平台生命周期 |
| `plugin.h` | `pime_plugin_ref`、插件安装/卸载/启停/列表、代构建/激活/退休 |
| `backend.h` | `pime_ml_backend`、`pime_ml_backend_query`、张量/原子类型 |

- 公共头只使用 `<stdint.h>`/`<stddef.h>` 与不透明指针；不出现 `std::`、模板、异常、C++ 类或引用；布尔值用 `int`。
- 结构体只追加字段；`size` 由调用方填 `sizeof`；`version` 填 ABI major。
- 导出符号统一 `PIME_API`；Windows 用 `.def` 或 `__declspec(dllexport)`，其余用 visibility。
- 插件作者头在 `sdk/wasm/include/pime/plugin_api.h`、`sdk/lua/`、`sdk/native/include/pime/native_plugin.h`，不混入 `include/pime/`。
- JNI/Node-API 桥接头在 `examples/hosts/*` 或独立宿主仓库，不属于 core 安装产物。

## 5 内部模块边界

```text
include/pime (C ABI)
        ▲
     src/hal
        │
     src/core
        ├──► src/host ──┬──► src/codec
        │               └──► src/capabilities
        ├──► src/capabilities ──► src/platform
        ├──► src/phl ───────────► src/platform
        └──► src/codec
```

- `src/hal` 是 core 库内唯一导出 `pime_*` 符号的目录；只做 ABI 参数检查、事件规整、sink 调用，不承载业务。
- `src/core` 不直接 include WAMR/Lua/curl/OpenSSL/Opus；只调用 `host`、`capabilities`、`phl` 的 C++ 接口。
- `src/host` 是唯一允许 include WAMR/Lua 头的目录；WASM/Lua 实例、scratch、host 函数表都在这里；包验签调用 `capabilities/crypto`。
- `src/capabilities` 对 `host` 暴露纯 C++ 接口；不感知插件运行时。
- `src/phl` 不依赖插件运行时，可单独单元测试；所有句柄校验与代际逻辑在这里。
- `src/codec` 不依赖网络/插件运行时；只解析签名结构，验签由 `host/package` 或 `tools` 调 `capabilities/crypto`；CBOR、清单、包可独立 fuzz。
- `src/platform` 只提供 OS 能力；不得调用 `src/core` 或 `src/host`。

建议文件粒度：

```text
src/hal/         abi.cpp event.cpp sink.cpp config.cpp
src/core/        core.cpp session.cpp generation.cpp pipeline.cpp registry.cpp command.cpp
src/host/        plugin_host.cpp package.cpp manifest.cpp wasm/instance.cpp lua/state.cpp native/module.cpp
src/capabilities/net/       http.cpp sse.cpp ws.cpp
src/capabilities/crypto/    digest.cpp mac.cpp cipher.cpp aead.cpp pkey.cpp kdf.cpp x509.cpp csr.cpp random.cpp
src/capabilities/audio/     pcm.cpp opus.cpp capture.cpp playback.cpp
src/capabilities/ml/        session.cpp job.cpp backend_loader.cpp
src/capabilities/store/     kv.cpp
src/capabilities/asset/     asset.cpp
src/capabilities/config/    config.cpp
src/capabilities/workspace/ workspace.cpp
src/capabilities/log/       log.cpp
src/capabilities/metric/    metric.cpp
src/capabilities/time/      time.cpp
src/capabilities/timer/     timer.cpp
src/capabilities/caps/      caps.cpp
src/capabilities/permission/permission.cpp
src/phl/         thread.cpp context_cache.cpp channel.cpp sync.cpp shared_buffer.cpp
src/codec/       cbor.cpp manifest.cpp package.cpp signature.cpp
```

## 6 命名与编码

- 目录、文件：`snake_case`；公共 C 头 `.h`，内部 C++ 头 `.hpp`，源文件 `.cpp`。
- C ABI：类型/函数 `pime_*`，枚举值 `PIME_*`；不导出 C++ 符号。
- C++ 命名空间：`pime::hal`、`pime::core`、`pime::host`、`pime::cap`、`pime::phl`、`pime::codec`、`pime::platform`。
- 内部错误：`pime::Status` / `pime::Result<T>`；热路径不抛异常；C ABI 边界捕获全部异常并转为 `pime_status`/`NULL`。
- 不跨 ABI 传 `std::string`、`std::vector`、`std::function`、异常、RTTI 对象。
- include 顺序：自身头 → 内部头 → 第三方 → 标准库。
- 平台宏只允许在 `src/platform/` 与 `backends/` 的构建文件中出现。

## 7 构建目标与产物

| 目标 | 类型 | 说明 |
|---|---|---|
| `pime_core` | shared | 默认产物；Windows `.dll`+`.lib`，Android/Harmony/Linux `.so`，macOS `.dylib`，iOS static framework |
| `pime_core_static` | static | 可选，供静态链接宿主 |
| `pime_ml_ncnn` / `mnn` / `tflite` / `ort` | module | 按开关构建；只暴露 `include/pime/backend.h` |
| `pime_pack` / `pimelint` / `pimeabi` / `pimebench` | executable | 开发与发布工具 |
| `pime_smoke` | executable | 最小宿主，驱动公共 ABI |

产物命名：

| 平台 | core | backend |
|---|---|---|
| Windows | `pime_core.dll` + `pime_core.lib` | `pime_ml_*.dll` |
| Android | `libpime_core.so` | `libpime_ml_*.so` |
| iOS | `pime_core.xcframework`（static） | `pime_ml_*.xcframework` |
| macOS | `libpime_core.dylib` 或 `pime_core.framework` | `libpime_ml_*.dylib` |
| HarmonyOS | `libpime_core.so` | `libpime_ml_*.so` |
| Linux | `libpime_core.so` | `libpime_ml_*.so` |

- `cmake/PimeVersion.cmake` 生成 `build/generated/pime/version.h`；仓库不提交。
- CMake 选项：`PIME_BUILD_WASM`、`PIME_BUILD_LUA`、`PIME_BUILD_BACKEND_*`、`PIME_BUILD_TESTS`、`PIME_SANITIZE_*`；移动端默认只开随包所需项。
- 安装布局：

```text
bin/            pime_pack, pimelint, pimeabi, pimebench
lib/            pime_core, pime_ml_*
include/pime/   公共头
share/pime/     CMake config 与 schema
```

- 第三方：固定版本源码放 `third_party/<lib>/`，补丁放 `third_party/<lib>/patches/`；不使用系统库（平台框架除外），保证可复现。

| 依赖 | 用途 |
|---|---|
| libcurl | `net.http` / `net.sse` / TLS |
| libwebsockets | `net.ws` / `net.wss` |
| OpenSSL | `crypto.*` / TLS / 随机源 |
| libopus | `audio.opus` |
| resampler | `audio.pcm` |
| WAMR | WASM AOT 运行时 |
| Lua 5.4 | Lua 运行时 |

- Android JNI、HarmonyOS Node-API、Windows TSF、macOS/iOS InputMethodKit 的桥接代码在 `examples/hosts/` 或独立宿主仓库，由宿主构建，不进入 `pime_core`。

## 8 测试与 CI

| 目录 | 内容 |
|---|---|
| `tests/unit/` | CBOR、清单、状态码、缓冲区、上下文缓存、channel、锁、UTF-8 |
| `tests/integration/` | 加载 WASM/Lua/native 插件、链调用、会话、代切换、PHL |
| `tests/conformance/` | ABI 布局、枚举值、CBOR golden、包签名、版本解析、错误码 |
| `tests/fuzz/` | 清单、CBOR、包、网络帧、音频帧 |
| `tests/bench/` | 同步链、缓冲池、上下文命中、channel 吞吐、pump 延迟 |
| `tests/fixtures/` | golden 文件、测试插件、测试证书/公钥、模型桩 |

CI 矩阵：Windows x64/arm64、macOS arm64/x64、Linux x64/arm64、Android arm64/x64、iOS arm64/simulator、Harmony arm64；按平台启用 ASan/UBSan/MSan，`pimeabi` 检查在每个 PR 跑。

## 9 工具

- `pimepack`：校验包、计算 `code_hash`、生成 detached `SIGNATURE`、原子安装。
- `pimelint`：manifest、能力、版本约束、CBOR schema、路径白名单。
- `pimeabi`：比较公共头与导出符号；结构体布局/枚举值变更必须显式。
- `pimebench`：固定负载，输出同步链、缓冲、上下文、channel 指标。

## 10 禁止事项

- 不在 `src/platform/` 之外写平台分支或 include 系统头；`backends/` 的平台构建逻辑除外。
- 不在 `src/core/` 直接 include 第三方头；只在 `src/host/{wasm,lua}`、`src/capabilities/{net,crypto,audio}` 适配层与 `backends/` 内 include。
- 不在 `include/pime/` 放 C++ 类型或插件作者 API。
- 不把分词、词典、排序、候选、状态机写入 core。
- 不提交生成头、构建产物、生产签名私钥、模型权重。
- 不让测试专用代码进入生产目标。
- 不在 `docs/` 新增第四份文档；结构变更只改本文件。
- 不要过分解读文档的含义
- 不要防御式编程，主体逻辑没问题就行了，一些极端边界情况没必要去考虑
- 不要过分test
