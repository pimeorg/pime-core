#include "net.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "platform/platform.hpp"
#include "util/string_util.hpp"

#if defined(PIME_HAVE_CURL)
#include <curl/curl.h>
#endif

#if defined(PIME_HAVE_LWS)
#include <libwebsockets.h>
#endif

namespace pime::cap {

#if !defined(PIME_HAVE_CURL) && !defined(PIME_HAVE_LWS)

struct NetService::Impl {};

NetService::NetService(NetConfig, EventSink) : impl_(std::make_unique<Impl>()) {}
NetService::~NetService() = default;
pime_status NetService::http(const PluginCapContext&, const codec::Value&, uint64_t*) {
    return PIME_UNSUPPORTED;
}
pime_status NetService::take_result(uint64_t, codec::Value*) { return PIME_UNSUPPORTED; }
pime_status NetService::open(const PluginCapContext&, const std::string&,
                             const std::string&, const codec::Value&, uint64_t*) {
    return PIME_UNSUPPORTED;
}
pime_status NetService::send(uint64_t, const void*, uint32_t) { return PIME_UNSUPPORTED; }
pime_status NetService::close(uint64_t) { return PIME_UNSUPPORTED; }
void NetService::cancel_generation(const std::string&, uint64_t) {}
void NetService::pause() {}
void NetService::resume() {}
void NetService::set_network_state(uint32_t) {}
uint64_t NetService::open_streams() const { return 0; }
void NetService::shutdown() {}

#else

#if defined(PIME_HAVE_CURL)
namespace {

std::string header_value(const codec::Value& headers, const char* name) {
    const codec::Value* v = headers.find(name);
    if (!v) return {};
    return std::string(v->as_text());
}

size_t write_body(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* body = static_cast<std::string*>(user);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

struct HeaderCollector {
    std::string raw;
};

size_t write_header(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* collector = static_cast<HeaderCollector*>(user);
    collector->raw.append(ptr, size * nmemb);
    return size * nmemb;
}

codec::Value parse_headers(const std::string& raw) {
    codec::Value map = codec::Value::map_v();
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find("\r\n", start);
        if (end == std::string::npos) break;
        std::string_view line(raw.data() + start, end - start);
        size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string key(to_lower(trim_view(line.substr(0, colon))));
            std::string value(trim_view(line.substr(colon + 1)));
            const codec::Value* existing = map.find(key);
            if (existing && existing->is_text()) {
                map.set(key, codec::Value::text_v(std::string(existing->as_text()) + ", " + value));
            } else if (!key.empty()) {
                map.set(key, codec::Value::text_v(value));
            }
        }
        start = end + 2;
    }
    return map;
}

}  // namespace
#endif

struct NetService::Impl {
    struct HttpTask {
        uint64_t id = 0;
        std::string alias;
        uint64_t generation = 0;
        codec::Value request;
        std::string method;
        std::string url;
        std::string body;
        std::string headers;
        uint32_t timeout_ms = 30000;
    };

    struct ActiveHttp {
        uint64_t id = 0;
        std::string alias;
        uint64_t generation = 0;
        std::string body;
        HeaderCollector headers;
        std::string request_headers;
        curl_slist* header_list = nullptr;
    };

    struct Stream {
        uint64_t id = 0;
        std::string alias;
        uint64_t generation = 0;
        std::string proto;
        std::string url;
        codec::Value options;
        std::atomic<bool> stop{false};
        std::thread thread;
    };

    NetConfig config;
    EventSink sink;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<HttpTask> http_queue;
    std::unordered_map<uint64_t, codec::Value> results;
    std::unordered_map<uint64_t, std::shared_ptr<Stream>> streams;
    uint64_t next_id = 1;
    std::atomic<bool> stopping{false};
    std::atomic<bool> paused{false};
    std::atomic<uint32_t> network_state{PIME_NET_ONLINE};

#if defined(PIME_HAVE_CURL)
    CURLM* multi = nullptr;
    std::thread http_thread;
    std::unordered_map<CURL*, ActiveHttp> active;
#endif

#if defined(PIME_HAVE_LWS)
    lws_context* lws_ctx = nullptr;
    std::thread lws_thread;
    struct WsConn {
        uint64_t id = 0;
        std::string alias;
        uint64_t generation = 0;
        lws* wsi = nullptr;
        std::mutex mutex;
        std::deque<std::string> outbox;
        bool open = false;
        uint64_t heartbeat_ms = 15000;
        uint64_t next_reconnect_ms = 0;
        uint32_t backoff_ms = 1000;
        bool reconnect = true;
    };
    std::unordered_map<uint64_t, std::shared_ptr<WsConn>> ws_conns;
    std::unordered_map<uint64_t, std::pair<std::string, codec::Value>> pending_ws_urls;
    std::vector<std::shared_ptr<WsConn>> ws_graveyard;
    std::deque<uint64_t> ws_pending_open;
#endif

    pime_status validate_url(const std::string& url) const {
        if (url.rfind("https://", 0) == 0 || url.rfind("wss://", 0) == 0) return PIME_OK;
        if (url.rfind("http://", 0) == 0 || url.rfind("ws://", 0) == 0)
            return config.allow_insecure ? PIME_OK : PIME_DENIED;
        return PIME_INVALID;
    }

    void emit(const std::string& alias, uint64_t generation, codec::Value payload) {
        if (!sink) return;
        NetEvent event;
        event.alias = alias;
        event.generation = generation;
        event.payload = std::move(payload);
        sink(event);
    }

#if defined(PIME_HAVE_CURL)
    void http_loop();
    void start_http(const HttpTask& task);
    void finish_http(CURL* easy, CURLcode code);
    void stream_loop(const std::shared_ptr<Stream>& stream);
#endif
#if defined(PIME_HAVE_LWS)
    void lws_loop();
    void create_ws(const std::string& alias, uint64_t generation, const std::string& url,
                   const codec::Value& options, uint64_t id);
    static int ws_callback(lws* wsi, lws_callback_reasons reason, void* user,
                           void* in, size_t len);
#endif
};

#if defined(PIME_HAVE_CURL)

void NetService::Impl::start_http(const HttpTask& task) {
    CURL* easy = curl_easy_init();
    if (!easy) return;
    ActiveHttp active_task;
    active_task.id = task.id;
    active_task.alias = task.alias;
    active_task.generation = task.generation;
    active_task.request_headers = task.headers;

    curl_easy_setopt(easy, CURLOPT_URL, task.url.c_str());
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, task.method.c_str());
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, (long)task.timeout_ms);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    if (!config.proxy.empty()) curl_easy_setopt(easy, CURLOPT_PROXY, config.proxy.c_str());
    if (!config.ca_file.empty()) curl_easy_setopt(easy, CURLOPT_CAINFO, config.ca_file.c_str());
    if (!config.client_cert.empty())
        curl_easy_setopt(easy, CURLOPT_SSLCERT, config.client_cert.c_str());
    if (!config.client_key.empty())
        curl_easy_setopt(easy, CURLOPT_SSLKEY, config.client_key.c_str());
    if (!task.body.empty() || task.method == "POST" || task.method == "PUT") {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, task.body.data());
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)task.body.size());
    }
    if (!task.headers.empty()) {
        size_t start = 0;
        while (start < task.headers.size()) {
            size_t end = task.headers.find("\r\n", start);
            if (end == std::string::npos) end = task.headers.size();
            std::string line = task.headers.substr(start, end - start);
            if (!line.empty())
                active_task.header_list = curl_slist_append(active_task.header_list, line.c_str());
            start = end + 2;
        }
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, active_task.header_list);
    }
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &active_task.body);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &active_task.headers);

    std::lock_guard<std::mutex> lock(mutex);
    active[easy] = std::move(active_task);
    curl_multi_add_handle(multi, easy);
}

void NetService::Impl::finish_http(CURL* easy, CURLcode code) {
    ActiveHttp task;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = active.find(easy);
        if (it == active.end()) {
            curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);
            return;
        }
        task = std::move(it->second);
        active.erase(it);
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_multi_remove_handle(multi, easy);
    curl_easy_cleanup(easy);
    if (task.header_list) curl_slist_free_all(task.header_list);

    codec::Value payload = codec::Value::map_v();
    payload.set("type", codec::Value::text_v("net.http"));
    payload.set("request", codec::Value::uint_v(task.id));
    if (code == CURLE_OK) {
        payload.set("status", codec::Value::uint_v(static_cast<uint64_t>(status)));
        payload.set("headers", parse_headers(task.headers.raw));
        payload.set("body", codec::Value::bytes(task.body));
    } else {
        payload.set("error", codec::Value::uint_v(PIME_IO));
        payload.set("curl", codec::Value::uint_v(static_cast<uint64_t>(code)));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        results[task.id] = payload;
    }
    emit(task.alias, task.generation, std::move(payload));
}

void NetService::Impl::http_loop() {
    while (!stopping.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(20), [&] {
                return stopping.load() || (!paused.load() && !http_queue.empty());
            });
            if (stopping.load()) break;
            if (!paused.load()) {
                while (!http_queue.empty()) {
                    HttpTask task = std::move(http_queue.front());
                    http_queue.pop_front();
                    lock.unlock();
                    start_http(task);
                    lock.lock();
                }
            }
        }
        int running = 0;
        curl_multi_perform(multi, &running);
        CURLMsg* msg = nullptr;
        int remaining = 0;
        while ((msg = curl_multi_info_read(multi, &remaining))) {
            if (msg->msg == CURLMSG_DONE) {
                finish_http(msg->easy_handle, msg->data.result);
            }
        }
        curl_multi_poll(multi, nullptr, 0, 20, nullptr);
    }
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& kv : active) {
        curl_multi_remove_handle(multi, kv.first);
        curl_easy_cleanup(kv.first);
    }
    active.clear();
    curl_multi_cleanup(multi);
    multi = nullptr;
}

void NetService::Impl::stream_loop(const std::shared_ptr<Stream>& stream) {
    uint32_t backoff = 1000;
    while (!stream->stop.load() && !stopping.load()) {
        if (paused.load() || network_state.load() == PIME_NET_OFFLINE) {
            platform::sleep_ms(200);
            continue;
        }
        if (network_state.load() == PIME_NET_METERED) {
            platform::sleep_ms(500);
            continue;
        }
        CURL* easy = curl_easy_init();
        if (!easy) {
            platform::sleep_ms(backoff);
            continue;
        }
        struct State {
            std::shared_ptr<Stream> stream;
            Impl* impl;
            std::string pending;
            uint64_t last_data_ms;
            uint32_t idle_ms;
            bool first = true;
        };
        State state;
        state.stream = stream;
        state.impl = this;
        state.last_data_ms = platform::monotonic_ms();
        state.idle_ms = 60000;
        if (const codec::Value* idle = stream->options.find("idle_timeout_ms"))
            state.idle_ms = static_cast<uint32_t>(idle->as_uint());

        curl_easy_setopt(easy, CURLOPT_URL, stream->url.c_str());
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!config.proxy.empty()) curl_easy_setopt(easy, CURLOPT_PROXY, config.proxy.c_str());
        if (!config.ca_file.empty()) curl_easy_setopt(easy, CURLOPT_CAINFO, config.ca_file.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, nullptr);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
                         +[](char* ptr, size_t size, size_t nmemb, void* user) -> size_t {
                             auto* st = static_cast<State*>(user);
                             if (st->first) {
                                 st->first = false;
                                 codec::Value payload = codec::Value::map_v();
                                 payload.set("type", codec::Value::text_v("net.open"));
                                 payload.set("id", codec::Value::uint_v(st->stream->id));
                                 st->impl->emit(st->stream->alias, st->stream->generation,
                                                std::move(payload));
                             }
                             const size_t total = size * nmemb;
                             st->pending.append(ptr, total);
                             st->last_data_ms = platform::monotonic_ms();
                             size_t pos;
                             while ((pos = st->pending.find('\n')) != std::string::npos) {
                                 std::string line = st->pending.substr(0, pos);
                                 st->pending.erase(0, pos + 1);
                                 if (!line.empty() && line.back() == '\r') line.pop_back();
                                 if (line.rfind("data:", 0) != 0) continue;
                                 std::string data = line.substr(5);
                                 if (!data.empty() && data.front() == ' ') data.erase(0, 1);
                                 if (data.empty() || data == "[DONE]") continue;
                                 codec::Value payload = codec::Value::map_v();
                                 payload.set("type", codec::Value::text_v("net.data"));
                                 payload.set("id", codec::Value::uint_v(st->stream->id));
                                 payload.set("data", codec::Value::bytes(data));
                                 st->impl->emit(st->stream->alias, st->stream->generation,
                                                std::move(payload));
                             }
                             return total;
                         });
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION,
                         +[](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                             auto* st = static_cast<State*>(user);
                             if (st->stream->stop.load() || st->impl->stopping.load()) return 1;
                             if (platform::monotonic_ms() - st->last_data_ms > st->idle_ms)
                                 return 1;
                             return 0;
                         });
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &state);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        CURLcode code = curl_easy_perform(easy);
        curl_easy_cleanup(easy);
        if (stream->stop.load() || stopping.load()) break;

        codec::Value payload = codec::Value::map_v();
        payload.set("type", codec::Value::text_v("net.close"));
        payload.set("id", codec::Value::uint_v(stream->id));
        payload.set("error", codec::Value::uint_v(code == CURLE_OK ? 0 : PIME_IO));
        emit(stream->alias, stream->generation, std::move(payload));
        platform::sleep_ms(backoff);
        backoff = backoff < 30000 ? backoff * 2 : 30000;
    }
}

#endif  // PIME_HAVE_CURL

#if defined(PIME_HAVE_LWS)

void NetService::Impl::create_ws(const std::string& alias, uint64_t generation,
                                 const std::string& url, const codec::Value& options,
                                 uint64_t id) {
    auto conn = std::make_shared<WsConn>();
    conn->id = id;
    conn->alias = alias;
    conn->generation = generation;
    if (const codec::Value* hb = options.find("heartbeat_ms"))
        conn->heartbeat_ms = hb->as_uint();
    {
        std::lock_guard<std::mutex> lock(mutex);
        ws_conns[id] = conn;
    }

    lws_client_connect_info info{};
    info.context = lws_ctx;
    const bool secure = url.rfind("wss://", 0) == 0;
    const size_t scheme_len = secure ? 6 : 5;
    std::string rest = url.substr(scheme_len);
    size_t slash = rest.find('/');
    std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    info.address = host.c_str();
    info.port = secure ? 443 : 80;
    info.path = path.c_str();
    info.host = host.c_str();
    info.origin = host.c_str();
    info.protocol = "pime";
    info.ssl_connection = secure ? LCCSCF_USE_SSL : 0;
    info.opaque_user_data = conn.get();
    info.pwsi = &conn->wsi;
    lws_client_connect_via_info(&info);
}

int NetService::Impl::ws_callback(lws* wsi, lws_callback_reasons reason, void* user,
                                  void* in, size_t len) {
    auto* conn = static_cast<WsConn*>(lws_get_opaque_user_data(wsi));
    auto* impl = static_cast<Impl*>(lws_context_user(lws_get_context(wsi)));
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            if (!conn) break;
            {
                std::lock_guard<std::mutex> lock(conn->mutex);
                conn->open = true;
            }
            lws_set_timer_usecs(wsi, static_cast<int>(conn->heartbeat_ms * 1000));
            if (impl) {
                codec::Value payload = codec::Value::map_v();
                payload.set("type", codec::Value::text_v("net.open"));
                payload.set("id", codec::Value::uint_v(conn->id));
                impl->emit(conn->alias, conn->generation, std::move(payload));
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if (!conn || !impl) break;
            codec::Value payload = codec::Value::map_v();
            payload.set("type", codec::Value::text_v("net.data"));
            payload.set("id", codec::Value::uint_v(conn->id));
            payload.set("data", codec::Value::bytes(
                                    std::string_view(static_cast<const char*>(in), len)));
            impl->emit(conn->alias, conn->generation, std::move(payload));
            break;
        }
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            if (!conn) break;
            std::string message;
            {
                std::lock_guard<std::mutex> lock(conn->mutex);
                if (conn->outbox.empty()) break;
                message = std::move(conn->outbox.front());
                conn->outbox.pop_front();
            }
            std::vector<char> buffer(LWS_PRE + message.size());
            std::memcpy(buffer.data() + LWS_PRE, message.data(), message.size());
            lws_write(wsi, reinterpret_cast<unsigned char*>(buffer.data()) + LWS_PRE,
                      message.size(), LWS_WRITE_TEXT);
            {
                std::lock_guard<std::mutex> lock(conn->mutex);
                if (!conn->outbox.empty()) lws_callback_on_writable(wsi);
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLIENT_CLOSED: {
            if (!conn || !impl) break;
            {
                std::lock_guard<std::mutex> lock(conn->mutex);
                conn->open = false;
            }
            codec::Value payload = codec::Value::map_v();
            payload.set("type", codec::Value::text_v("net.close"));
            payload.set("id", codec::Value::uint_v(conn->id));
            impl->emit(conn->alias, conn->generation, std::move(payload));
            conn->wsi = nullptr;
            break;
        }
        case LWS_CALLBACK_TIMER: {
            if (!conn) break;
            conn->next_reconnect_ms = platform::monotonic_ms();
            const char ping[1] = {'p'};
            lws_write(wsi, reinterpret_cast<unsigned char*>(const_cast<char*>(ping)), 0,
                      LWS_WRITE_PING);
            lws_set_timer_usecs(wsi, static_cast<int>(conn->heartbeat_ms * 1000));
            break;
        }
        default:
            break;
    }
    return 0;
}

void NetService::Impl::lws_loop() {
    static const lws_protocols protocols[] = {
        {"pime", ws_callback, 0, 0, 0, nullptr, 0},
        {nullptr, nullptr, 0, 0, 0, nullptr, 0},
    };
    lws_context_creation_info info{};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.user = this;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    if (!config.ca_file.empty()) info.client_ssl_ca_filepath = config.ca_file.c_str();
    lws_ctx = lws_create_context(&info);
    if (!lws_ctx) return;
    while (!stopping.load()) {
        std::deque<uint64_t> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.swap(ws_pending_open);
        }
        for (uint64_t id : pending) {
            std::shared_ptr<WsConn> conn;
            std::string url;
            codec::Value options;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = ws_conns.find(id);
                if (it == ws_conns.end()) continue;
                conn = it->second;
                auto params = pending_ws_urls.find(id);
                if (params == pending_ws_urls.end()) continue;
                url = params->second.first;
                options = params->second.second;
                pending_ws_urls.erase(params);
            }
            create_ws(conn->alias, conn->generation, url, options, id);
        }
        lws_service(lws_ctx, 20);
    }
    std::vector<std::shared_ptr<WsConn>> conns;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& kv : ws_conns) conns.push_back(kv.second);
        ws_conns.clear();
    }
    for (auto& conn : conns)
        if (conn->wsi) lws_cancel_service(lws_ctx);
    lws_context_destroy(lws_ctx);
    lws_ctx = nullptr;
}

#endif  // PIME_HAVE_LWS

NetService::NetService(NetConfig config, EventSink sink)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->sink = std::move(sink);
#if defined(PIME_HAVE_CURL)
    curl_global_init(CURL_GLOBAL_DEFAULT);
    impl_->multi = curl_multi_init();
    impl_->http_thread = std::thread([this] { impl_->http_loop(); });
#endif
#if defined(PIME_HAVE_LWS)
    impl_->lws_thread = std::thread([this] { impl_->lws_loop(); });
#endif
}

NetService::~NetService() { shutdown(); }

pime_status NetService::http(const PluginCapContext& ctx, const codec::Value& request,
                             uint64_t* request_id) {
#if !defined(PIME_HAVE_CURL)
    (void)ctx; (void)request; (void)request_id;
    return PIME_UNSUPPORTED;
#else
    if (!request_id || request.type != codec::Type::Map) return PIME_INVALID;
    const codec::Value* url_value = request.find("url");
    if (!url_value || !url_value->is_text()) return PIME_INVALID;
    std::string url(url_value->as_text());
    pime_status st = impl_->validate_url(url);
    if (st != PIME_OK) return st;

    Impl::HttpTask task;
    task.alias = ctx.alias;
    task.generation = ctx.generation;
    task.request = request;
    task.url = std::move(url);
    const codec::Value* method = request.find("method");
    task.method = method && method->is_text() ? std::string(method->as_text()) : "GET";
    if (const codec::Value* timeout = request.find("timeout_ms"))
        task.timeout_ms = static_cast<uint32_t>(timeout->as_uint());
    if (const codec::Value* body = request.find("body")) {
        if (body->is_text() || body->is_bytes())
            task.body.assign(body->text.data(), body->text.size());
    }
    if (const codec::Value* headers = request.find("headers");
        headers && headers->type == codec::Type::Map) {
        for (const auto& kv : headers->map) {
            if (kv.first.type != codec::Type::Text) continue;
            task.headers += kv.first.text + ": " + std::string(kv.second.as_text()) + "\r\n";
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        task.id = impl_->next_id++;
        impl_->http_queue.push_back(std::move(task));
    }
    *request_id = task.id;
    impl_->cv.notify_all();
    return PIME_OK;
#endif
}

pime_status NetService::take_result(uint64_t request_id, codec::Value* out) {
#if !defined(PIME_HAVE_CURL)
    (void)request_id; (void)out;
    return PIME_UNSUPPORTED;
#else
    if (!out) return PIME_INVALID;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->results.find(request_id);
    if (it == impl_->results.end()) return PIME_NOT_FOUND;
    *out = std::move(it->second);
    impl_->results.erase(it);
    return PIME_OK;
#endif
}

pime_status NetService::open(const PluginCapContext& ctx, const std::string& proto,
                             const std::string& url, const codec::Value& options,
                             uint64_t* connection_id) {
#if !defined(PIME_HAVE_CURL) && !defined(PIME_HAVE_LWS)
    (void)ctx; (void)proto; (void)url; (void)options; (void)connection_id;
    return PIME_UNSUPPORTED;
#else
    if (!connection_id) return PIME_INVALID;
    if (proto != "sse" && proto != "ws" && proto != "wss") return PIME_INVALID;
    pime_status st = impl_->validate_url(url);
    if (st != PIME_OK) return st;
    if (proto == "sse" && url.rfind("ws", 0) == 0) return PIME_INVALID;

    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        id = impl_->next_id++;
    }
#if defined(PIME_HAVE_CURL)
    if (proto == "sse") {
        auto stream = std::make_shared<Impl::Stream>();
        stream->id = id;
        stream->alias = ctx.alias;
        stream->generation = ctx.generation;
        stream->proto = proto;
        stream->url = url;
        stream->options = options;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->streams[id] = stream;
        }
        stream->thread = std::thread([this, stream] { impl_->stream_loop(stream); });
        *connection_id = id;
        return PIME_OK;
    }
#endif
#if defined(PIME_HAVE_LWS)
    if (proto == "ws" || proto == "wss") {
        /*
         * Connection creation runs on the lws service thread; the URL and
         * options are queued there.
         */
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            /* Store pending parameters on the connection record. */
            auto conn = std::make_shared<Impl::WsConn>();
            conn->id = id;
            conn->alias = ctx.alias;
            conn->generation = ctx.generation;
            conn->heartbeat_ms = 15000;
            if (const codec::Value* hb = options.find("heartbeat_ms"))
                conn->heartbeat_ms = hb->as_uint();
            impl_->ws_conns[id] = conn;
            impl_->pending_ws_urls[id] = std::make_pair(url, options);
            impl_->ws_pending_open.push_back(id);
        }
        if (impl_->lws_ctx) lws_cancel_service(impl_->lws_ctx);
        *connection_id = id;
        return PIME_OK;
    }
#endif
    return PIME_UNSUPPORTED;
#endif
}

pime_status NetService::send(uint64_t connection_id, const void* data,
                             uint32_t len) {
#if !defined(PIME_HAVE_CURL) && !defined(PIME_HAVE_LWS)
    (void)connection_id; (void)data; (void)len;
    return PIME_UNSUPPORTED;
#else
    if (!data && len != 0) return PIME_INVALID;
#if defined(PIME_HAVE_LWS)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->ws_conns.find(connection_id);
        if (it != impl_->ws_conns.end()) {
            auto conn = it->second;
            std::lock_guard<std::mutex> conn_lock(conn->mutex);
            conn->outbox.emplace_back(static_cast<const char*>(data), len);
            if (conn->wsi) lws_callback_on_writable(conn->wsi);
            return PIME_OK;
        }
    }
    if (impl_->lws_ctx) lws_cancel_service(impl_->lws_ctx);
#endif
    return PIME_NOT_FOUND;
#endif
}

pime_status NetService::close(uint64_t connection_id) {
#if !defined(PIME_HAVE_CURL) && !defined(PIME_HAVE_LWS)
    (void)connection_id;
    return PIME_UNSUPPORTED;
#else
    std::shared_ptr<Impl::Stream> stream;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto sit = impl_->streams.find(connection_id);
        if (sit != impl_->streams.end()) {
            stream = sit->second;
            impl_->streams.erase(sit);
        }
    }
    if (stream) {
        stream->stop.store(true);
        if (stream->thread.joinable()) stream->thread.join();
        return PIME_OK;
    }
#if defined(PIME_HAVE_LWS)
    std::shared_ptr<Impl::WsConn> conn;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->ws_conns.find(connection_id);
        if (it != impl_->ws_conns.end()) {
            conn = it->second;
            impl_->ws_conns.erase(it);
        }
    }
    if (conn) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->ws_graveyard.push_back(conn);
        }
        if (conn->wsi && impl_->lws_ctx) lws_cancel_service(impl_->lws_ctx);
        return PIME_OK;
    }
#endif
    return PIME_NOT_FOUND;
#endif
}

void NetService::cancel_generation(const std::string& alias, uint64_t generation) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto it = impl_->streams.begin(); it != impl_->streams.end();) {
            if (it->second->alias == alias && it->second->generation == generation) {
                it->second->stop.store(true);
                if (it->second->thread.joinable()) it->second->thread.join();
                it = impl_->streams.erase(it);
            } else {
                ++it;
            }
        }
#if defined(PIME_HAVE_LWS)
        for (auto it = impl_->ws_conns.begin(); it != impl_->ws_conns.end();) {
            if (it->second->alias == alias && it->second->generation == generation) {
                impl_->ws_graveyard.push_back(it->second);
                if (it->second->wsi && impl_->lws_ctx) lws_cancel_service(impl_->lws_ctx);
                it = impl_->ws_conns.erase(it);
            } else {
                ++it;
            }
        }
#endif
        for (auto it = impl_->http_queue.begin(); it != impl_->http_queue.end();) {
            if (it->alias == alias && it->generation == generation)
                it = impl_->http_queue.erase(it);
            else
                ++it;
        }
    }
}

void NetService::pause() { impl_->paused.store(true); }
void NetService::resume() {
    impl_->paused.store(false);
    impl_->cv.notify_all();
}
void NetService::set_network_state(uint32_t state) { impl_->network_state.store(state); }

uint64_t NetService::open_streams() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(PIME_HAVE_LWS)
    return impl_->streams.size() + impl_->ws_conns.size();
#else
    return impl_->streams.size();
#endif
}

void NetService::shutdown() {
    if (!impl_) return;
    impl_->stopping.store(true);
    impl_->cv.notify_all();
#if defined(PIME_HAVE_CURL)
    if (impl_->http_thread.joinable()) impl_->http_thread.join();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& kv : impl_->streams) {
            kv.second->stop.store(true);
            if (kv.second->thread.joinable()) kv.second->thread.join();
        }
        impl_->streams.clear();
    }
    curl_global_cleanup();
#endif
#if defined(PIME_HAVE_LWS)
    if (impl_->lws_thread.joinable()) impl_->lws_thread.join();
#endif
}

#endif  // any net support

}  // namespace pime::cap
