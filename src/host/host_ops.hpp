#ifndef PIME_HOST_HOST_OPS_HPP
#define PIME_HOST_HOST_OPS_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pime::cap {
struct PluginCapContext;
}

namespace pime::host {

class PluginInstance;

enum class HostKind : uint8_t {
    Workspace = 0,
    Store,
    Asset,
    Config,
    Ctx,
    Channel,
    Sync,
    Shm,
    Net,
    Crypto,
    Audio,
    Ml,
    Time,
    Timer,
    Caps,
    Thread,
    Log,
    Metric,
    Random
};

enum class WorkspaceOp : uint8_t { Get = 0, Set = 1 };
enum class StoreOp : uint8_t { Get = 0, Set, Del, Batch, Scan, Quota, Flush };
enum class AssetOp : uint8_t { List = 0, Open, Size, Read, Map, Close };
enum class ConfigOp : uint8_t { Get = 0, List, Snapshot };
enum class CtxOp : uint8_t {
    Acquire = 0,
    Release,
    Pin,
    Drop,
    Info,
    Call,
    Set,
    Get
};
enum class ChannelOp : uint8_t { Create = 0, Send, Recv, Close, Depth };
enum class SyncOp : uint8_t {
    MutexCreate = 0,
    MutexLock,
    MutexTryLock,
    MutexUnlock,
    CondvarCreate,
    CondvarWait,
    CondvarSignal,
    CondvarBroadcast,
    Close
};
enum class ShmOp : uint8_t {
    Create = 0,
    Open,
    Read,
    Write,
    Resize,
    Map,
    Unmap,
    Close
};
enum class NetOp : uint8_t { Http = 0, Result, Open, Send, Close };
enum class CryptoOp : uint8_t {
    Digest = 0,
    Mac,
    AeadEncrypt,
    AeadDecrypt,
    Random,
    Verify,
    Sign,
    Kdf,
    X509Verify
};
enum class AudioOp : uint8_t {
    CaptureStart = 0,
    CaptureStop,
    CaptureRead,
    PlaybackStart,
    PlaybackStop,
    PlaybackWrite,
    PcmConvert,
    OpusEncode,
    OpusDecode
};
enum class MlOp : uint8_t { Load = 0, Run, RunAsync, Cancel, Free };
enum class TimeOp : uint8_t { Query = 0 };
enum class TimerOp : uint8_t { After = 0, Every, Cancel };
enum class CapsOp : uint8_t { Query = 0 };
enum class ThreadOp : uint8_t {
    Create = 0,
    Post,
    Recv,
    Park,
    Unpark,
    Close,
    Join,
    Info
};
enum class LogOp : uint8_t { Write = 0 };
enum class MetricOp : uint8_t { Add = 0, Set, Get, Snapshot };

/* Context passed to the bridge for one plugin call. */
struct HostCallContext {
    std::string alias;
    uint64_t generation = 0;
    uint64_t session_id = 0;
    bool secure = false;
    uint32_t host_caps = 0;
    const cap::PluginCapContext* caps = nullptr;
    PluginInstance* instance = nullptr;
};

class HostBridge {
public:
    virtual ~HostBridge() = default;

    virtual pime_status check(const HostCallContext& context,
                              const std::string& capability) = 0;
    virtual pime_status invoke(const HostCallContext& context, HostKind kind,
                               uint8_t op, const uint8_t* in, uint32_t in_len,
                               std::vector<uint8_t>* out) = 0;
    virtual pime_status emit(uint64_t session_id, const std::string& alias,
                             uint64_t generation, const uint8_t* data,
                             uint32_t len) = 0;
};

}  // namespace pime::host

#endif
