#include "core.hpp"

#include <pime/core.h>
#include <pime/version.h>

#include <algorithm>
#include <cstring>
#include <thread>

#include "capabilities/time/time.hpp"
#include "platform/platform.hpp"

namespace pime::core {

namespace {

using codec::Value;

Value make_map() { return Value::map_v(); }
Value make_array() { return Value::array_v(); }

Value text_value(const std::string& text) { return Value::text_v(text); }

bool is_string(const Value* value) {
    return value && (value->is_text() || value->is_bytes());
}

std::string string_field(const Value& map, const char* key,
                         const std::string& fallback = std::string()) {
    const Value* value = map.find(key);
    return is_string(value) ? std::string(value->as_text()) : fallback;
}

uint64_t uint_field(const Value& map, const char* key, uint64_t fallback = 0) {
    const Value* value = map.find(key);
    return value && value->is_number() ? value->as_uint() : fallback;
}

int64_t int_field(const Value& map, const char* key, int64_t fallback = 0) {
    const Value* value = map.find(key);
    return value && value->is_number() ? value->as_int() : fallback;
}

bool bool_field(const Value& map, const char* key, bool fallback = false) {
    const Value* value = map.find(key);
    return value ? value->as_bool() : fallback;
}

std::vector<uint8_t> bytes_field(const Value& map, const char* key) {
    const Value* value = map.find(key);
    if (!is_string(value)) return {};
    std::string_view view = value->as_text();
    return std::vector<uint8_t>(view.begin(), view.end());
}

uint32_t level_from_value(const Value& request) {
    uint64_t level = uint_field(request, "level", PIME_LOG_INFO);
    if (level > PIME_LOG_TRACE) level = PIME_LOG_TRACE;
    return static_cast<uint32_t>(level);
}

Value without_key(const Value& map, std::string_view key) {
    if (!map.is_map()) return map;
    Value copy = Value::map_v();
    for (const auto& entry : map.map) {
        if (entry.first.is_text() && entry.first.as_text() == key) continue;
        copy.map.push_back(entry);
    }
    return copy;
}

}  // namespace

Core::Core(const CoreOptions& options)
    : options_(options),
      permission_((options.flags & PIME_FLAG_ALLOW_INSECURE) != 0),
      buffers_(options.memory_budget_mb * 1024ull * 1024ull) {
    sink_ = options_.sink;
    sink_ud_ = options_.sink_ud;
    log_.set_level(static_cast<pime_log_level>(options_.log_level));
}

Core::~Core() {
    timers_.stop();
    if (net_) net_->shutdown();
    if (ml_) ml_->shutdown();

    std::vector<std::shared_ptr<phl::PluginThread>> threads;
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        for (auto& entry : threads_) threads.push_back(entry.second);
        threads_.clear();
    }
    for (auto& thread : threads) {
        thread->close();
        thread->join(2000);
    }

    generations_.clear();
    sessions_.clear();
    contexts_.clear();
    channels_.clear();
    mutexes_.clear();
    condvars_.clear();
    shared_buffers_.clear();
    installed_.clear();
    buffers_.invalidate_all();
    phl::SyncRegistry::clear();
    if (store_) store_->flush_all();
}

pime_status Core::initialize() {
    if (!options_.data_dir.empty()) {
        pime_status status = platform::create_directories(options_.data_dir);
        if (status != PIME_OK) return status;
        store_ = std::make_unique<cap::Store>(
            platform::path_join(options_.data_dir, "store"), options_.store_key);
    } else {
        store_ = std::make_unique<cap::Store>(std::string(), options_.store_key);
    }

    host::PackageConfig package_config;
    package_config.plugin_dir = options_.plugin_dir;
    package_config.trust_store = options_.trust_store;
    package_config.strict_signature =
        (options_.flags & PIME_FLAG_STRICT_SIGNATURE) != 0;
    package_config.allow_insecure =
        (options_.flags & PIME_FLAG_ALLOW_INSECURE) != 0;
    package_config.core_version.major = PIME_VERSION_MAJOR;
    package_config.core_version.minor = PIME_VERSION_MINOR;
    package_config.core_version.patch = PIME_VERSION_PATCH;
    package_config.core_version.valid = true;
    packages_ = std::make_unique<host::PackageManager>(package_config);

    cap::NetConfig net_config;
    net_config.proxy = options_.proxy;
    net_config.ca_file = options_.ca_file;
    net_config.client_cert = options_.client_cert;
    net_config.client_key = options_.client_key;
    net_config.allow_insecure = package_config.allow_insecure;
    net_ = std::make_unique<cap::NetService>(
        net_config, [this](const cap::NetEvent& event) {
            enqueue_async(event.alias, event.generation, event.payload);
        });

    ml_ = std::make_unique<cap::MlHost>(
        options_.backend_dir, [this](const cap::MlEvent& event) {
            enqueue_async(event.alias, event.generation, event.payload);
        });

    timers_.start([this](const cap::TimerFire& fire) {
        Value payload = make_map();
        payload.set("type", text_value("timer"));
        payload.set("id", Value::uint_v(fire.id));
        payload.set("event_id", text_value(fire.event_id));
        enqueue_async(fire.alias, fire.generation, std::move(payload));
    });

    log_.write(PIME_LOG_INFO, "core", "init", "core initialized");
    return PIME_OK;
}

void Core::set_log_level(pime_log_level level) { log_.set_level(level); }

void Core::set_plugin_log_level(const std::string& plugin_id,
                                pime_log_level level) {
    log_.set_plugin_level(plugin_id, level);
}

Session* Core::create_session(const pime_session_config& config,
                              pime_status* status) {
    uint64_t id = config.id;
    if (id == 0) id = next_session_++;
    while (sessions_.count(id)) {
        if (config.id != 0) {
            if (status) *status = PIME_INVALID;
            return nullptr;
        }
        id = next_session_++;
    }
    if (id == 0 || id == UINT64_MAX) {
        if (status) *status = PIME_RESOURCE;
        return nullptr;
    }
    auto session = std::make_unique<Session>();
    session->id = id;
    session->flags = config.flags;
    Session* raw = session.get();
    sessions_[id] = std::move(session);
    contexts_[id] = std::make_unique<phl::ContextCache>();
    if (status) *status = PIME_OK;
    return raw;
}

void Core::destroy_session(Session* session) {
    if (!session) return;
    auto it = sessions_.find(session->id);
    if (it == sessions_.end() || it->second.get() != session) return;
    audio_.stop_capture(session->id);
    audio_.stop_playback(session->id);
    buffers_.release_session(session->id);
    contexts_.erase(session->id);
    for (auto entry = last_session_by_alias_.begin();
         entry != last_session_by_alias_.end();) {
        if (entry->second == session->id)
            entry = last_session_by_alias_.erase(entry);
        else
            ++entry;
    }
    sessions_.erase(it);
}

Session* Core::find_session(uint64_t id) {
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

Session* Core::session_for_alias(const std::string& alias) {
    auto it = last_session_by_alias_.find(alias);
    if (it == last_session_by_alias_.end()) return nullptr;
    return find_session(it->second);
}

Generation* Core::active_gen() {
    auto it = generations_.find(active_generation_);
    return it == generations_.end() ? nullptr : it->second.get();
}

ChainItem* Core::chain_item(Generation* generation, const std::string& alias) {
    if (!generation) return nullptr;
    for (auto& item : generation->chain) {
        if (item.alias == alias) return &item;
    }
    return nullptr;
}

phl::ContextCache* Core::ctx_cache(uint64_t session_id) {
    auto it = contexts_.find(session_id);
    if (it != contexts_.end()) return it->second.get();
    auto cache = std::make_unique<phl::ContextCache>();
    phl::ContextCache* raw = cache.get();
    contexts_[session_id] = std::move(cache);
    return raw;
}

pime_status Core::run_event(Session* session, const codec::Value& event,
                            bool* handled, std::vector<Value>* commands) {
    if (handled) *handled = false;
    if (commands) commands->clear();
    Generation* generation = active_gen();
    if (!generation) return PIME_NOT_FOUND;

    cap::Workspace workspace(generation->workspace_bytes);
    workspace.reset();
    cap::Workspace* previous_workspace = active_workspace_;
    active_workspace_ = &workspace;

    pime_status result = PIME_OK;
    for (auto& item : generation->chain) {
        if (!item.enabled || !item.instance || !item.instance->alive()) continue;

        auto schema = session->state_schema.find(item.alias);
        if (schema != session->state_schema.end() &&
            schema->second != item.caps.state_schema) {
            session->state.erase(item.alias);
            session->state_schema.erase(item.alias);
        }

        Value frame = make_map();
        Value plugin_event =
            (event.is_map() && event.has("type") &&
             string_field(event, "type") == "context" &&
             !permission_.app_allowed(item.alias))
                ? without_key(event, "app_id")
                : event;
        frame.set("event", plugin_event);
        auto state = session->state.find(item.alias);
        frame.set("state", state != session->state.end() ? state->second : make_map());
        frame.set("config", generation->config.for_plugin(item.alias));
        frame.set("workspace", workspace.snapshot());
        frame.set("secure", Value::boolean_v(session->secure));
        frame.set("session", Value::uint_v(session->id));
        frame.set("host_caps", Value::uint_v(session->host_caps));
        frame.set("generation", Value::uint_v(generation->id));
        frame.set("plugin", text_value(item.alias));

        std::vector<uint8_t> input;
        codec::encode(frame, &input);
        std::vector<uint8_t> output;
        item.instance->set_session(session->id, session->secure,
                                   session->host_caps);
        pime_status status = item.instance->process(
            input.data(), static_cast<uint32_t>(input.size()), &output);
        if (status != PIME_OK) {
            result = status;
            break;
        }
        Value decoded;
        status = codec::decode(output.data(), output.size(), &decoded);
        if (status != PIME_OK || !decoded.is_map()) {
            result = PIME_CORRUPT;
            break;
        }
        status = ingest_output(session, item, decoded, &workspace, handled,
                               commands);
        if (status != PIME_OK) {
            result = status;
            break;
        }
        workspace.commit();
        last_session_by_alias_[item.alias] = session->id;
    }

    active_workspace_ = previous_workspace;
    if (result != PIME_OK) return result;

    if (commands && !commands->empty()) {
        result = apply_commands(session, *commands);
    }
    return result;
}

pime_status Core::ingest_output(Session* session, const ChainItem& item,
                                const codec::Value& output,
                                cap::Workspace* workspace, bool* handled,
                                std::vector<Value>* commands) {
    if (const Value* state = output.find("state")) {
        std::vector<uint8_t> encoded;
        codec::encode(*state, &encoded);
        const uint64_t limit =
            static_cast<uint64_t>(item.caps.limits.state_kb) * 1024ull;
        if (encoded.size() > limit) return PIME_FULL;
        session->state[item.alias] = *state;
        session->state_schema[item.alias] = item.caps.state_schema;
    }
    if (workspace) {
        if (const Value* writes = output.find("workspace")) {
            if (writes->is_map()) {
                for (const auto& entry : writes->map) {
                    if (!entry.first.is_text()) continue;
                    pime_status status = workspace->set(
                        item.alias, std::string(entry.first.as_text()),
                        entry.second);
                    if (status != PIME_OK) return status;
                }
            }
        }
    }
    if (handled) {
        if (const Value* value = output.find("handled"))
            if (value->as_bool()) *handled = true;
    }
    if (commands) {
        if (const Value* value = output.find("commands")) {
            if (value->is_array()) {
                for (const auto& command : value->array) commands->push_back(command);
            }
        }
    }
    return PIME_OK;
}

pime_status Core::apply_commands(Session* session,
                                 const std::vector<Value>& commands) {
    for (const auto& command : commands) {
        pime_status status = apply_command(session, command);
        if (status != PIME_OK) return status;
    }
    return PIME_OK;
}

pime_status Core::apply_command(Session* session, const codec::Value& command) {
    if (!command.is_map()) return PIME_CORRUPT;
    const std::string type = string_field(command, "type");
    if (type.empty()) return PIME_CORRUPT;

    if (type == "set_composition") {
        if (!sink_.set_composition) return PIME_UNSUPPORTED;
        std::string text = string_field(command, "text");
        uint32_t cursor = static_cast<uint32_t>(uint_field(command, "cursor"));
        std::vector<pime_segment> segments;
        if (const Value* list = command.find("segments")) {
            if (list->is_array()) {
                segments.reserve(list->array.size());
                for (const auto& item : list->array) {
                    if (!item.is_map()) return PIME_CORRUPT;
                    pime_segment segment{};
                    segment.start =
                        static_cast<uint32_t>(uint_field(item, "start"));
                    segment.end =
                        static_cast<uint32_t>(uint_field(item, "end"));
                    segment.type =
                        static_cast<uint32_t>(uint_field(item, "type"));
                    segments.push_back(segment);
                }
            }
        }
        if (cursor > text.size()) return PIME_INVALID;
        pime_composition composition{};
        composition.size = sizeof(composition);
        composition.version = PIME_ABI_MAJOR;
        composition.text = text.data();
        composition.text_len = static_cast<uint32_t>(text.size());
        composition.cursor = cursor;
        composition.segments = segments.empty() ? nullptr : segments.data();
        composition.segment_count = static_cast<uint32_t>(segments.size());
        return sink_.set_composition(sink_ud_, &composition);
    }
    if (type == "commit") {
        if (!sink_.commit) return PIME_UNSUPPORTED;
        std::string text = string_field(command, "text");
        pime_text value{};
        value.size = sizeof(value);
        value.version = PIME_ABI_MAJOR;
        value.text = text.data();
        value.text_len = static_cast<uint32_t>(text.size());
        return sink_.commit(sink_ud_, &value);
    }
    if (type == "show_candidates") {
        if (!sink_.show_candidates) return PIME_UNSUPPORTED;
        std::vector<std::string> texts;
        std::vector<std::string> comments;
        std::vector<std::string> labels;
        std::vector<pime_candidate> items;
        if (const Value* list = command.find("items")) {
            if (list->is_array()) {
                if (list->array.size() > 10000) return PIME_INVALID;
                texts.reserve(list->array.size());
                comments.reserve(list->array.size());
                labels.reserve(list->array.size());
                items.reserve(list->array.size());
                for (const auto& entry : list->array) {
                    if (!entry.is_map()) return PIME_CORRUPT;
                    texts.push_back(string_field(entry, "text"));
                    comments.push_back(string_field(entry, "comment"));
                    labels.push_back(string_field(entry, "label"));
                    pime_candidate candidate{};
                    candidate.text = texts.back().data();
                    candidate.text_len = static_cast<uint32_t>(texts.back().size());
                    candidate.comment = comments.back().data();
                    candidate.comment_len =
                        static_cast<uint32_t>(comments.back().size());
                    candidate.label = labels.back().data();
                    candidate.label_len = static_cast<uint32_t>(labels.back().size());
                    items.push_back(candidate);
                }
            }
        }
        pime_candidate_list list{};
        list.size = sizeof(list);
        list.version = PIME_ABI_MAJOR;
        list.items = items.empty() ? nullptr : items.data();
        list.count = static_cast<uint32_t>(items.size());
        return sink_.show_candidates(sink_ud_, &list);
    }
    if (type == "hide_candidates") {
        if (!sink_.hide_candidates) return PIME_UNSUPPORTED;
        return sink_.hide_candidates(sink_ud_);
    }
    if (type == "audio_out") {
        if (!sink_.audio_out) return PIME_UNSUPPORTED;
        std::vector<uint8_t> data = bytes_field(command, "data");
        pime_audio_frame frame{};
        frame.size = sizeof(frame);
        frame.version = PIME_ABI_MAJOR;
        frame.rate = static_cast<uint32_t>(uint_field(command, "rate", 16000));
        frame.channels =
            static_cast<uint32_t>(uint_field(command, "channels", 1));
        frame.format = static_cast<uint32_t>(
            uint_field(command, "format", PIME_AUDIO_S16LE));
        frame.data = data.empty() ? nullptr : data.data();
        frame.data_len = static_cast<uint32_t>(data.size());
        frame.time_ms = uint_field(command, "time_ms");
        return sink_.audio_out(sink_ud_, &frame);
    }
    if (type == "beep") {
        if (!sink_.beep) return PIME_UNSUPPORTED;
        return sink_.beep(sink_ud_);
    }
    if (type == "clear_ink") {
        if (!sink_.clear_ink) return PIME_UNSUPPORTED;
        return sink_.clear_ink(sink_ud_);
    }
    if (type == "notify") {
        if (!sink_.notify) return PIME_UNSUPPORTED;
        return sink_.notify(sink_ud_, static_cast<pime_status>(
                                          uint_field(command, "code")));
    }
    if (type == "log") {
        std::string tag = string_field(command, "tag");
        std::string message = string_field(command, "message");
        if (session && session->secure) message.clear();
        log_.write(static_cast<pime_log_level>(level_from_value(command)),
                   "plugin", tag.c_str(), message.c_str());
        return PIME_OK;
    }
    if (type == "metric") {
        std::string name = string_field(command, "name");
        int64_t value = int_field(command, "value");
        const std::string alias = session ? session->app_id : std::string();
        if (bool_field(command, "add"))
            metrics_.add(alias, name, value);
        else
            metrics_.set(alias, name, value);
        return PIME_OK;
    }
    return PIME_INVALID;
}

pime_status Core::deliver_async(const AsyncEvent& event) {
    if (event.generation == 0 || event.generation != active_generation_)
        return PIME_STALE;
    Generation* generation = active_gen();
    ChainItem* item = chain_item(generation, event.alias);
    if (!item || !item->enabled || !item->instance || !item->instance->alive())
        return PIME_NOT_FOUND;
    Session* session = session_for_alias(event.alias);
    if (!session) return PIME_NOT_FOUND;

    auto schema = session->state_schema.find(item->alias);
    if (schema != session->state_schema.end() &&
        schema->second != item->caps.state_schema) {
        session->state.erase(item->alias);
        session->state_schema.erase(item->alias);
    }

    Value frame = make_map();
    frame.set("event", event.payload);
    auto state = session->state.find(item->alias);
    frame.set("state", state != session->state.end() ? state->second : make_map());
    frame.set("config", generation->config.for_plugin(item->alias));
    frame.set("workspace", make_map());
    frame.set("secure", Value::boolean_v(session->secure));
    frame.set("session", Value::uint_v(session->id));
    frame.set("host_caps", Value::uint_v(session->host_caps));
    frame.set("generation", Value::uint_v(generation->id));
    frame.set("plugin", text_value(item->alias));

    std::vector<uint8_t> input;
    codec::encode(frame, &input);
    std::vector<uint8_t> output;
    item->instance->set_session(session->id, session->secure,
                                session->host_caps);
    pime_status status = item->instance->on_event(
        input.data(), static_cast<uint32_t>(input.size()), &output);
    if (status != PIME_OK) {
        log_.write(PIME_LOG_WARN, "core", "async", "plugin event failed");
        return status;
    }
    Value decoded;
    status = codec::decode(output.data(), output.size(), &decoded);
    if (status != PIME_OK || !decoded.is_map()) return PIME_CORRUPT;

    bool handled = false;
    std::vector<Value> commands;
    status = ingest_output(session, *item, decoded, nullptr, &handled, &commands);
    if (status != PIME_OK) return status;
    return apply_commands(session, commands);
}

void Core::enqueue_async(const std::string& alias, uint64_t generation,
                         codec::Value payload) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (async_queue_.size() >= 256) async_queue_.pop_front();
        async_queue_.push_back(AsyncEvent{alias, generation, std::move(payload)});
    }
    wake();
}

void Core::wake() {
    if (!wake_pending_.exchange(true)) {
        if (sink_.wake) sink_.wake(sink_ud_);
    }
}

pime_status Core::emit(uint64_t session_id, const std::string& alias,
                       uint64_t generation, const uint8_t* data, uint32_t len) {
    if (len && !data) return PIME_INVALID;
    if (!find_session(session_id)) return PIME_STALE;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (emit_queue_.size() >= 256) return PIME_QUEUE_FULL;
        EmitEnvelope envelope;
        envelope.session_id = session_id;
        envelope.alias = alias;
        envelope.generation = generation;
        envelope.data.assign(data, data + len);
        emit_queue_.push_back(std::move(envelope));
    }
    wake();
    return PIME_OK;
}

void Core::drain() {
    log_.pump(&sink_, sink_ud_);

    for (int i = 0; i < 16; ++i) {
        uint64_t session_id = 0;
        cap::AudioFrameData frame;
        if (!audio_.pop_playback(&session_id, &frame)) break;
        if (!sink_.audio_out) continue;
        pime_audio_frame out{};
        out.size = sizeof(out);
        out.version = PIME_ABI_MAJOR;
        out.rate = frame.format.rate;
        out.channels = frame.format.channels;
        out.format = frame.format.format;
        out.data = frame.data.empty() ? nullptr : frame.data.data();
        out.data_len = static_cast<uint32_t>(frame.data.size());
        out.time_ms = frame.time_ms;
        sink_.audio_out(sink_ud_, &out);
        (void)session_id;
    }

    for (int i = 0; i < 64; ++i) {
        EmitEnvelope envelope;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (emit_queue_.empty()) break;
            envelope = std::move(emit_queue_.front());
            emit_queue_.pop_front();
        }
        Session* session = find_session(envelope.session_id);
        if (!session) continue;
        Value decoded;
        if (codec::decode(envelope.data.data(), envelope.data.size(), &decoded) != PIME_OK ||
            !decoded.is_map()) {
            if (sink_.notify) sink_.notify(sink_ud_, PIME_CORRUPT);
            continue;
        }
        Generation* generation = active_gen();
        ChainItem* item = chain_item(generation, envelope.alias);
        if (!item) continue;
        bool handled = false;
        std::vector<Value> commands;
        pime_status status =
            ingest_output(session, *item, decoded, nullptr, &handled, &commands);
        if (status == PIME_OK) status = apply_commands(session, commands);
        if (status != PIME_OK && sink_.notify)
            sink_.notify(sink_ud_, status);
    }

    for (int i = 0; i < 64; ++i) {
        AsyncEvent event;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (async_queue_.empty()) break;
            event = std::move(async_queue_.front());
            async_queue_.pop_front();
        }
        pime_status status = deliver_async(event);
        if (status != PIME_OK && status != PIME_STALE &&
            status != PIME_NOT_FOUND && sink_.notify) {
            sink_.notify(sink_ud_, status);
        }
    }

    wake_pending_.store(false);
}

void Core::pump() { drain(); }

/* ------------------------------ input ---------------------------------- */

pime_status Core::key(Session* session, const pime_key_event& event,
                      bool* handled) {
    if (handled) *handled = false;
    if (!session) return PIME_INVALID;
    if (session->secure) return PIME_OK;

    Value value = make_map();
    value.set("type", text_value("key"));
    value.set("keycode", Value::uint_v(event.keycode));
    value.set("modifiers", Value::uint_v(event.modifiers));
    value.set("action", Value::uint_v(event.action));
    value.set("time_ms", Value::uint_v(event.time_ms));
    if (event.text && event.text_len)
        value.set("text", Value::text_v(
                              std::string_view(event.text, event.text_len)));

    std::vector<Value> commands;
    return run_event(session, value, handled, &commands);
}

pime_status Core::touch(Session* session, const pime_touch_event& event,
                        bool* handled) {
    if (handled) *handled = false;
    if (!session) return PIME_INVALID;
    if (session->secure) return PIME_OK;

    Value points = make_array();
    for (uint32_t i = 0; event.points && i < event.point_count; ++i) {
        Value point = make_map();
        point.set("x", Value::uint_v(event.points[i].x));
        point.set("y", Value::uint_v(event.points[i].y));
        point.set("pressure", Value::uint_v(event.points[i].pressure));
        point.set("t_ms", Value::uint_v(event.points[i].t_ms));
        points.push(std::move(point));
    }
    Value value = make_map();
    value.set("type", text_value("touch"));
    value.set("phase", Value::uint_v(event.phase));
    value.set("points", std::move(points));
    value.set("time_ms", Value::uint_v(event.time_ms));
    std::vector<Value> commands;
    return run_event(session, value, handled, &commands);
}

pime_status Core::text(Session* session, const pime_text& text) {
    if (!session) return PIME_INVALID;
    if (text.text_len && !text.text) return PIME_INVALID;
    if (session->secure) return PIME_OK;
    Value value = make_map();
    value.set("type", text_value("text"));
    value.set("text",
              Value::text_v(text.text
                                ? std::string_view(text.text, text.text_len)
                                : std::string_view()));
    std::vector<Value> commands;
    return run_event(session, value, nullptr, &commands);
}

pime_status Core::candidate(Session* session,
                            const pime_candidate_event& event) {
    if (!session) return PIME_INVALID;
    Value value = make_map();
    value.set("type", text_value("candidate"));
    value.set("index", Value::uint_v(event.index));
    std::vector<Value> commands;
    return run_event(session, value, nullptr, &commands);
}

pime_status Core::surrounding(Session* session, const pime_text& before,
                              const pime_text& after) {
    if (!session) return PIME_INVALID;
    if (session->secure) return PIME_OK;
    Value value = make_map();
    value.set("type", text_value("surrounding"));
    value.set("before",
              Value::text_v(before.text
                                ? std::string_view(before.text, before.text_len)
                                : std::string_view()));
    value.set("after",
              Value::text_v(after.text
                                ? std::string_view(after.text, after.text_len)
                                : std::string_view()));
    std::vector<Value> commands;
    return run_event(session, value, nullptr, &commands);
}

pime_status Core::context(Session* session, const pime_context& context) {
    if (!session) return PIME_INVALID;
    session->field_type = context.field_type;
    session->input_type = context.input_type;
    session->page_size = context.page_size;
    session->host_caps = context.host_caps;
    if (context.locale && context.locale_len)
        session->locale.assign(context.locale, context.locale_len);
    if (session->secure) {
        session->app_id.clear();
    } else if (context.app_id && context.app_id_len) {
        session->app_id.assign(context.app_id, context.app_id_len);
    }

    Value value = make_map();
    value.set("type", text_value("context"));
    value.set("field_type", Value::uint_v(context.field_type));
    value.set("input_type", Value::uint_v(context.input_type));
    value.set("page_size", Value::uint_v(context.page_size));
    value.set("host_caps", Value::uint_v(context.host_caps));
    value.set("locale", text_value(session->locale));
    if (!session->secure && !session->app_id.empty())
        value.set("app_id", text_value(session->app_id));

    session->context = value;
    std::vector<Value> commands;
    return run_event(session, value, nullptr, &commands);
}

pime_status Core::focus(Session* session, bool focused) {
    if (!session) return PIME_INVALID;
    session->focused = focused;
    Value value = make_map();
    value.set("type", text_value("focus"));
    value.set("focused", Value::boolean_v(focused));
    std::vector<Value> commands;
    return run_event(session, value, nullptr, &commands);
}

pime_status Core::secure(Session* session, bool secure) {
    if (!session) return PIME_INVALID;
    if (session->secure == secure) return PIME_OK;
    session->secure = secure;
    if (session->secure) {
        audio_.stop_capture(session->id);
        audio_.stop_playback(session->id);
        buffers_.release_session(session->id);
        session->app_id.clear();
    }
    Value value = make_map();
    value.set("type", text_value("secure"));
    value.set("secure", Value::boolean_v(secure));
    std::vector<Value> commands;
    return run_event(session, value, nullptr, &commands);
}

pime_status Core::reset(Session* session) {
    if (!session) return PIME_INVALID;
    Value value = make_map();
    value.set("type", text_value("reset"));
    std::vector<Value> commands;
    return run_event(session, value, nullptr, &commands);
}

pime_status Core::audio_in(Session* session, const pime_audio_frame& frame) {
    if (!session || !frame.data) return PIME_INVALID;
    if (session->secure) return PIME_DENIED;
    if (frame.format != PIME_AUDIO_S16LE && frame.format != PIME_AUDIO_F32LE)
        return PIME_INVALID;
    cap::PcmFormat format;
    format.rate = frame.rate;
    format.channels = frame.channels;
    format.format = frame.format;
    if (!cap::pcm_format_valid(format)) return PIME_INVALID;
    cap::AudioFrameData data;
    data.format = format;
    data.time_ms = frame.time_ms;
    data.data.assign(static_cast<const uint8_t*>(frame.data),
                     static_cast<const uint8_t*>(frame.data) + frame.data_len);
    return audio_.push_capture(session->id, data);
}

pime_status Core::voice(Session* session, uint32_t action) {
    if (!session) return PIME_INVALID;
    if (action > PIME_VOICE_CANCEL) return PIME_INVALID;
    if (session->secure && action == PIME_VOICE_START) return PIME_DENIED;
    Value value = make_map();
    value.set("type", text_value("voice"));
    value.set("action", Value::uint_v(action));
    std::vector<Value> commands;
    return run_event(session, value, nullptr, &commands);
}

/* --------------------------- lifecycle --------------------------------- */

pime_status Core::pause() {
    if (paused_) return PIME_OK;
    paused_ = true;
    timers_.pause();
    if (net_) net_->pause();
    audio_.stop_all();
    return PIME_OK;
}

pime_status Core::resume() {
    if (!paused_) return PIME_OK;
    paused_ = false;
    timers_.resume();
    if (net_) net_->resume();
    return PIME_OK;
}

pime_status Core::memory_warning(uint32_t level) {
    if (level > PIME_MEMORY_CRITICAL) return PIME_INVALID;
    for (auto& entry : contexts_) entry.second->on_memory_pressure(level);
    if (level == PIME_MEMORY_CRITICAL)
        log_.write(PIME_LOG_WARN, "core", "memory", "critical memory pressure");
    return PIME_OK;
}

pime_status Core::network_changed(uint32_t state) {
    if (state > PIME_NET_UNMETERED) return PIME_INVALID;
    if (net_) net_->set_network_state(state);
    Generation* generation = active_gen();
    if (!generation) return PIME_OK;
    for (auto& item : generation->chain) {
        Value value = make_map();
        value.set("type", text_value("net.state"));
        value.set("state", Value::uint_v(state));
        enqueue_async(item.alias, generation->id, std::move(value));
    }
    return PIME_OK;
}

pime_status Core::audio_focus(uint32_t focus) {
    if (focus > PIME_FOCUS_DUCKED) return PIME_INVALID;
    Generation* generation = active_gen();
    if (!generation) return PIME_OK;
    for (auto& item : generation->chain) {
        Value value = make_map();
        value.set("type", text_value("audio.focus"));
        value.set("focus", Value::uint_v(focus));
        enqueue_async(item.alias, generation->id, std::move(value));
    }
    return PIME_OK;
}

pime_status Core::audio_route(uint32_t route) {
    if (route > PIME_ROUTE_BLUETOOTH) return PIME_INVALID;
    Generation* generation = active_gen();
    if (!generation) return PIME_OK;
    for (auto& item : generation->chain) {
        Value value = make_map();
        value.set("type", text_value("audio.route"));
        value.set("route", Value::uint_v(route));
        enqueue_async(item.alias, generation->id, std::move(value));
    }
    return PIME_OK;
}

pime_status Core::permission(const std::string& capability, bool granted) {
    return permission_.grant(capability, granted);
}

}  // namespace pime::core
