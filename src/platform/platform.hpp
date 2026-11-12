#ifndef PIME_PLATFORM_PLATFORM_HPP
#define PIME_PLATFORM_PLATFORM_HPP

#include <pime/abi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pime::platform {

/* e.g. "windows-x64", "macos-arm64", "linux-x64" */
const char* current_platform();
const char* module_extension();

uint64_t monotonic_ms();
uint64_t wall_ms();
int32_t timezone_offset_minutes();
void sleep_ms(uint32_t ms);
void yield_thread();

pime_status random_bytes(uint8_t* out, size_t len);

/* filesystem */
struct DirEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
};

bool path_exists(const std::string& path);
bool is_directory(const std::string& path);
pime_status read_file(const std::string& path, std::vector<uint8_t>* out);
pime_status read_text_file(const std::string& path, std::string* out);
pime_status write_file(const std::string& path, const void* data, size_t len,
                       bool append = false);
pime_status create_directories(const std::string& path);
pime_status list_directory(const std::string& path, std::vector<DirEntry>* out);
pime_status remove_file(const std::string& path);
pime_status remove_tree(const std::string& path);
/* Atomically replaces destination when the platform supports it. */
pime_status replace_file(const std::string& from, const std::string& to);
pime_status copy_file(const std::string& from, const std::string& to);
uint64_t file_size(const std::string& path);

std::string path_join(const std::string& a, const std::string& b);
std::string path_parent(const std::string& path);
std::string path_filename(const std::string& path);
std::string path_absolute(const std::string& path);
std::string current_directory();

/* dynamic library */
class Library {
public:
    Library() = default;
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    Library& operator=(Library&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    pime_status open(const std::string& path, std::string* error);
    void* symbol(const char* name);
    void close();
    bool is_open() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

}  // namespace pime::platform

#endif
