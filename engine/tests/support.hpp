// Helpers shared by the tests.
#pragma once

#include "core/clock.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <exception>
#include <optional>
#include <utility>

namespace poni::test {

// Runs a coroutine to completion on `io` and returns its result (or rethrows).
template <class T>
T run(boost::asio::io_context& io, boost::asio::awaitable<T> task) {
    std::optional<T> result;
    std::exception_ptr error;
    boost::asio::co_spawn(io, std::move(task), [&](std::exception_ptr e, T value) {
        error = e;
        if (!e) result.emplace(std::move(value));
    });
    io.run();
    io.restart();
    if (error) std::rethrow_exception(error);
    return std::move(*result);
}

inline void run(boost::asio::io_context& io, boost::asio::awaitable<void> task) {
    std::exception_ptr error;
    boost::asio::co_spawn(io, std::move(task), [&](std::exception_ptr e) { error = e; });
    io.run();
    io.restart();
    if (error) std::rethrow_exception(error);
}

// 2026-11-10 12:00:00 UTC.
inline core::TimePoint t0() {
    using namespace std::chrono;
    return core::TimePoint(sys_days{2026y / November / 10} + 12h);
}

}  // namespace poni::test
