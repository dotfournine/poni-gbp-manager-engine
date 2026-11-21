// A real server on a free loopback port, for the tests that need sockets.
#pragma once

#include "http/router.hpp"
#include "http/server.hpp"
#include "support.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace poni::test {

// Starts a server for `router` on a free loopback port, runs `exchange(port)`
// against it, and stops the server, also when the exchange threw. Returns
// what the exchange returned.
template <class Exchange>
auto with_server(boost::asio::io_context& io, const http::Router& router, Exchange exchange) {
    using T = typename std::invoke_result_t<Exchange&, std::uint16_t>::value_type;
    http::Server server(io.get_executor(), router, "127.0.0.1", 0);
    server.start();
    return run(io, [&]() -> boost::asio::awaitable<T> {
        std::exception_ptr failure;
        std::optional<T> result;
        try {
            result.emplace(co_await exchange(server.port()));
        } catch (...) {
            failure = std::current_exception();
        }
        server.stop();
        if (failure) std::rethrow_exception(failure);
        co_return std::move(*result);
    }());
}

}  // namespace poni::test
