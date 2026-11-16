#include "hal_internal.hpp"

#include <new>

using namespace pime;

namespace {

bool session_ok(const pime_session* session, const pime_session** out) {
    if (!session || !session->impl || !session->core) return false;
    if (out) *out = session;
    return true;
}

pime_text empty_text() {
    pime_text text{};
    text.size = sizeof(text);
    text.version = PIME_ABI_MAJOR;
    return text;
}

}  // namespace

extern "C" {

pime_session* pime_session_create(pime_core* core,
                                  const pime_session_config* config) {
    if (!core || !core->impl) return nullptr;

    pime_session_config resolved{};
    resolved.size = sizeof(resolved);
    resolved.version = PIME_ABI_MAJOR;
    if (config) {
        if (!hal::struct_ok(config->size, config->version,
                            sizeof(pime_session_config)))
            return nullptr;
        resolved = *config;
    }

    pime_status status = PIME_OK;
    pime::core::Session* session = core->impl->create_session(resolved, &status);
    if (!session || status != PIME_OK) return nullptr;

    pime_session* wrapper = new (std::nothrow) pime_session();
    if (!wrapper) {
        core->impl->destroy_session(session);
        return nullptr;
    }
    wrapper->core = core->impl;
    wrapper->impl = session;
    return wrapper;
}

void pime_session_destroy(pime_session* session) {
    if (!session) return;
    if (session->core && session->impl)
        session->core->destroy_session(session->impl);
    delete session;
}

uint64_t pime_session_id(const pime_session* session) {
    if (!session || !session->impl) return 0;
    return session->impl->id;
}

pime_status pime_core_key(pime_session* session, const pime_key_event* event,
                          int* handled) {
    if (handled) *handled = 0;
    if (!session_ok(session, nullptr) || !event) return PIME_INVALID;
    if (!hal::struct_ok(event->size, event->version, sizeof(pime_key_event)))
        return PIME_INVALID;
    if (event->action > PIME_KEY_REPEAT) return PIME_INVALID;
    if (event->text_len && !event->text) return PIME_INVALID;

    bool result = false;
    pime_status status =
        session->core->key(session->impl, *event, &result);
    if (handled) *handled = result ? 1 : 0;
    return status;
}

pime_status pime_core_touch(pime_session* session,
                            const pime_touch_event* event, int* handled) {
    if (handled) *handled = 0;
    if (!session_ok(session, nullptr) || !event) return PIME_INVALID;
    if (!hal::struct_ok(event->size, event->version, sizeof(pime_touch_event)))
        return PIME_INVALID;
    if (event->phase > PIME_TOUCH_CANCEL) return PIME_INVALID;
    if (event->point_count > 4096) return PIME_INVALID;
    if (event->point_count && !event->points) return PIME_INVALID;

    bool result = false;
    pime_status status =
        session->core->touch(session->impl, *event, &result);
    if (handled) *handled = result ? 1 : 0;
    return status;
}

pime_status pime_core_text(pime_session* session, const pime_text* text) {
    if (!session_ok(session, nullptr) || !text) return PIME_INVALID;
    if (!hal::valid_text(*text)) return PIME_INVALID;
    return session->core->text(session->impl, *text);
}

pime_status pime_core_candidate(pime_session* session,
                                const pime_candidate_event* event) {
    if (!session_ok(session, nullptr) || !event) return PIME_INVALID;
    if (!hal::struct_ok(event->size, event->version,
                        sizeof(pime_candidate_event)))
        return PIME_INVALID;
    return session->core->candidate(session->impl, *event);
}

pime_status pime_core_surrounding(pime_session* session,
                                  const pime_text* before,
                                  const pime_text* after) {
    if (!session_ok(session, nullptr)) return PIME_INVALID;
    if (before && !hal::valid_text(*before)) return PIME_INVALID;
    if (after && !hal::valid_text(*after)) return PIME_INVALID;
    const pime_text empty = empty_text();
    return session->core->surrounding(session->impl,
                                      before ? *before : empty,
                                      after ? *after : empty);
}

pime_status pime_core_context(pime_session* session,
                              const pime_context* context) {
    if (!session_ok(session, nullptr) || !context) return PIME_INVALID;
    if (!hal::struct_ok(context->size, context->version, sizeof(pime_context)))
        return PIME_INVALID;
    if ((context->app_id_len && !context->app_id) ||
        (context->locale_len && !context->locale))
        return PIME_INVALID;
    return session->core->context(session->impl, *context);
}

pime_status pime_core_focus(pime_session* session, int focused) {
    if (!session_ok(session, nullptr)) return PIME_INVALID;
    return session->core->focus(session->impl, focused != 0);
}

pime_status pime_core_secure(pime_session* session, int secure) {
    if (!session_ok(session, nullptr)) return PIME_INVALID;
    return session->core->secure(session->impl, secure != 0);
}

pime_status pime_core_reset(pime_session* session) {
    if (!session_ok(session, nullptr)) return PIME_INVALID;
    return session->core->reset(session->impl);
}

pime_status pime_core_audio_in(pime_session* session,
                               const pime_audio_frame* frame) {
    if (!session_ok(session, nullptr) || !frame) return PIME_INVALID;
    if (!hal::valid_audio(*frame)) return PIME_INVALID;
    return session->core->audio_in(session->impl, *frame);
}

pime_status pime_core_voice(pime_session* session, uint32_t action) {
    if (!session_ok(session, nullptr)) return PIME_INVALID;
    return session->core->voice(session->impl, action);
}

}  // extern "C"
