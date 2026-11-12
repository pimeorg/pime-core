#include "ml.hpp"

#include <algorithm>

namespace pime::cap {

namespace {

std::string engine_base(const std::string& engine) {
    size_t at = engine.find('@');
    std::string base = at == std::string::npos ? engine : engine.substr(0, at);
    std::string out;
    for (char c : base) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
        else out.push_back('_');
    }
    return out;
}

}  // namespace

MlHost::MlHost(std::string backend_dir, EventSink sink)
    : backend_dir_(std::move(backend_dir)), sink_(std::move(sink)) {
    for (auto& worker : workers_) worker = std::thread([this] { worker_loop(); });
}

MlHost::~MlHost() { shutdown(); }

std::shared_ptr<MlHost::Module> MlHost::module_for(const std::string& engine,
                                                   pime_status* status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find(engine);
    if (it != modules_.end()) {
        *status = PIME_OK;
        return it->second;
    }
    const std::string base = engine_base(engine);
    if (base.empty()) {
        *status = PIME_INVALID;
        return nullptr;
    }
    uint32_t versions = 0;
    for (const auto& kv : modules_) {
        if (engine_base(kv.second->engine) == base) ++versions;
    }
    if (versions >= 2) {
        *status = PIME_FULL;
        return nullptr;
    }

    auto module = std::make_shared<Module>();
    module->engine = engine;
    module->key = engine + "|" + platform::current_platform() + "|" +
                  std::to_string(PIME_ABI_MAJOR);
    const std::string path = platform::path_join(
        backend_dir_, "pime_ml_" + base + platform::module_extension());
    std::string error;
    pime_status st = module->library.open(path, &error);
    if (st != PIME_OK) {
        *status = PIME_NOT_FOUND;
        return nullptr;
    }
    auto query = reinterpret_cast<pime_status (*)(uint32_t, pime_ml_backend*)>(
        module->library.symbol("pime_ml_backend_query"));
    if (!query) {
        *status = PIME_CORRUPT;
        return nullptr;
    }
    module->backend.size = sizeof(pime_ml_backend);
    module->backend.version = PIME_ABI_MAJOR;
    st = query(PIME_ABI_MAJOR, &module->backend);
    if (st != PIME_OK || !module->backend.load || !module->backend.run) {
        *status = PIME_UNSUPPORTED;
        return nullptr;
    }
    modules_[engine] = module;
    *status = PIME_OK;
    return module;
}

pime_status MlHost::load(const PluginCapContext& ctx, const std::string& engine,
                         const codec::Value& desc, uint64_t* session) {
    if (!session) return PIME_INVALID;
    pime_status st = PIME_OK;
    auto module = module_for(engine, &st);
    if (!module) return st;
    std::vector<uint8_t> bytes;
    codec::encode(desc, &bytes);
    uint64_t backend_session = 0;
    st = module->backend.load(bytes.data(), static_cast<uint32_t>(bytes.size()),
                              &backend_session);
    if (st != PIME_OK) return st;
    auto s = std::make_shared<Session>();
    s->module = module;
    s->backend_session = backend_session;
    s->alias = ctx.alias;
    s->generation = ctx.generation;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t id = next_session_++;
    sessions_[id] = s;
    *session = id;
    return PIME_OK;
}

pime_status MlHost::run(uint64_t session, const std::vector<uint8_t>& input,
                        std::vector<uint8_t>* output) {
    if (!output) return PIME_INVALID;
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session);
        if (it == sessions_.end()) return PIME_STALE;
        s = it->second;
    }
    std::vector<uint8_t> buffer(4u * 1024u * 1024u);
    uint32_t out_len = 0;
    pime_status st = s->module->backend.run(
        s->backend_session, input.data(), static_cast<uint32_t>(input.size()),
        buffer.data(), static_cast<uint32_t>(buffer.size()), &out_len);
    if (st == PIME_BUFFER_TOO_SMALL) {
        buffer.resize(out_len);
        st = s->module->backend.run(
            s->backend_session, input.data(), static_cast<uint32_t>(input.size()),
            buffer.data(), static_cast<uint32_t>(buffer.size()), &out_len);
    }
    if (st != PIME_OK) return st;
    output->assign(buffer.begin(), buffer.begin() + out_len);
    return PIME_OK;
}

pime_status MlHost::run_async(uint64_t session, std::vector<uint8_t> input,
                              uint64_t* job_id) {
    if (!job_id) return PIME_INVALID;
    auto job = std::make_shared<Job>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session);
        if (it == sessions_.end()) return PIME_STALE;
        if (queue_.size() >= 32) return PIME_QUEUE_FULL;
        uint32_t active = 0;
        for (const auto& j : jobs_) {
            if (j.second->session->alias == it->second->alias) ++active;
        }
        if (active >= 4) return PIME_FULL;
        job->id = next_job_++;
        job->session = it->second;
        job->input = std::move(input);
        jobs_[job->id] = job;
        queue_.push_back(job);
        *job_id = job->id;
    }
    cv_.notify_one();
    return PIME_OK;
}

pime_status MlHost::cancel(uint64_t job) {
    std::shared_ptr<Job> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(job);
        if (it == jobs_.end()) return PIME_STALE;
        it->second->cancelled = true;
        target = it->second;
    }
    if (target && target->session && target->session->module->backend.cancel) {
        target->session->module->backend.cancel(job);
    }
    return PIME_OK;
}

pime_status MlHost::free_session(uint64_t session) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session);
        if (it == sessions_.end()) return PIME_STALE;
        s = it->second;
        sessions_.erase(it);
    }
    if (s->module->backend.free) s->module->backend.free(s->backend_session);
    return PIME_OK;
}

void MlHost::cancel_generation(const std::string& alias, uint64_t generation) {
    std::vector<std::shared_ptr<Session>> dead;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second->alias == alias && it->second->generation == generation) {
                dead.push_back(it->second);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& kv : jobs_) {
            if (kv.second->session->alias == alias &&
                kv.second->session->generation == generation) {
                kv.second->cancelled = true;
            }
        }
    }
    for (auto& s : dead)
        if (s->module->backend.free) s->module->backend.free(s->backend_session);
}

void MlHost::worker_loop() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            job = queue_.front();
            queue_.pop_front();
            if (job->cancelled) {
                jobs_.erase(job->id);
                continue;
            }
        }
        std::vector<uint8_t> output;
        pime_status st = PIME_OK;
        {
            std::shared_ptr<Session> session = job->session;
            std::vector<uint8_t> buffer(4u * 1024u * 1024u);
            uint32_t out_len = 0;
            st = session->module->backend.run(
                session->backend_session, job->input.data(),
                static_cast<uint32_t>(job->input.size()), buffer.data(),
                static_cast<uint32_t>(buffer.size()), &out_len);
            if (st == PIME_BUFFER_TOO_SMALL) {
                buffer.resize(out_len);
                st = session->module->backend.run(
                    session->backend_session, job->input.data(),
                    static_cast<uint32_t>(job->input.size()), buffer.data(),
                    static_cast<uint32_t>(buffer.size()), &out_len);
            }
            if (st == PIME_OK) output.assign(buffer.begin(), buffer.begin() + out_len);
        }
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(job->id);
            if (it == jobs_.end() || it->second->cancelled) cancelled = true;
            else jobs_.erase(it);
        }
        if (!cancelled) finish_job(*job, st, output);
    }
}

void MlHost::finish_job(const Job& job, pime_status status,
                        const std::vector<uint8_t>& output) {
    if (!sink_ || !job.session) return;
    MlEvent event;
    event.alias = job.session->alias;
    event.generation = job.session->generation;
    codec::Value payload = codec::Value::map_v();
    payload.set("job", codec::Value::uint_v(job.id));
    if (status == PIME_OK) {
        event.type = "ml.result";
        const char* data = output.empty() ? "" : reinterpret_cast<const char*>(output.data());
        payload.set("data", codec::Value::bytes(std::string_view(data, output.size())));
    } else {
        event.type = "ml.error";
        payload.set("code", codec::Value::uint_v(status));
    }
    event.payload = std::move(payload);
    sink_(event);
}

void MlHost::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_)
        if (worker.joinable()) worker.join();
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : sessions_) sessions.push_back(kv.second);
        sessions_.clear();
        jobs_.clear();
        queue_.clear();
    }
    for (auto& s : sessions)
        if (s->module->backend.free) s->module->backend.free(s->backend_session);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        modules_.clear();
    }
}

}  // namespace pime::cap
