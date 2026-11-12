#ifndef PIME_UTIL_RESULT_HPP
#define PIME_UTIL_RESULT_HPP

#include <pime/abi.h>

#include <string>
#include <utility>

namespace pime {

using Status = pime_status;

inline bool ok(Status s) { return s == PIME_OK; }
inline bool failed(Status s) { return s != PIME_OK; }

const char* status_name(Status s);

/* Lightweight result for non-hot paths. Hot paths use Status + out params. */
template <typename T>
class Result {
public:
    Result(T value) : status_(PIME_OK), value_(std::move(value)) {}
    Result(Status status) : status_(status), value_() {}

    bool ok() const { return status_ == PIME_OK; }
    Status status() const { return status_; }
    const T& value() const { return value_; }
    T& value() { return value_; }
    T take() { return std::move(value_); }
    explicit operator bool() const { return ok(); }

private:
    Status status_;
    T value_;
};

class StatusResult {
public:
    StatusResult(Status s) : status_(s) {}
    Status status() const { return status_; }
    bool ok() const { return status_ == PIME_OK; }
    explicit operator bool() const { return ok(); }

private:
    Status status_;
};

}  // namespace pime

#endif
