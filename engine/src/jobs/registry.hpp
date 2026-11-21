// The job kinds the engine runs: the coroutine that runs each, and which of
// them the scheduler starts by itself. Each job class installs its own kinds
// (`install` next to the class). A handler finishes normally to mark the job
// succeeded and throws to have it retried.
#pragma once

#include "jobs/queue.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace poni::jobs {

using Handler = std::function<boost::asio::awaitable<void>(const Job&)>;

// A kind the scheduler enqueues once per slot of `interval` (scheduler.hpp).
struct Recurring {
    std::string kind;
    std::chrono::seconds interval;
    // 1 for work whose next slot repeats it anyway (polling): a retry would
    // only run alongside the next slot's job.
    int max_attempts = 5;
};

class Registry {
public:
    void add(std::string kind, Handler handler) {
        if (!handlers_.emplace(std::move(kind), std::move(handler)).second) {
            throw std::logic_error("job kind registered twice");
        }
    }

    // Adds `kind`, which the scheduler also starts every `interval`.
    void every(std::string kind, std::chrono::seconds interval, int max_attempts, Handler handler) {
        recurring_.push_back({.kind = kind, .interval = interval, .max_attempts = max_attempts});
        add(std::move(kind), std::move(handler));
    }

    const Handler* find(const std::string& kind) const {
        const auto it = handlers_.find(kind);
        return it == handlers_.end() ? nullptr : &it->second;
    }

    const std::vector<Recurring>& recurring() const { return recurring_; }

private:
    std::map<std::string, Handler> handlers_;
    std::vector<Recurring> recurring_;
};

}  // namespace poni::jobs
