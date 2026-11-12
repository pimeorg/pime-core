#ifndef PIME_CAPABILITIES_TIMER_HPP
#define PIME_CAPABILITIES_TIMER_HPP

#include <pime/abi.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pime::cap {

struct TimerFire {
    uint32_t id = 0;
    std::string alias;
    uint64_t generation = 0;
    std::string event_id;
};

/* after/every/cancel. One timer per (plugin, event id). */
class TimerService {
public:
    using Callback = std::function<void(const TimerFire&)>;

    TimerService();
    ~TimerService();

    void start(Callback callback);
    void stop();
    void pause();
    void resume();

    pime_status add(const std::string& alias, uint64_t generation,
                    uint32_t delay_ms, uint32_t repeat_ms,
                    const std::string& event_id, uint32_t* timer_id);
    pime_status cancel(uint32_t timer_id);
    void cancel_generation(const std::string& alias, uint64_t generation);
    void cancel_alias(const std::string& alias);

private:
    struct Timer {
        uint32_t id = 0;
        std::string alias;
        uint64_t generation = 0;
        std::string event_id;
        uint64_t due_ms = 0;
        uint32_t repeat_ms = 0;
        bool cancelled = false;
    };

    void loop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<std::pair<uint64_t, uint32_t>,
                        std::vector<std::pair<uint64_t, uint32_t>>,
                        std::greater<std::pair<uint64_t, uint32_t>>>
        queue_;
    std::unordered_map<uint32_t, Timer> timers_;
    std::unordered_map<std::string, uint32_t> by_event_;
    Callback callback_;
    std::thread thread_;
    uint32_t next_id_ = 1;
    bool running_ = false;
    bool stopping_ = false;
    bool paused_ = false;
};

}  // namespace pime::cap

#endif
