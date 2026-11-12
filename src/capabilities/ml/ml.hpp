#ifndef PIME_CAPABILITIES_ML_HPP
#define PIME_CAPABILITIES_ML_HPP

#include <pime/abi.h>
#include <pime/backend.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cap_context.hpp"
#include "codec/cbor.hpp"
#include "platform/platform.hpp"

namespace pime::cap {

struct MlEvent {
    std::string alias;
    uint64_t generation = 0;
    std::string type; /* "ml.result" / "ml.error" */
    codec::Value payload;
};

/* Backend loader, model sessions and async job pool for ml.infer. */
class MlHost {
public:
    using EventSink = std::function<void(const MlEvent&)>;

    MlHost(std::string backend_dir, EventSink sink);
    ~MlHost();

    pime_status load(const PluginCapContext& ctx, const std::string& engine,
                     const codec::Value& desc, uint64_t* session);
    pime_status run(uint64_t session, const std::vector<uint8_t>& input,
                    std::vector<uint8_t>* output);
    pime_status run_async(uint64_t session, std::vector<uint8_t> input,
                          uint64_t* job);
    pime_status cancel(uint64_t job);
    pime_status free_session(uint64_t session);
    void cancel_generation(const std::string& alias, uint64_t generation);
    void shutdown();

private:
    struct Module {
        platform::Library library;
        pime_ml_backend backend{};
        std::string key;
        std::string engine;
    };
    struct Session {
        std::shared_ptr<Module> module;
        uint64_t backend_session = 0;
        std::string alias;
        uint64_t generation = 0;
    };
    struct Job {
        uint64_t id = 0;
        std::shared_ptr<Session> session;
        std::vector<uint8_t> input;
        bool cancelled = false;
    };

    std::shared_ptr<Module> module_for(const std::string& engine,
                                       pime_status* status);
    void worker_loop();
    void finish_job(const Job& job, pime_status status,
                    const std::vector<uint8_t>& output);

    std::string backend_dir_;
    EventSink sink_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Module>> modules_;
    std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions_;
    std::unordered_map<uint64_t, std::shared_ptr<Job>> jobs_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::condition_variable cv_;
    std::thread workers_[2];
    bool stopping_ = false;
    uint64_t next_session_ = 1;
    uint64_t next_job_ = 1;
};

}  // namespace pime::cap

#endif
