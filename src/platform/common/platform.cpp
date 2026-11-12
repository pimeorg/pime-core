#include "platform/platform.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pime::platform {

namespace fs = std::filesystem;

const char* current_platform() {
#if defined(_WIN32)
#  if defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#  else
    return "windows-x64";
#  endif
#elif defined(__APPLE__)
#  if TARGET_OS_IPHONE
#    if defined(__aarch64__) || defined(__arm64__)
    return "ios-arm64";
#    else
    return "ios-x64";
#    endif
#  else
#    if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#    else
    return "macos-x64";
#    endif
#  endif
#elif defined(__ANDROID__)
#  if defined(__aarch64__)
    return "android-arm64";
#  elif defined(__x86_64__)
    return "android-x64";
#  elif defined(__arm__)
    return "android-arm";
#  else
    return "android-x86";
#  endif
#elif defined(__OHOS__)
#  if defined(__aarch64__)
    return "harmony-arm64";
#  else
    return "harmony-x64";
#  endif
#elif defined(__linux__)
#  if defined(__aarch64__)
    return "linux-arm64";
#  else
    return "linux-x64";
#  endif
#else
    return "unknown";
#endif
}

const char* module_extension() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

uint64_t monotonic_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t wall_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void yield_thread() { std::this_thread::yield(); }

bool path_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}

bool is_directory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(fs::path(path), ec);
}

pime_status read_file(const std::string& path, std::vector<uint8_t>* out) {
    if (!out) return PIME_INVALID;
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) return PIME_NOT_FOUND;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) return PIME_IO;
    in.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out->data()), size);
    if (!in) return PIME_IO;
    return PIME_OK;
}

pime_status read_text_file(const std::string& path, std::string* out) {
    if (!out) return PIME_INVALID;
    std::vector<uint8_t> data;
    pime_status st = read_file(path, &data);
    if (st != PIME_OK) return st;
    out->assign(reinterpret_cast<const char*>(data.data()), data.size());
    return PIME_OK;
}

pime_status write_file(const std::string& path, const void* data, size_t len,
                       bool append) {
    std::ofstream out(fs::path(path),
                      std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) return PIME_IO;
    if (len > 0) out.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!out) return PIME_IO;
    return PIME_OK;
}

pime_status create_directories(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path), ec);
    if (ec) return PIME_IO;
    return is_directory(path) ? PIME_OK : PIME_IO;
}

pime_status list_directory(const std::string& path, std::vector<DirEntry>* out) {
    if (!out) return PIME_INVALID;
    std::error_code ec;
    fs::directory_iterator it(fs::path(path), ec);
    if (ec) return PIME_NOT_FOUND;
    for (const auto& entry : it) {
        DirEntry de;
        de.name = entry.path().filename().string();
        de.is_dir = entry.is_directory(ec);
        if (!de.is_dir) de.size = entry.is_regular_file(ec) ? entry.file_size(ec) : 0;
        out->push_back(std::move(de));
    }
    return PIME_OK;
}

pime_status remove_file(const std::string& path) {
    std::error_code ec;
    fs::remove(fs::path(path), ec);
    return ec ? PIME_IO : PIME_OK;
}

pime_status remove_tree(const std::string& path) {
    std::error_code ec;
    fs::remove_all(fs::path(path), ec);
    return ec ? PIME_IO : PIME_OK;
}

pime_status replace_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(fs::path(from), fs::path(to), ec);
    if (!ec) return PIME_OK;
    /* Windows cannot rename over an existing file. */
    fs::remove(fs::path(to), ec);
    ec.clear();
    fs::rename(fs::path(from), fs::path(to), ec);
    return ec ? PIME_IO : PIME_OK;
}

pime_status copy_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::copy_file(fs::path(from), fs::path(to), fs::copy_options::overwrite_existing, ec);
    return ec ? PIME_IO : PIME_OK;
}

uint64_t file_size(const std::string& path) {
    std::error_code ec;
    auto size = fs::file_size(fs::path(path), ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

std::string path_join(const std::string& a, const std::string& b) {
    return (fs::path(a) / fs::path(b)).string();
}

std::string path_parent(const std::string& path) {
    return fs::path(path).parent_path().string();
}

std::string path_filename(const std::string& path) {
    return fs::path(path).filename().string();
}

std::string path_absolute(const std::string& path) {
    std::error_code ec;
    auto p = fs::absolute(fs::path(path), ec);
    return ec ? path : p.lexically_normal().string();
}

std::string current_directory() {
    std::error_code ec;
    auto p = fs::current_path(ec);
    return ec ? std::string(".") : p.string();
}

}  // namespace pime::platform
