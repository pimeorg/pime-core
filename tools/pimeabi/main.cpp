#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <pime/abi.h>
#include <pime/backend.h>
#include <pime/core.h>
#include <pime/hal.h>
#include <pime/plugin.h>

#include "util/result.hpp"

namespace {

constexpr const char* kUsage =
    "usage: pimeabi dump | pimeabi check <baseline-file>\n";

struct Report {
    std::string text;

    void line(const std::string& value) {
        text += value;
        text += '\n';
    }

    void field(const char* type, const char* name, size_t offset) {
        std::ostringstream out;
        out << type << "." << name << "=" << offset;
        line(out.str());
    }

    void size(const char* type, size_t value) {
        std::ostringstream out;
        out << "sizeof " << type << "=" << value;
        line(out.str());
    }

    void value(const char* name, long long number) {
        std::ostringstream out;
        out << name << "=" << number;
        line(out.str());
    }
};

void collect(Report* report) {
    Report& r = *report;
    r.value("PIME_ABI_MAJOR", PIME_ABI_MAJOR);
    r.value("PIME_ABI_MINOR", PIME_ABI_MINOR);

    for (pime_status status = PIME_OK; status <= PIME_CORRUPT;
         status = static_cast<pime_status>(status + 1))
        r.value(pime::status_name(status), status);

#define PIME_STRUCT(T) r.size(#T, sizeof(T));
#define PIME_FIELD(T, f) r.field(#T, #f, offsetof(T, f));

    PIME_STRUCT(pime_core_config)
    PIME_FIELD(pime_core_config, size)
    PIME_FIELD(pime_core_config, version)
    PIME_FIELD(pime_core_config, plugin_dir)
    PIME_FIELD(pime_core_config, plugin_dir_len)
    PIME_FIELD(pime_core_config, data_dir)
    PIME_FIELD(pime_core_config, data_dir_len)
    PIME_FIELD(pime_core_config, backend_dir)
    PIME_FIELD(pime_core_config, backend_dir_len)
    PIME_FIELD(pime_core_config, trust_store)
    PIME_FIELD(pime_core_config, trust_store_len)
    PIME_FIELD(pime_core_config, proxy)
    PIME_FIELD(pime_core_config, proxy_len)
    PIME_FIELD(pime_core_config, ca_file)
    PIME_FIELD(pime_core_config, ca_file_len)
    PIME_FIELD(pime_core_config, client_cert)
    PIME_FIELD(pime_core_config, client_cert_len)
    PIME_FIELD(pime_core_config, client_key)
    PIME_FIELD(pime_core_config, client_key_len)
    PIME_FIELD(pime_core_config, store_key)
    PIME_FIELD(pime_core_config, store_key_len)
    PIME_FIELD(pime_core_config, log_level)
    PIME_FIELD(pime_core_config, flags)
    PIME_FIELD(pime_core_config, memory_budget_mb)

    PIME_STRUCT(pime_session_config)
    PIME_FIELD(pime_session_config, id)
    PIME_FIELD(pime_session_config, flags)

    PIME_STRUCT(pime_log_record)
    PIME_FIELD(pime_log_record, time_ms)
    PIME_FIELD(pime_log_record, level)
    PIME_FIELD(pime_log_record, source)
    PIME_FIELD(pime_log_record, source_len)
    PIME_FIELD(pime_log_record, tag)
    PIME_FIELD(pime_log_record, tag_len)
    PIME_FIELD(pime_log_record, msg)
    PIME_FIELD(pime_log_record, msg_len)

    PIME_STRUCT(pime_text)
    PIME_FIELD(pime_text, text)
    PIME_FIELD(pime_text, text_len)

    PIME_STRUCT(pime_key_event)
    PIME_FIELD(pime_key_event, keycode)
    PIME_FIELD(pime_key_event, modifiers)
    PIME_FIELD(pime_key_event, action)
    PIME_FIELD(pime_key_event, text)
    PIME_FIELD(pime_key_event, text_len)
    PIME_FIELD(pime_key_event, time_ms)

    PIME_STRUCT(pime_touch_point)
    PIME_FIELD(pime_touch_point, x)
    PIME_FIELD(pime_touch_point, y)
    PIME_FIELD(pime_touch_point, pressure)
    PIME_FIELD(pime_touch_point, t_ms)

    PIME_STRUCT(pime_touch_event)
    PIME_FIELD(pime_touch_event, phase)
    PIME_FIELD(pime_touch_event, points)
    PIME_FIELD(pime_touch_event, point_count)
    PIME_FIELD(pime_touch_event, time_ms)

    PIME_STRUCT(pime_candidate_event)
    PIME_FIELD(pime_candidate_event, index)

    PIME_STRUCT(pime_context)
    PIME_FIELD(pime_context, app_id)
    PIME_FIELD(pime_context, app_id_len)
    PIME_FIELD(pime_context, field_type)
    PIME_FIELD(pime_context, input_type)
    PIME_FIELD(pime_context, locale)
    PIME_FIELD(pime_context, locale_len)
    PIME_FIELD(pime_context, page_size)
    PIME_FIELD(pime_context, host_caps)

    PIME_STRUCT(pime_segment)
    PIME_FIELD(pime_segment, start)
    PIME_FIELD(pime_segment, end)
    PIME_FIELD(pime_segment, type)

    PIME_STRUCT(pime_composition)
    PIME_FIELD(pime_composition, text)
    PIME_FIELD(pime_composition, text_len)
    PIME_FIELD(pime_composition, cursor)
    PIME_FIELD(pime_composition, segments)
    PIME_FIELD(pime_composition, segment_count)

    PIME_STRUCT(pime_candidate)
    PIME_FIELD(pime_candidate, text)
    PIME_FIELD(pime_candidate, text_len)
    PIME_FIELD(pime_candidate, comment)
    PIME_FIELD(pime_candidate, comment_len)
    PIME_FIELD(pime_candidate, label)
    PIME_FIELD(pime_candidate, label_len)

    PIME_STRUCT(pime_candidate_list)
    PIME_FIELD(pime_candidate_list, items)
    PIME_FIELD(pime_candidate_list, count)

    PIME_STRUCT(pime_audio_frame)
    PIME_FIELD(pime_audio_frame, rate)
    PIME_FIELD(pime_audio_frame, channels)
    PIME_FIELD(pime_audio_frame, format)
    PIME_FIELD(pime_audio_frame, data)
    PIME_FIELD(pime_audio_frame, data_len)
    PIME_FIELD(pime_audio_frame, time_ms)

    PIME_STRUCT(pime_host_sink)
    PIME_FIELD(pime_host_sink, set_composition)
    PIME_FIELD(pime_host_sink, commit)
    PIME_FIELD(pime_host_sink, show_candidates)
    PIME_FIELD(pime_host_sink, hide_candidates)
    PIME_FIELD(pime_host_sink, audio_out)
    PIME_FIELD(pime_host_sink, beep)
    PIME_FIELD(pime_host_sink, clear_ink)
    PIME_FIELD(pime_host_sink, notify)
    PIME_FIELD(pime_host_sink, on_log)
    PIME_FIELD(pime_host_sink, wake)

    PIME_STRUCT(pime_plugin_ref)
    PIME_FIELD(pime_plugin_ref, size)
    PIME_FIELD(pime_plugin_ref, version)
    PIME_FIELD(pime_plugin_ref, id)
    PIME_FIELD(pime_plugin_ref, id_len)
    PIME_FIELD(pime_plugin_ref, semver)
    PIME_FIELD(pime_plugin_ref, semver_len)

    PIME_STRUCT(pime_ml_tensor)
    PIME_FIELD(pime_ml_tensor, size)
    PIME_FIELD(pime_ml_tensor, version)
    PIME_FIELD(pime_ml_tensor, dtype)
    PIME_FIELD(pime_ml_tensor, rank)
    PIME_FIELD(pime_ml_tensor, shape)
    PIME_FIELD(pime_ml_tensor, data)
    PIME_FIELD(pime_ml_tensor, data_len)

    PIME_STRUCT(pime_ml_request)
    PIME_FIELD(pime_ml_request, size)
    PIME_FIELD(pime_ml_request, version)
    PIME_FIELD(pime_ml_request, desc)
    PIME_FIELD(pime_ml_request, desc_len)
    PIME_FIELD(pime_ml_request, inputs)
    PIME_FIELD(pime_ml_request, input_count)
    PIME_FIELD(pime_ml_request, outputs)
    PIME_FIELD(pime_ml_request, output_cap)
    PIME_FIELD(pime_ml_request, output_count)

    PIME_STRUCT(pime_ml_backend)
    PIME_FIELD(pime_ml_backend, size)
    PIME_FIELD(pime_ml_backend, version)
    PIME_FIELD(pime_ml_backend, load)
    PIME_FIELD(pime_ml_backend, run)
    PIME_FIELD(pime_ml_backend, cancel)
    PIME_FIELD(pime_ml_backend, free)

#undef PIME_FIELD
#undef PIME_STRUCT
}

bool read_baseline(const std::string& path, std::string* out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    std::string text = buffer.str();
    text.erase(std::remove(text.begin(), text.end(), static_cast<char>(13)), text.end());
    *out = text;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Report report;
    collect(&report);

    if (argc == 1 || std::strcmp(argv[1], "dump") == 0) {
        std::fwrite(report.text.data(), 1, report.text.size(), stdout);
        return 0;
    }

    if (std::strcmp(argv[1], "check") == 0 && argc == 3) {
        std::string baseline;
        if (!read_baseline(argv[2], &baseline)) {
            std::fprintf(stderr, "pimeabi: cannot read %s\n", argv[2]);
            return 2;
        }
        if (baseline == report.text) {
            std::printf("abi ok\n");
            return 0;
        }

        std::vector<std::string> expected;
        std::istringstream expected_stream(baseline);
        for (std::string line; std::getline(expected_stream, line);)
            expected.push_back(line);
        std::vector<std::string> actual;
        std::istringstream actual_stream(report.text);
        for (std::string line; std::getline(actual_stream, line);)
            actual.push_back(line);

        size_t index = 0;
        while (index < expected.size() || index < actual.size()) {
            const std::string e = index < expected.size() ? expected[index] : "";
            const std::string a = index < actual.size() ? actual[index] : "";
            if (e != a)
                std::printf("line %zu: expected '%s' got '%s'\n", index + 1,
                            e.c_str(), a.c_str());
            ++index;
        }
        std::fputs("abi changed\n", stderr);
        return 1;
    }

    std::fputs(kUsage, stderr);
    return 2;
}
