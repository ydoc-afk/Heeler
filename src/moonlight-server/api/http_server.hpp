#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/write.hpp>
#include <fmt/format.h>
#include <functional>
#include <helpers/logger.hpp>
#include <immer/box.hpp>
#include <optional>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <string>
#include <string_view>
#include <utility.hpp>

namespace wolf::api {

namespace detail {

/**
 * Sends a minimal JSON error response to a unix-socket API client. Used as the
 * last-resort error path when a request handler throws (see HTTPServer::handle_request).
 */
template <typename Socket>
void send_error_response(const Socket &socket, int status_code, std::string_view message) {
  struct ErrorBody {
    bool success = false;
    std::string error;
  };
  auto body = rfl::json::write(ErrorBody{.error = std::string(message)});
  auto reply = fmt::format("HTTP/1.0 {} Internal Server Error\r\nContent-Length: {}\r\n\r\n{}",
                           status_code,
                           body.size(),
                           body);
  boost::system::error_code ec;
  boost::asio::write(socket->socket, boost::asio::buffer(reply), ec); // best effort: the client may be gone
}

} // namespace detail

enum class HTTPMethod {
  GET,
  POST,
  PUT,
  DELETE
};

struct HTTPRequest {
  HTTPMethod method{};
  std::string path{};
  std::string query_string{};
  std::string http_version{};
  SimpleWeb::CaseInsensitiveMultimap headers{};
  std::string body{};
};

struct APIDescription {
  std::string description;
  std::optional<std::string> json_schema;
};

template <typename Socket> struct RequestHandler {
  std::string summary;
  std::string description;
  std::optional<APIDescription> request_description = std::nullopt;
  std::vector<std::pair<int /*status code*/, APIDescription>> response_description = {};
  std::function<void(const HTTPRequest &, Socket socket)> handler;
};

template <typename T> class HTTPServer {
public:
  HTTPServer() : pool_(std::max(std::thread::hardware_concurrency(), 2u)) {};

  ~HTTPServer() {
    pool_.join();
  }

  void add(const HTTPMethod &method, const std::string &path, const RequestHandler<T> &handler) {
    endpoints_[{method, path}] = handler;
  }

  bool handle_request(const HTTPRequest &request, const T &socket) {
    auto it = endpoints_.find({request.method, request.path});
    if (it != endpoints_.end()) {
      auto boxed_request = immer::box<HTTPRequest>(request);
      auto handler = it->second.handler;
      boost::asio::post(pool_, [handler, boxed_request, socket]() {
        // Handlers run on the pool: an uncaught exception (e.g. std::stoul on a
        // malformed body, .value() on a missing field) would propagate out of the
        // pool thread and std::terminate the whole process. Catch it and answer
        // with a 500 so one bad request can't take the API down.
        try {
          handler(*boxed_request, socket);
        } catch (const std::exception &e) {
          logs::log(logs::error, "[API] Unhandled exception in request handler: {}", e.what());
          detail::send_error_response(socket, 500, e.what());
        } catch (...) {
          logs::log(logs::error, "[API] Unknown exception in request handler");
          detail::send_error_response(socket, 500, "unknown error");
        }
      });
      return true;
    }
    return false;
  }

  std::string openapi_schema() const;

private:
  std::map<std::pair<HTTPMethod, std::string>, RequestHandler<T>> endpoints_ = {};
  boost::asio::thread_pool pool_;
};

} // namespace wolf::api