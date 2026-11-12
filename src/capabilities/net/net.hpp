#ifndef PIME_CAPABILITIES_NET_HPP
#define PIME_CAPABILITIES_NET_HPP

#include <pime/abi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cap_context.hpp"
#include "codec/cbor.hpp"

namespace pime::cap {

struct NetConfig {
    std::string proxy;
    std::string ca_file;
    std::string client_cert;
    std::string client_key;
    bool allow_insecure = false;
};

struct NetEvent {
    std::string alias;
    uint64_t generation = 0;
    codec::Value payload; /* includes "type" */
};

/* net.http / net.sse / net.ws / net.wss. Runs off the owner thread. */
class NetService {
public:
    using EventSink = std::function<void(const NetEvent&)>;

    NetService(NetConfig config, EventSink sink);
    ~NetService();

    pime_status http(const PluginCapContext& ctx, const codec::Value& request,
                     uint64_t* request_id);
    /* Completed HTTP responses are polled by request id. */
    pime_status take_result(uint64_t request_id, codec::Value* out);

    pime_status open(const PluginCapContext& ctx, const std::string& proto,
                     const std::string& url, const codec::Value& options,
                     uint64_t* connection_id);
    pime_status send(uint64_t connection_id, const void* data, uint32_t len);
    pime_status close(uint64_t connection_id);

    void cancel_generation(const std::string& alias, uint64_t generation);
    void pause();
    void resume();
    void set_network_state(uint32_t state);
    uint64_t open_streams() const;
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pime::cap

#endif
