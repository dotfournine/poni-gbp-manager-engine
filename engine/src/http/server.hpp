// HTTP/1.1 server on Boost.Beast. This is the only file pair that includes
// the Beast socket and parser headers (they are slow to compile); everything
// else sees Router, Request and Response.
#pragma once

#include "http/router.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace poni::http {

class Server {
public:
    // Binds and listens at once; throws boost::system::system_error if the
    // address is in use or invalid. Port 0 picks a free port (see port()).
    Server(boost::asio::any_io_executor ex, const Router& router, const std::string& host, std::uint16_t port);
    ~Server();

    // Starts accepting connections. The router must outlive the server.
    void start();

    // Stops accepting, closes idle connections, and lets requests in progress finish.
    void stop();

    std::uint16_t port() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace poni::http
