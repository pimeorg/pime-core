# pime-core

## 1 形态

Core 是原生库，不是进程、不是 App：

| 平台 | 产物 |
|---|---|
| Windows | `pime_core.dll` + `.lib` |
| Android | `libpime_core.so` + JNI 桥 |
| iOS | 静态 framework |
| macOS | `.dylib` / framework |
| HarmonyOS | `.so` + Node-API 桥 |
| Linux | `.so` |

宿主 App（TSF DLL、Android IME Service、iOS 键盘扩展等）加载或链接 core，并实现 HAL 回调。Core 没有 `main`，不创建 UI，不假设进程模型。

核心依赖：libcurl（HTTP/HTTPS/SSE）、libwebsockets（WS/WSS）、OpenSSL（全量）、libopus、PCM 重采样库。
推理依赖：`ml.infer` backend（NCNN / MNN / TFLite / ONNX Runtime），按插件 `requires` 加载。

## 2 定位

Core 提供五件事：

1. **可插拔 HAL**：宿主 App 与 core 之间的 C ABI 边界。
2. **插件宿主**：安全、可计量地运行 WASM 与 Lua 插件。
3. **数据管道**：输入 → 插件链 → 输出指令。
4. **共享能力**：网络（HTTP/HTTPS/SSE/WS/WSS）、加解密（OpenSSL 全量）、音频（PCM 转换/重采样、Opus 编解码）、存储、配置、资产、日志、指标、线程/上下文缓存/channel/mutex/shared_buffer、推理宿主（`ml.infer`，引擎在独立 backend 模块）。
5. **统一约束**：安全、性能、跨平台行为一致。

Core **不做**输入法业务：不分词、不查词典、不排序、不生成候选、不维护输入法状态机。这些全部由插件完成。

## 3 边界

| Core | 插件 |
|---|---|
| HAL、线程、上下文缓存、同步原语、生命周期 | 输入解释、分词、组词 |
| WASM/Lua 宿主、配额、隔离 | 词典、语言模型、排序 |
| 网络、加解密、存储、配置、资产 | 候选生成、学习、标点、键位 |
| 数据格式、能力模型、错误码 | 输入法模式、schema 逻辑 |
| `ml.infer` ABI、backend 加载与配额 | 模型选择、前后处理、解码 |
| UTF-8 内部表示、编码边界转换 | 业务规则与数据 |

Core 只搬数据、提供能力，不解释业务。

## 4 HAL

HAL 是宿主与 core 之间的 C ABI：宿主调用输入与生命周期函数，core 通过 sink 回调宿主。每个平台一份适配器，薄、可替换，不含插件逻辑。

```c
#define PIME_ABI_MAJOR 1
#define PIME_ABI_MINOR 0

typedef enum {
  PIME_OK = 0,
  PIME_INVALID = 1,
  PIME_NOT_FOUND = 2,
  PIME_STALE = 3,
  PIME_BUSY = 4,
  PIME_TIMEOUT = 5,
  PIME_FULL = 6,
  PIME_EMPTY = 7,
  PIME_CLOSED = 8,
  PIME_POISONED = 9,
  PIME_BUFFER_TOO_SMALL = 10,
  PIME_NO_MEMORY = 11,
  PIME_RESOURCE = 12,
  PIME_QUEUE_FULL = 13,
  PIME_UNSUPPORTED = 14,
  PIME_DENIED = 15,
  PIME_IO = 16,
  PIME_CORRUPT = 17
} pime_status;

/* 句柄：(generation << 32) | index；0 无效 */
typedef uint64_t pime_buffer;
typedef struct pime_core pime_core;
typedef struct pime_session pime_session;

typedef struct {
  uint32_t size, version;
  const char* plugin_dir;   uint32_t plugin_dir_len;
  const char* data_dir;     uint32_t data_dir_len;
  const char* backend_dir;  uint32_t backend_dir_len;
  const char* trust_store;  uint32_t trust_store_len;  /* 目录：roots.pem + revoked.txt，可空 */
  const char* proxy;        uint32_t proxy_len;        /* 可空 */
  const char* ca_file;      uint32_t ca_file_len;      /* 可空 */
  const char* client_cert;  uint32_t client_cert_len;  /* 可空 */
  const char* client_key;   uint32_t client_key_len;   /* 可空 */
  const uint8_t* store_key; uint32_t store_key_len;    /* 32B，可空 */
  uint32_t log_level;
  uint32_t flags;                                      /* 4.1 */
  uint64_t memory_budget_mb;
} pime_core_config;

typedef struct {
  uint32_t size, version;
  uint64_t id;                                         /* 0 = core 分配 */
  uint32_t flags;
} pime_session_config;

typedef struct {
  uint32_t size, version;
  const char* id;      uint32_t id_len;
  const char* semver;  uint32_t semver_len;  /* SemVer；enable 空=当前激活，unload 必须精确 */
} pime_plugin_ref;

typedef enum {
  PIME_LOG_ERROR = 0, PIME_LOG_WARN, PIME_LOG_INFO, PIME_LOG_DEBUG, PIME_LOG_TRACE
} pime_log_level;

typedef struct {
  uint32_t size, version;
  uint64_t time_ms;                          /* 单调时钟毫秒 */
  uint32_t level;
  const char* source; uint32_t source_len;   /* "core" 或 "plugin:<id>@<version>" */
  const char* tag;    uint32_t tag_len;
  const char* msg;    uint32_t msg_len;      /* UTF-8，仅回调期间有效 */
} pime_log_record;

typedef struct {
  uint32_t size, version;
  const char* text; uint32_t text_len;      /* UTF-8 */
} pime_text;

typedef struct {
  uint32_t size, version;
  uint32_t keycode;                          /* 平台无关键值 */
  uint32_t modifiers;                        /* ctrl/alt/shift/meta/caps */
  uint32_t action;                           /* down / up / repeat */
  const char* text; uint32_t text_len;       /* 可打印字符，可空 */
  uint64_t time_ms;
} pime_key_event;

typedef struct { uint32_t x, y, pressure; uint64_t t_ms; } pime_touch_point;

typedef struct {
  uint32_t size, version;
  uint32_t phase;                            /* down / move / up / cancel */
  const pime_touch_point* points; uint32_t point_count;
  uint64_t time_ms;
} pime_touch_event;

typedef struct {
  uint32_t size, version;
  uint32_t index;                            /* 候选全局序号 */
} pime_candidate_event;

typedef struct {
  uint32_t size, version;
  const char* app_id; uint32_t app_id_len;   /* 需 context.read(app) 授权 */
  uint32_t field_type;                       /* text/password/number/email/url/phone */
  uint32_t input_type;                       /* keyboard/handwriting/voice */
  const char* locale; uint32_t locale_len;   /* BCP 47 */
  uint32_t page_size;                        /* 候选每页条数 */
  uint32_t host_caps;                        /* surrounding 等宿主能力位 */
} pime_context;

typedef struct { uint32_t start, end, type; } pime_segment;  /* 字符索引 */

typedef struct {
  uint32_t size, version;
  const char* text; uint32_t text_len;
  uint32_t cursor;                           /* UTF-8 字符索引 */
  const pime_segment* segments; uint32_t segment_count;  /* 可空 */
} pime_composition;

typedef struct {
  const char* text; uint32_t text_len;
  const char* comment; uint32_t comment_len;
  const char* label; uint32_t label_len;
} pime_candidate;

typedef struct {
  uint32_t size, version;
  const pime_candidate* items; uint32_t count;
} pime_candidate_list;

typedef struct {
  uint32_t size, version;
  uint32_t rate, channels, format;           /* format: s16le / f32le */
  const void* data; uint32_t data_len;
  uint64_t time_ms;
} pime_audio_frame;

/* core 调用宿主；全部在 owner 线程，返回非 OK 表示宿主拒绝 */
typedef struct {
  uint32_t size, version;
  pime_status (*set_composition)(void* ud, const pime_composition*);
  pime_status (*commit)(void* ud, const pime_text*);
  pime_status (*show_candidates)(void* ud, const pime_candidate_list*);
  pime_status (*hide_candidates)(void* ud);
  pime_status (*audio_out)(void* ud, const pime_audio_frame*);
  pime_status (*beep)(void* ud);
  pime_status (*clear_ink)(void* ud);
  pime_status (*notify)(void* ud, pime_status code);
  void (*on_log)(void* ud, const pime_log_record*);
  void (*wake)(void* ud);   /* 任意 core 线程可调用，宿主调度 pump */
} pime_host_sink;

pime_core* pime_core_create(const pime_core_config*, const pime_host_sink*,
                            void* ud);
void       pime_core_destroy(pime_core*);
void       pime_core_pump(pime_core*);   /* 所有者线程 */
void       pime_core_abi(uint32_t* major, uint32_t* minor);
void       pime_core_version(uint32_t* major, uint32_t* minor, uint32_t* patch);
const char* pime_status_string(pime_status);   /* 静态字符串，不释放 */
void       pime_log_set_level(pime_core*, pime_log_level);
void       pime_log_set_plugin_level(pime_core*, const char* plugin_id,
                                     pime_log_level);

/* 会话 */
pime_session* pime_session_create(pime_core*, const pime_session_config*);
void          pime_session_destroy(pime_session*);
uint64_t      pime_session_id(const pime_session*);

/* 插件与代 */
pime_status pime_plugin_load(pime_core*, const char* package_dir, uint32_t len);
pime_status pime_plugin_unload(pime_core*, const pime_plugin_ref*);
pime_status pime_plugin_enable(pime_core*, const pime_plugin_ref*, int enabled);
pime_status pime_plugin_list(pime_core*, uint8_t* out, uint32_t cap,
                             uint32_t* out_len);   /* CBOR */
pime_status pime_generation_build(pime_core*, const uint8_t* config,
                                  uint32_t len, uint64_t* generation);
pime_status pime_generation_activate(pime_core*, uint64_t generation);
pime_status pime_generation_retire(pime_core*, uint64_t generation);

/* 宿主 → core 输入；owner 线程（audio_in 可从音频线程写采集环） */
pime_status pime_core_key(pime_session*, const pime_key_event*, int* handled);
pime_status pime_core_touch(pime_session*, const pime_touch_event*, int* handled);
pime_status pime_core_text(pime_session*, const pime_text*);
pime_status pime_core_candidate(pime_session*, const pime_candidate_event*);
pime_status pime_core_surrounding(pime_session*, const pime_text* before,
                                  const pime_text* after);
pime_status pime_core_context(pime_session*, const pime_context*);
pime_status pime_core_focus(pime_session*, int focused);
pime_status pime_core_secure(pime_session*, int secure);
pime_status pime_core_reset(pime_session*);
pime_status pime_core_audio_in(pime_session*, const pime_audio_frame*);

/* 平台生命周期；owner 线程 */
pime_status pime_core_pause(pime_core*);
pime_status pime_core_resume(pime_core*);
pime_status pime_core_memory_warning(pime_core*, uint32_t level);
pime_status pime_core_network_changed(pime_core*, uint32_t state);
pime_status pime_core_audio_focus(pime_core*, uint32_t focus);
pime_status pime_core_audio_route(pime_core*, uint32_t route);
pime_status pime_core_permission(pime_core*, const char* capability,
                                 uint32_t len, int granted);
```

约束：

- 所有 sink 回调和输出都在 owner 线程执行；`wake` 可从任意 core 线程调用，宿主据此调度 `pime_core_pump`。
- `pime_core_key` / `pime_core_touch` 的 `handled=1` 表示已消费，宿主不得再插入字符；0 表示放行。
- 创建 core 后先 `pime_core_context`，焦点或应用变化时重发；输入函数只在 owner 线程调用，`pime_core_audio_in` 可从宿主音频线程写入采集环。
- 所有指针只在调用期间有效；core 不保存宿主指针，宿主不得保存 core 返回的临时指针。
- C++ 异常不得跨 C ABI；所有入口捕获异常并转为 `pime_status` 或 `NULL`。
- 宿主组合区间、UTF-16 ↔ UTF-8 转换由适配器负责。
- 平台头文件只允许出现在 `platform/`；线程、时间、文件、动态库按目标编译，向上暴露统一接口。

### 4.1 常量与 ABI

- ABI：`PIME_ABI_MAJOR = 1`，`PIME_ABI_MINOR = 0`。插件 `abi.major` 必须相等，`abi.minor` 不得高于 core；struct 的 `version` 填 ABI major。
- `min_core` 是 core SemVer，必须不高于 `pime_core_version()`；ABI 兼容与 core 版本分开判定。
- 签名默认 strict；关闭 `strict_signature` 仅用于开发构建，release 禁止。
- struct：`size = sizeof`；新字段只追加；读取方按 `size` 判断字段是否存在，缺失尾部按 0；native struct 与裸二进制字段 little-endian；CBOR 按 RFC 8949（多字节大端），definite-length。

| 类别 | 值 |
|---|---|
| modifiers | ctrl=1<<0, alt=1<<1, shift=1<<2, meta=1<<3, caps=1<<4, num=1<<5, fn=1<<6 |
| key action | down=0, up=1, repeat=2 |
| touch phase | down=0, move=1, up=2, cancel=3 |
| field type | text=0, password=1, number=2, email=3, url=4, phone=5, search=6, unknown=0xFFFFFFFF |
| input type | keyboard=0, handwriting=1, voice=2, other=0xFFFFFFFF |
| segment type | raw=0, syllable=1, word=2, phrase=3, punctuation=4, other=0xFFFFFFFF |
| audio format | s16le=0, f32le=1 |
| host caps | surrounding=1<<0, secure=1<<1, audio_capture=1<<2, audio_playback=1<<3, candidates=1<<4, touch=1<<5 |
| core flags | allow_insecure=1<<0, strict_signature=1<<1 |
| network state | offline=0, online=1, metered=2, unmetered=3 |
| audio focus | lost=0, gained=1, ducked=2 |
| audio route | speaker=0, receiver=1, headset=2, bluetooth=3 |
| memory level | normal=0, warning=1, critical=2 |

- keycode：`0` 未知；`1..0x10FFFF` 为 Unicode 码点；`0x01000000|code` 为特殊键。特殊键从 `0x01000001` 起：backspace、tab、enter、escape、left、right、up、down、home、end、page_up、page_down、insert、delete；F1 从 `0x01000010` 起。
- 文本索引统一为 Unicode scalar value（码点），不是 UTF-8 字节，也不是 grapheme。

### 4.2 会话、插件与代

- `pime_core_create` 进程内单例；第二次调用返回 `NULL`，`pime_core_destroy` 释放单例。创建期间不调用 sink；config 内字符串与 `store_key` 在返回前拷贝。
- `pime_session_create` 分配会话；`state` / composition / 焦点 / secure 均按会话隔离；`destroy` 等待在途调用结束。
- `pime_plugin_load` 校验包、签名、ABI、平台、版本后安装，不激活；同一平台/架构的同 `(id, version)` code hash 必须一致；可并存多个版本。
- `pime_plugin_enable/disable` 按 `pime_plugin_ref` 精确控制；`unload` 被任一代或会话引用时返回 `BUSY`，否则删除该版本。
- `pime_plugin_list` 返回 CBOR 数组：`{id, version, code_hash, active, enabled, generations[], state}`。
- `pime_generation_build` 接收 CBOR 链配置 `{plugins:[{id, version?, alias?, role, settings}]}`；省略 version 时选满足条件的最高版本；同 id 多版本必须用不同 alias；构建后 `activate` 切换当前代，`retire` 退休旧代。
- `config` 快照 = core 配置 + 代配置 + 插件 `settings`，构建代时冻结；运行中不热改，改配置即构建新代。
- 代切换：在途同步调用完成后返回；该代 async 结果按 generation 丢弃；插件线程关闭并 join。

### 4.3 平台生命周期

- `pause`：停止音频采集/播放，暂停 timer 与 net 流；会话状态保留；`resume` 恢复并重连。暂停期间错过的 timer 不补发。
- `memory_warning`：`warning` 淘汰未 pin 的 TTL 上下文、收缩池与缓存；`critical` 继续淘汰并停止空闲线程；仍不足则后续分配返回 `NO_MEMORY`。pinned 项不淘汰。`memory_budget_mb` 是 core 总预算；插件 `memory_mb` 是单插件沙箱上限，二者独立记账。线程栈、channel、shared_buffer 由 core 记账，计入 `memory_budget_mb`，不计入插件 `memory_mb`；插件可因此耗尽总预算，宿主可禁用插件。
- `network_changed`：core 暂停/恢复 net 流，并向插件投递 `net.state` 事件；`metered` 下不自动重连大流量流。
- `audio_focus` / `audio_route`：core 停止或恢复播放，并向插件投递 `audio.focus` / `audio.route` 事件。
- `permission`：宿主授予/拒绝能力；拒绝后相关调用返回 `PIME_DENIED`，并向插件投递 `permission` 事件。未声明的能力仍默认拒绝；未知能力返回 `INVALID`，重复授予幂等。
- iOS 扩展由宿主设置 `memory_budget_mb`；core 以该值作为预算，不假设进程可用内存。

### 4.4 线程与重入

| 函数 | 线程 |
|---|---|
| `pime_core_create/destroy` | 进程初始化/退出线程，不可并发 |
| `pime_core_pump` | owner |
| 输入（`audio_in` 除外）、会话、插件、代、平台生命周期 | owner |
| `pime_core_audio_in` | 任意线程，写入采集环 |
| `pime_log_set_level` / `pime_status_string` / `pime_core_abi` / `pime_core_version` | 任意线程 |

- sink 在 owner 线程执行，不得同步回调 core；需要回注时先入队，返回后由宿主调用输入函数。
- `pime_core_pump` 推荐间隔 ≤10ms；队列有界，满时 `emit` 返回 `QUEUE_FULL`。
- `pime_core_destroy` 不得从 sink 或插件线程调用；它等待全部 core 线程退出。
- `wake` 只做唤醒，不做业务；宿主可合并多次唤醒。

### 4.5 插件版本

- 插件版本是 SemVer 2.0 `MAJOR.MINOR.PATCH`；`(id, version)` 唯一标识一个包，`code_hash` 标识二进制与资产内容（安装时由 core 计算 SHA-256）。
- 同一平台/架构的 `(id, version)` 必须同 `code_hash`；重复安装但哈希不同返回 `CORRUPT`，不覆盖。
- 多版本可并存；一个代内默认每个 `id` 只激活一个版本；需要并行（迁移、A/B）时显式给 `alias`，alias 在该代内唯一。
- 解析顺序：平台/架构 → ABI major → `min_core` → `requires` 版本约束 → 取满足条件的最高 SemVer；无满足返回 `NOT_FOUND`，不降级、不回退。`requires` 可引用能力或插件，插件依赖参与解析，循环或缺失返回 `NOT_FOUND`。
- 代记录精确的 `(id, version, code_hash, alias)`；代不可变。升级 = 构建并激活新代；回滚 = 重新激活旧代；旧代保留到显式 `retire`。
- 卸载某版本：被任一代、会话、上下文租约引用时返回 `BUSY`；删除后该版本不能参与新代。`unload` 必须给精确版本，空版本返回 `INVALID`；`enable` 空版本作用于当前激活版本。
- `pime_core_version` 返回 core SemVer；`min_core` 与它比较，ABI 兼容仍由 `abi.major/minor` 单独判定。
- `state` / `store` 记录 plugin version 与 schema；迁移由插件在 `init` 完成，core 不清空、不改写。降级需显式 pin，且插件声明可读旧 schema；否则返回 `STALE`。
- 上下文键使用 `alias` + `code_hash`；日志 `source` 用 `id@version`；`alias` 默认 `id`；code hash 变化则 `DATA` / `INSTANCE` 不静默复用。
- 上下文与 `store` 命名空间使用 `alias`；升级（同 alias）共享数据，靠 `state_schema` / `store_schema` 迁移；并行版本用不同 alias 隔离。

## 5 插件宿主

- 运行时：**WASM（WAMR AOT）** 与 **Lua 5.4**，共用同一清单、能力、数据 schema、错误码。
- 默认插件只通过 host API 访问能力，不直接链接 libcurl/OpenSSL。
- 可信 native 插件仅作逃生舱：随包、签名、显式启用，可自带 Runtime；不受沙箱保护，计入 backend 版本上限。
- WASM 用于热路径；Lua 用于控制、配置与低频逻辑；同步链准入按实测预算。
- 配额：仅指沙箱运行时（线性内存、指令数、墙钟时间、输出大小、state/workspace、存储、网络并发）；PHL 的线程/上下文/channel/shm 不设人为数量上限，资源耗尽返回明确错误。
- 失败：trap / 超时 / 越界 / 配额 → 禁用并隔离，core 与会话继续运行。

### 5.1 线程模型

Core 进程内单例；下列线程归 core，宿主只提供 owner 线程与音频回调。

| 线程 | 创建 | 用途 |
|---|---|---|
| owner | 宿主决定 | HAL 回调、`pump`、sink |
| worker pool | core 启动 | 共享插件任务、异步收尾 |
| audio | 首个采集/播放流 | PCM/Opus 管线，两个环 |
| HTTP / SSE | 首个请求/流 | libcurl multi |
| WS / WSS | 首个连接 | libwebsockets |
| timer | core 启动 | 定时器合并与投递 |
| backend | backend 加载 | 推理执行 |

- 插件可申请**自己的线程**：`thread.create / close / join / post / park / unpark / info`。默认保留到插件显式 `close` 或卸载，core 不按空闲回收。
- 不设线程总数、每插件线程数、CPU 份额上限；创建失败返回 `RESOURCE` / `NO_MEMORY`。
- 线程参数（栈、优先级、亲和性）**要么按请求生效，要么返回 `UNSUPPORTED`**；不静默改成别的值。
- 插件线程优先级只允许 `background / normal / interactive`；`critical / audio` 归 core。
- 不提供 detach。插件卸载、代退休时 core 关闭并 join 全部插件线程；插件必须让线程在收到关闭信号后退出。
- 任意插件线程通过 `emit(session_id, output, len)` 把命令投递到 owner 线程的 `pump`；投递队列满返回 `QUEUE_FULL`，不丢、不合并。
- 插件线程可调用全部 host API；涉及会话状态（`state` / `workspace` / 输出）的调用必须带 `session_id`，由 core 串行化到该会话。
- WASM 线程各自持有独立实例与线性内存；Lua 线程各自持有独立 `lua_State`。跨线程不共享栈、不共享 GC 对象，用 channel / shared_buffer 传数据。
- 线程 park/unpark 使用 core 的等待原语，不暴露平台线程句柄。

### 5.2 上下文缓存

高频但时在时不在的插件，用**命名上下文**保留工作集，避免反复加载/重建。键为 `(alias, name, schema, code_hash)`；`alias` 默认 `plugin_id`。

| 类型 | 内容 | 插件卸载后 | 淘汰 |
|---|---|---|---|
| `DATA` | 插件字节（词典索引、缓存） | 保留 | TTL、pin、内存压力 |
| `INSTANCE` | 预热实例或实例池 | 保持加载 | TTL、pin、code hash 变化 |
| `RESOURCE` | core 句柄（模型 session、资产映射、线程、shared_buffer） | 由 owner 决定 | 引用计数、pin、TTL |

- `ctx.acquire(name, kind, schema, flags)`：命中则引用计数 +1；`CREATE` 未命中时创建；`REQUIRED` 未命中返回 `NOT_FOUND`；未指定 `CREATE` 视为 `REQUIRED`；schema 或 code hash 不符返回 `STALE`，不静默复用旧数据。
- `ctx.release` 减引用；到 0 后按 TTL 保留。`ctx.pin` / `ctx.unpin` 控制淘汰；pinned 项永不淘汰。
- `ctx.drop` 删除；仍有租约返回 `BUSY`。`ctx.info` 返回类型、大小、引用数、pin、TTL、最后一次使用。
- 不设上下文数量、实例池大小、单插件缓存上限；内存不足返回 `NO_MEMORY`，由插件决定是否清理。
- `INSTANCE` 调用走 `ctx.call(ctx, entry, in, out)`；池内无空闲实例时**新建**，不排队、不复用忙实例；创建失败返回 `RESOURCE`。
- 插件代码 hash 变化或代退休时，`INSTANCE` 不再接新租约；已有调用完成后销毁。
- `DATA` 跨卸载保留，但读取时校验 schema 版本；不兼容返回 `STALE`。
- `RESOURCE` 的 owner 是创建者；共享需在清单声明 `ctx.share` 并在 acquire 时指定 owner alias 命名空间。
- 上下文缓存是**显式 API**，core 不自动猜测、不自动迁移、不自动降级。

### 5.3 同步原语

- `channel`：多生产者/多消费者，FIFO。`capacity = 0` 为无界（直到分配失败）；`capacity > 0` 为有界，满时 `send` 返回 `TIMEOUT` / `FULL`。
- `channel.close` 默认 `DRAIN`：已入队消息仍可接收，之后 `recv` 返回 `CLOSED`；`ABORT` 显式丢弃未消费消息。不静默丢消息。
- 消息三种模式：`INLINE`（小消息拷入队列）、`BUFFER`（池化 buffer 句柄）、`SHM`（shared_buffer 引用 + 偏移/长度）。
- `mutex`：默认非递归；`RECURSIVE` 需创建时显式声明。`unlock` 必须由持有线程调用；持有线程死亡后锁进入 `POISONED`，不自动解锁、不静默恢复。
- `condvar`：必须与 mutex 配对；`wait` 原子释放并重新获取；`close` 唤醒全部等待者并返回 `CLOSED`。
- `shared_buffer`：core 持有、引用计数；native 插件 `map` 零拷贝；WASM/Lua 用 `read/write` 或拷入 scratch。存在 map 租约时 `resize` 返回 `BUSY`。
- 所有同步对象都是索引 + 代际句柄；跨代使用返回 `STALE`；不暴露裸指针。
- 不设 channel 长度、消息数、锁数量上限；资源耗尽返回 `NO_MEMORY` / `RESOURCE`。
- 不提供隐式同步：shared_buffer 的并发读写由插件用 mutex/channel 自行协调。
- 句柄关闭后所有等待者被唤醒并得到 `CLOSED`；重复关闭返回 `INVALID`。

错误码见第 4 节的 `pime_status`，WASM/Lua 同名；只有 `OK` 表示请求完整生效，任何降级都不得返回 `OK`。

## 6 数据管道

- 输入：`{event, state_in, workspace, context, config, assets}`。
- 输出：`{state_out, workspace_delta[], handled, commands[]}`。
- `state` 是插件私有字节，默认上限 64KB，清单可调至 1MB。
- `workspace` 是同一次链调用内共享的有界键值区，默认 256KB；所有插件可读，写入自动加自身前缀 `plugin/<id>/`；调用结束即失效。
- `handled`：链上任一插件置 true 或产生命令，core 返回宿主“已处理”，否则放行按键。
- `commands`：组合（含光标/分段）、提交、候选、隐藏、通知、蜂鸣、清笔迹；core 按顺序转发给 HAL。
- 插件可声明 `sync` 或 `async`；async 结果和 `on_event` 输出带 `generation`，代不匹配丢弃。
- `on_event` 与 `process` 同样返回 output；网络、音频、推理、定时器结果都从 `on_event` 产出命令。
- 插件链顺序由配置声明；core 只认“有序插件”，不认输入法阶段。

### 6.1 缓冲与共享内存

- 高频数据（PCM、Opus 包、网络消息、候选快照、日志）全部走**池化缓冲**；热路径不 malloc/free。
- `pime_buffer`：引用计数 + 所属池 + 代际。`acquire/release`；释放归池，不归还系统堆。
- **按最大可能帧长分配**：每类缓冲一次性分配为该类允许的最大帧长；实际长度单独记录，小帧复用大块，不缩容、不重分配。
- 固定上限：

| 类别 | 最大帧长 | 槽大小 |
|---|---|---:|
| PCM | 48kHz × 2ch × 32bit × 40ms = 15,360B | 16KB |
| Opus | RFC 6716 上限 1,275B | 2KB |
| 网络消息 | 默认 64KB；大消息独立 1MB 类 | 64KB / 1MB |
| 候选快照 | 200 条 × 128B（含注释/标签） | 64KB |
| 文本 | 组合/提交文本 | 4KB |
| 日志 | 1KB/条 | 1KB × 64 |

- 槽数：音频 8、Opus 32、网络 4、候选 2（按会话）；日志 64（按 core）。总量计入内存预算；更高采样率或更大消息必须显式提高上限并重新计入预算。
- 环形缓冲：HAL→core 采集环、core→HAL 播放环、网络收发环。有界；音频满丢最旧帧，网络满施加背压。
- 所有权：生产者填充 → 消费者 retain → 最后一个 release 归池；句柄不得跨会话或跨代际。
- 句柄是索引 + 代际，不是裸指针；重复释放、跨代使用、长度越界一律拒绝并上报。
- 资产只读映射，不计入插件线性内存；WASM 按需分段读入 scratch。
- 跨沙箱：
  - native 插件：直接传句柄，零拷贝。
  - WASM：写入插件导出的持久 scratch 区；免分配，但仍有一次拷贝；插件返回前必须消费完。
  - Lua：`pime.buffer` userdata 持有池化内存；Opus/PCM/网络接口直接收句柄，避免 Lua 字符串复制。
- 同进程组件共享池化缓冲；进程外 backend 的张量走 OS 共享内存 + CBOR 控制消息；接口不暴露裸指针。
- 安全：敏感音频释放时清零；安全模式停止采集并清空池；跨代句柄立即失效。
- 指标：池命中/未命中、分配次数、丢帧、背压次数。

### 6.2 日志

- 统一结构化日志：`{time_ms, level, source, tag, msg}`，UTF-8；core 与插件共用 error/warn/info/debug/trace。
- 固定 1KB 记录、64 槽 MPSC 环；写满丢最旧并计数，随下一条上报；生产者不碰 I/O。
- `on_log` 由 `pime_core_pump` 在所有者线程投递；关闭级别只做一次分支，开启时格式化到栈缓冲再拷入池化槽，无 malloc。
- 插件日志强制 `source = plugin:<id>@<version>`；默认限速 64KB/s、单条截断 1KB；过滤 NUL 与控制字符。`pime_log_set_plugin_level` 接受 `id` 或 `id@version`；`id` 作用于全部版本。
- 级别运行时可调、可按插件覆盖；release 默认 `info`。
- 不记录按键、文本、候选、密钥、URL 查询串；安全模式不记录事件内容。
- core 内部使用同一套 C++ 日志宏，走同一管道。

## 7 能力

| 能力 | 实现 | 规则 |
|---|---|---|
| `input.read` | HAL | 安全模式下停发原始按键 |
| `context.read` | HAL | app id 需单独授权；字段类型、locale、页大小 |
| `surrounding.read` | HAL | 光标前后文本；宿主能力可选 |
| `output.emit` | HAL | 只发数据，不碰宿主指针 |
| `config.read` | 不可变快照 | 类型化 get/list、快照 id；热路径无锁 |
| `asset.read` | 只读资产根 | list/open/size/read/map；路径白名单 |
| `store.kv` | core | get/set/del/batch/scan/quota/flush；按插件命名空间隔离 |
| `workspace` | core | 链内共享；读任意、写自身前缀；调用结束失效 |
| `thread` | core | create/close/join/post/park/unpark/info；保留到 close 或卸载；无数量上限 |
| `ctx` | core | DATA/INSTANCE/RESOURCE；acquire/release/pin/unpin/drop/info；引用计数 + TTL |
| `emit` | core | 插件线程向 owner `pump` 投递 output；队列满返回 `QUEUE_FULL` |
| `channel` | core | 多生产/多消费 FIFO；`0` 无界；有界满返回 `FULL`/`TIMEOUT`；close 默认 `DRAIN` |
| `sync` | core | mutex/condvar；默认非递归；持有线程死亡 → `POISONED` |
| `shm` | core | core 持有、引用计数；native `map` 零拷贝；WASM/Lua `read`/`write` |
| `time` | core | 单调/墙钟/时区 |
| `timer` | core | after/every/cancel；异步事件 |
| `caps.query` | core | 返回已授予能力 + core ABI；不混用宿主能力位 |
| `log` / `metric` | core | 分级、限速、命名空间隔离；release 不记录输入内容 |
| `net.http` | libcurl | HTTP/HTTPS，请求/响应与流式响应，异步 |
| `net.sse` | libcurl | `text/event-stream` 长连接，断线重连，空闲超时 |
| `net.ws` / `net.wss` | libwebsockets + OpenSSL | 全双工消息/二进制帧，心跳、重连、背压 |
| `net.insecure` | libcurl/libwebsockets | 允许明文 HTTP/WS，需单独授权 |
| `crypto.*` | **OpenSSL 全量** | digest / MAC / cipher / AEAD / PKEY / KDF / X.509 / CSR / 随机；算法名直通 OpenSSL |
| `random` | OpenSSL CSPRNG | 统一随机源 |
| `audio.pcm` | 重采样/格式转换 | 采样率、声道、位深、混音、增益 |
| `audio.opus` | libopus | 编码/解码、FEC/PLC、分帧 |
| `audio.capture` / `audio.playback` | HAL | 麦克风/扬声器流，权限由宿主申请 |
| `ml.infer` | backend | load/run/run_async/cancel/free；见 7.1 |

网络与加解密都通过 host API 暴露，WASM 与 Lua 语义一致。插件不得直接打开 socket、加载动态库或读取密钥文件。

### 7.1 模型推理

Core 提供 `ml.infer` 的 C ABI 与 backend 宿主；推理引擎由 backend 模块实现，按插件声明加载。

| 层 | 内容 | 提供者 |
|---|---|---|
| core | `load / run / run_async / cancel / free`、张量视图、配额、签名校验、backend 发现 | pime-core |
| backend | `pime_ml_ncnn` / `pime_ml_mnn` / `pime_ml_tflite` / `pime_ml_ort`，内部链接对应 Runtime | 第一方 CI 或第三方厂商 |
| plugin | 模型资产 + `requires: ml.infer@1, engine=ncnn@X` | 插件作者 |

- backend 静态链接引擎与 C++ 运行时，只暴露 C ABI；不跨边界传 C++ 对象。
- WASM/Lua 插件**不打包 Runtime**，只带模型与清单。
- 两个插件声明同一 Runtime 键时，共用同一 backend，不会各自加载一份引擎。
- 张量统一为 `{dtype, shape[4], data}`，输入输出均为数组；走池化 buffer；不暴露引擎原生张量类型。
- 复用键：`(engine, version, build_hash, arch, abi)`。同键共享一个 backend 实例；不同键允许并存，但**最多 2 个版本**，超出拒绝。
- 模型按内容哈希共享只读权重；每个插件独立 session。
- 共享一个推理线程池；按插件做时间、内存、并发配额。
- 大模型走 `run_async`：返回 job id，结果经 `ml.result` / `ml.error` 事件回到插件，在 `on_event` 产出命令；同步 `run` 只用于小模型。
- backend 清单声明：引擎/版本、ABI、平台/架构、模型格式与算子集、最低 core 版本。不匹配只禁用功能，不崩溃。
- 分发：移动端随包签名（iOS 不能下载代码，Android 按 ABI 分包）；桌面随安装包，额外下载只接受签名 backend。
- 隔离：随包第一方 backend 可进程内；下载的第三方 backend 走进程外，崩溃不拖垮输入法。
- 没有可用 backend：功能不可用；由插件显式选择云端或关闭，不影响输入。

### 7.2 native 插件与进程外 backend

Native 插件仅限可信、随包、签名；进程内运行，链接同一 host API 符号，不受沙箱保护。

```c
typedef struct {
  uint32_t size, version;
  pime_status (*init)(const uint8_t* config, uint32_t len, void** state);
  pime_status (*process)(void* state, const uint8_t* in, uint32_t in_len,
                         uint8_t* out, uint32_t cap, uint32_t* out_len);
  pime_status (*on_event)(void* state, const uint8_t* evt, uint32_t len,
                          uint8_t* out, uint32_t cap, uint32_t* out_len);
  void (*shutdown)(void* state);
} pime_native_plugin;

pime_status pime_native_plugin_query(uint32_t abi_major, pime_native_plugin* out);
```

- native 插件的 `config` / `in` / `evt` / `out` 与 WASM 使用同一 CBOR schema；不允许另造协议。
- native 插件线程归插件所有；core 不强制终止，卸载必须 join，超时则禁用并延迟卸载。
- 下载/第三方 backend 走进程外 helper：控制消息 CBOR over 本地 socket/pipe；张量走 OS 共享内存；句柄由 core 映射，跨进程只传 id + offset/len。
- helper 崩溃：管道关闭，在途 job 以 `PIME_IO` 失败，插件收到 `ml.error`；core 在下次 load 时重启 helper，不影响输入。
- backend 的 `desc` / `in` / `out` 也是 CBOR；张量 `data` 是字节串或共享内存引用。
- backend 声明 `copy_ok` 才允许在共享内存不可用时拷贝；否则返回 `UNSUPPORTED`，不静默降级。

```c
typedef struct {
  uint32_t size, version;
  pime_status (*load)(const uint8_t* desc, uint32_t len, uint64_t* session);
  pime_status (*run)(uint64_t session, const uint8_t* in, uint32_t in_len,
                     uint8_t* out, uint32_t cap, uint32_t* out_len);
  pime_status (*cancel)(uint64_t job);
  pime_status (*free)(uint64_t session);
} pime_ml_backend;

pime_status pime_ml_backend_query(uint32_t abi_major, pime_ml_backend* out);
```

## 8 安全

- 能力最小化；清单声明权限；默认拒绝未声明能力。
- 授权：宿主用 `pime_core_permission` 授予/拒绝；未声明或未授予一律 `PIME_DENIED`。`context.read(app)`、`net.insecure`、`ctx.share`、`store.kv` 可单独拒绝。
- 信任：`trust_store` 目录提供 `roots.pem` 与 `revoked.txt`；包签名、backend 签名、模型哈希在加载前校验；吊销列表命中拒绝加载。
- 版本完整性：同一平台/架构的 `(id, version)` 的 `code_hash` 必须一致，不一致拒绝；降级必须显式 pin；解析失败返回 `NOT_FOUND`，不回退旧版本。
- 存储：`store.kv` 使用 `store_key` 做 AES-256-GCM 加密；未提供密钥返回 `PIME_DENIED`，不回退明文。
- 网络配置：代理、CA、客户端证书只从 `pime_core_config` 读取；插件不能覆盖 CA 或关闭校验。
- native / 进程外：native 插件仅可信签名；下载 backend 进程外；IPC 只传 id/offset/len，不传裸指针；崩溃重启不影响进程。
- 安全模式：不投递按键文本、surrounding、app_id；禁用 `net.*`、`store.kv` 写入、`ctx` DATA 写入与日志内容；本地组合/候选/提交仍可用，被拒绝的调用返回 `PIME_DENIED`。
- 网络安全：TLS 验证强制开启；明文 HTTP/WS 需要独立权限；重定向、端口、方法受控；防 SSRF 的主机白名单。
- 流式网络：连接数、消息大小、带宽、重连频率配额；SSE/WS 空闲超时；背压由 core 统一处理。
- 音频：帧大小与缓冲上限；插件不得直接打开麦克风/扬声器，只能经 HAL 流；宿主负责权限申请。
- 上下文：app id 只在授权时提供；字段类型、locale 最小化；surrounding 文本按敏感数据处理，不落日志、不进缓存。
- 工作区：读写前缀强制隔离，大小有上限；跨代清空；不得作为持久化通道。
- 线程：插件线程只运行沙箱代码；不可信 native 插件/backend 进程外运行；不提供 detach，卸载/代退休必须关闭并 join；插件线程不能申请 `critical` / `audio` 优先级；线程创建失败明确报错，不静默降级。
- 上下文：键含 `alias`，命名空间强制隔离；`ctx.share` 需单独授权；`pin` 可阻止淘汰，内存耗尽由插件负责清理；code hash 或代际变化后 `INSTANCE` / `DATA` 立即失效，不静默复用。
- 同步：channel / mutex / shared_buffer 均为索引 + 代际句柄；mutex 持有线程死亡进入 `POISONED`，不自动解锁；关闭后的对象拒绝新操作；shared_buffer 释放时按敏感级别清零。
- 内存安全：跨边界结构校验 `size/version`、空指针、长度/容量与整数溢出；句柄用索引+代际，不暴露裸指针。
- 生命周期安全：池化缓冲校验引用计数与代际，拒绝重复释放、悬垂引用、跨代使用；敏感内存释放清零。
- 越界与崩溃：WASM 线性内存和 Lua 分配器兜底，host 函数再校验入参；解码器与清单做 fuzz；CI 开 ASan/UBSan/MSan；native backend 进程外隔离。
- 密钥安全：插件只拿句柄；签名/解密在 core 内完成；不提供私钥导出。
- 日志安全：插件日志不可信，单条截断并过滤 NUL/控制字符；按插件限速防刷；release 与安全模式不记录按键、文本、候选、密钥。
- 安全输入模式：停止投递原始按键，只投递 `secure` 状态；插件不得缓存历史输入。
- 插件签名与哈希校验使用 OpenSSL；签名不通过拒绝加载。
- 推理：backend 与模型签名校验；模型只读；backend 无文件/网络权限；张量大小与推理时间配额。
- 插件失败隔离；输出边界校验；不因单插件退出。

## 9 性能

- 边界预算：事件分发 ≤50µs；WASM AOT 单次 ≤2µs；Lua 单次 ≤10µs；输出 ≤50µs。
- 同步链总预算 ≤1ms；网络与重加解密只能 async。
- libcurl 使用 multi 接口；libwebsockets 使用独立事件线程；OpenSSL 大运算在工作线程，小哈希可在同步链但计入预算。
- WS/SSE/音频按消息或帧异步处理，使用有界环形缓冲；不阻塞所有者线程。
- `on_event`、定时器、推理结果不占同步链；定时器合并，同一插件同 id 只保留一个。
- 队列有界；溢出只丢最旧 async 结果并上报，同步事件不丢；插件线程 `emit` 不丢，队列满时调用方得到 `QUEUE_FULL`。
- 推理走独立线程池；共享 backend；每插件并发/时间配额；不与同步链抢线程。
- 插件线程按需创建后保留，不与 worker pool 争用；`park`/`unpark` 用等待原语，不轮询。
- 上下文缓存命中只做哈希查找与引用计数，无加载/初始化；`INSTANCE` 池按需扩容，首次 `acquire` 完成预热。
- channel 传 `BUFFER` / `SHM` 句柄，避免大块拷贝；无界 channel 的增长受进程内存约束，压测需覆盖。
- `shared_buffer` native 零拷贝；WASM/Lua 只有显式 `read`/`write` 才发生拷贝。
- 日志关闭时一次分支；开启时无 malloc，写环 O(1)，由 `pime_core_pump` 异步投递，不在同步链做 I/O。
- 插件清单声明预算；CI 与线上指标校验。

## 10 统一

- 同一份插件在所有平台行为一致。
- 同一份数据 schema、能力名、错误码、生命周期。
- 网络与加解密语义由 core 统一，平台差异只在 HAL。
- 相同输入 + 相同资产 + 相同插件 → 相同输出。

## 11 生命周期

`core_create → plugin load → generation build/activate → session create → process / on_event / plugin threads → generation swap → session destroy → close threads → unload plugins → core_destroy`

- 代退休：取消该代的 timer / net / ml / channel 作业，关闭并 join 该代插件线程，`INSTANCE` / `RESOURCE` 停止接新租约。
- 插件卸载：等待引用与租约归零；`DATA` 按 TTL 保留，`INSTANCE` 销毁，`RESOURCE` 交还 owner。
- `core_destroy`：关闭全部线程与同步对象，清空上下文缓存，释放池化内存。

Core 不因单插件或单会话失败退出；HAL 失效由宿主重建 core。

## 12 不做

分词、词典、LM、排序、候选生成、FSM、学习、标点、键位、schema 语义，全部在插件层，见 `docs/plugin.md`。
