// Injectable clock. Engine code asks a Clock for the time instead of calling
// std::chrono directly, so tests can move time forward without sleeping.
#pragma once

#include <chrono>

namespace poni::core {

using TimePoint = std::chrono::sys_time<std::chrono::microseconds>;

class Clock {
public:
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;
};

class SystemClock final : public Clock {
public:
    TimePoint now() const override {
        return std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
    }
};

// For tests: time only moves when told to.
class ManualClock final : public Clock {
public:
    explicit ManualClock(TimePoint start) : now_(start) {}
    TimePoint now() const override { return now_; }
    void set(TimePoint t) { now_ = t; }
    void advance(std::chrono::microseconds d) { now_ += d; }

private:
    TimePoint now_;
};

}  // namespace poni::core
