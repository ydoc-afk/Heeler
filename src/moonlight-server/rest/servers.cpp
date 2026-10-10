#include <boost/property_tree/json_parser.hpp>
#include <events/events.hpp>
#include <immer/atom.hpp>
#include <immer/map_transient.hpp>
#include <netdb.h>
#include <rest/endpoints.hpp>
#include <rest/pairing_key.hpp>
#include <rest/pairing_webhook.hpp>
#include <rest/preset_pin.hpp>
#include <rest/trusted_nets.hpp>

namespace HTTPServers {

/**
 * A bit of magic here, it'll load up the pin.html via Cmake (look for `make_includable`)
 */
constexpr char const *pin_html =
#include "html/pin.include.html"

    ;

namespace bt = boost::property_tree;
using namespace wolf::core;

/**
 * Best-effort reverse DNS lookup for a client IP, so the PIN page can show e.g. "steamdeck"
 * instead of a bare IP. Returns an empty string when the IP can't be resolved
 * (common on LANs without PTR records), the page then falls back to the IP.
 */
std::string get_hostname(const std::string &ip) {
  struct addrinfo hints {};
  struct addrinfo *result = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_flags = AI_CANONNAME;
  if (getaddrinfo(ip.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
    return {};
  }
  std::string hostname = result->ai_canonname;
  freeaddrinfo(result);
  return hostname;
}

/**
 * Escapes a string so that it can be embedded in a JSON string literal
 * (hostnames come from reverse DNS, so they aren't under our control)
 */
std::string json_escape(std::string_view in) {
  std::string out;
  for (char c : in) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += fmt::format("\\u{:04x}", static_cast<int>(static_cast<unsigned char>(c)));
      } else {
        out += c;
      }
    }
  }
  return out;
}

/**
 * @brief Start the generic server on the specified port
 * @return std::thread: the thread where this server will run
 */
void startServer(HttpServer *server, const immer::box<state::AppState> state, int port) {
  server->config.port = port;
  server->config.address = "0.0.0.0";
  server->default_resource["GET"] = endpoints::not_found<SimpleWeb::HTTP>;
  server->default_resource["POST"] = endpoints::not_found<SimpleWeb::HTTP>;

  server->resource["^/serverinfo$"]["GET"] = [&state](auto resp, auto req) {
    endpoints::serverinfo<SimpleWeb::HTTP>(resp, req, {}, state);
  };

  server->resource["^/pair$"]["GET"] = [&state](auto resp, auto req) { endpoints::pair(resp, req, state); };

  auto pairing_atom = state->pairing_atom;

  server->resource["^/pin/$"]["GET"] = [](auto resp, auto req) {
    // Without a Content-Type some browsers don't render the page at all
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    resp->write(pin_html, headers);
  };
  // The page lives at /pin/, a typed /pin (no trailing slash) would otherwise get the Moonlight XML 404
  server->resource["^/pin$"]["GET"] = [](auto resp, auto req) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Location", "/pin/");
    resp->write(SimpleWeb::StatusCode::redirection_moved_permanently, headers);
  };
  server->resource["^/pin/$"]["POST"] = [pairing_atom](auto resp, auto req) {
    try {
      bt::ptree pt;

      read_json(req->content, pt);

      auto pin = pt.get<std::string>("pin");
      auto secret = pt.get<std::string>("secret");
      logs::log(logs::debug, "Received POST /pin/ pin:{} secret:{}", pin, secret);

      auto pair_request = pairing_atom->load()->at(secret);
      pair_request->user_pin->set_value(pin);
      resp->write("OK");
      pairing_atom->update([&secret](auto m) { return m.erase(secret); });
    } catch (const std::exception &e) {
      *resp << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << strlen(e.what()) << "\r\n\r\n" << e.what();
    }
  };

  // Lists the pending pair requests, so the PIN page (and any other client) can
  // discover them without reading the server log for the one-shot /pin/#<secret> URL.
  // The list hands out the secrets needed to submit a PIN, so it requires the preset HEELER_PAIRING_KEY
  std::string expected_key = utils::get_env("HEELER_PAIRING_KEY", "");
  // Clients on these networks (LAN / VPN) can use the page without the key, see trusted_nets.hpp
  auto trusted = std::make_shared<const std::vector<trusted_nets::Net>>(
      trusted_nets::from_env(utils::get_env("HEELER_PIN_TRUSTED_NETS")));
  if (expected_key.empty() && trusted->empty()) {
    logs::log(logs::info, "PIN landing page disabled, set HEELER_PAIRING_KEY to enable it at /pin/");
  } else if (expected_key.empty()) {
    logs::log(logs::info,
              "PIN landing page open to trusted networks only (HEELER_PIN_TRUSTED_NETS), set HEELER_PAIRING_KEY to "
              "also allow other networks");
  }
  auto key_attempts = std::make_shared<immer::atom<pairing_key::AttemptsMap>>();
  server->resource["^/pin/pending$"]["GET"] = [pairing_atom, expected_key, key_attempts, trusted](auto resp, auto req) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");

    // The TCP peer only, never a header the client could set; trusted networks skip the key entirely
    if (!trusted_nets::is_trusted(req->remote_endpoint().address(), *trusted)) {
      if (expected_key.empty()) {
        resp->write(SimpleWeb::StatusCode::client_error_forbidden, R"({"error":"disabled"})", headers);
        return;
      }

      auto client_ip = req->remote_endpoint().address().to_string();
      auto provided = get_header(req->header, "X-Pairing-Key").value_or("");
      if (provided.empty()) { // Not a guess: the page asks for the key, doesn't count towards the lockout
        resp->write(SimpleWeb::StatusCode::client_error_unauthorized, R"({"error":"key_required"})", headers);
        return;
      }
      auto result = pairing_key::Result::WRONG_KEY;
      key_attempts->update([&](const pairing_key::AttemptsMap &attempts) {
        auto [check_result, updated] =
            pairing_key::check(attempts, client_ip, provided, expected_key, pairing_key::clock::now());
        result = check_result;
        return updated;
      });
      if (result == pairing_key::Result::LOCKED_OUT) {
        logs::log(logs::warning, "[PIN] Too many wrong pairing keys from {}, locked out", client_ip);
        resp->write(SimpleWeb::StatusCode::client_error_too_many_requests, R"({"error":"locked_out"})", headers);
        return;
      } else if (result == pairing_key::Result::WRONG_KEY) {
        logs::log(logs::warning, "[PIN] Wrong pairing key from {}", client_ip);
        resp->write(SimpleWeb::StatusCode::client_error_unauthorized, R"({"error":"wrong_key"})", headers);
        return;
      }
    }

    std::string body = R"({"requests":[)";
    bool first = true;
    for (const auto &[secret, pair_request] : *pairing_atom->load()) {
      if (!first) {
        body += ',';
      }
      first = false;
      body += R"({"secret":")" + json_escape(secret) + R"(","client_ip":")" + json_escape(pair_request->client_ip) +
              R"(","hostname":")" + json_escape(get_hostname(pair_request->client_ip)) + R"("})";
    }
    body += "]}";
    resp->write(SimpleWeb::StatusCode::success_ok, body, headers);
  };

  server->resource["^/unpair$"]["GET"] = [&state](auto resp, auto req) {
    SimpleWeb::CaseInsensitiveMultimap headers = req->parse_query_string();
    auto client_id = get_header(headers, "uniqueid");
    auto client_ip = req->remote_endpoint().address().to_string();

    // This endpoint is unauthenticated, so validate the input and answer with 400
    // instead of throwing out of the handler (which would terminate the process)
    if (!client_id.has_value()) {
      logs::log(logs::warning, "[HTTP] /unpair request without a 'uniqueid' parameter from {}", client_ip);
      endpoints::server_error<SimpleWeb::HTTP>(resp);
      return;
    }
    auto cache_key = client_id.value() + "@" + client_ip;

    logs::log(logs::info, "Unpairing: {}", cache_key);
    auto cache = state->pairing_cache->load().get();
    auto cached_client = cache.find(cache_key);
    if (!cached_client) {
      logs::log(logs::warning, "[HTTP] /unpair request for unknown client: {}", cache_key);
      endpoints::server_error<SimpleWeb::HTTP>(resp);
      return;
    }
    state::unpair(state->config, state::PairedClient{.client_cert = cached_client->client_cert});

    XML xml;
    xml.put("root.<xmlattr>.status_code", 200);
    send_xml<SimpleWeb::HTTP>(resp, SimpleWeb::StatusCode::success_ok, xml);
  };

  auto preset_pin = preset_pin::parse(utils::get_env("HEELER_PAIRING_PIN"));
  if (preset_pin) {
    logs::log(logs::warning,
              "HEELER_PAIRING_PIN is set: any client that knows it can pair, remove it once you're done pairing");
  }
  auto preset_pin_attempts = std::make_shared<immer::atom<preset_pin::AttemptsMap>>();
  std::string pairing_webhook_url = utils::get_env("HEELER_PAIRING_WEBHOOK", "");
  auto pair_handler = state->event_bus->register_handler<immer::box<events::PairSignal>>(
      [pairing_atom, preset_pin, preset_pin_attempts, pairing_webhook_url](
          const immer::box<events::PairSignal> pair_sig) {
        if (preset_pin) {
          bool allowed = false;
          preset_pin_attempts->update([&](const preset_pin::AttemptsMap &attempts) {
            auto [is_allowed, updated] = preset_pin::allow(attempts, pair_sig->client_ip, preset_pin::clock::now());
            allowed = is_allowed;
            return updated;
          });
          if (allowed) {
            logs::log(logs::info, "Answering pairing request from {} with HEELER_PAIRING_PIN", pair_sig->client_ip);
            pair_sig->user_pin->set_value(*preset_pin);
            return;
          }
          logs::log(logs::warning,
                    "Too many pairing attempts from {}, it has to be approved with the PIN page instead",
                    pair_sig->client_ip);
        }
        auto secret = crypto::str_to_hex(crypto::random(8));
        auto http_port = std::to_string(state::get_port(state::HTTP_PORT));
        auto pin_url = fmt::format("http://{}:{}/pin/#{}", pair_sig->host_ip, http_port, secret);
        logs::log(logs::info, "Insert pin at {}", pin_url);
        pairing_atom->update([&pair_sig, &secret](const immer::map<std::string, immer::box<events::PairSignal>> &m) {
          // filter out any other (dangling) pair request from the same client
          auto t_map = m.transient();
          for (auto [key, value] : m) {
            if (value->client_ip == pair_sig->client_ip) {
              t_map.erase(key);
            }
          }
          // insert the new pair request
          t_map.set(secret, pair_sig);
          return t_map.persistent();
        });
        // Only once the request is listed as pending, so that the link works right away
        if (!pairing_webhook_url.empty()) {
          pairing_webhook::notify(pairing_webhook_url, pair_sig->client_ip, pin_url, get_hostname);
        }
      });

  // Start server (blocks until stopped, so the PairSignal handler above
  // stays registered for the lifetime of the server)
  server->start([](unsigned short port) { logs::log(logs::info, "HTTP server listening on port: {} ", port); });
}

std::optional<state::PairedClient>
get_client_if_paired(const immer::box<state::AppState> state,
                     const std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> &request) {
  auto client_cert = SimpleWeb::Server<SimpleWeb::HTTPS>::get_client_cert(request);
  return state::get_client_via_ssl(state->config, std::move(client_cert));
}

void reply_unauthorized(const std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> &request,
                        const std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> &response) {
  logs::log(logs::warning, "Received HTTPS request from a client which wasn't previously paired.");

  XML xml;

  xml.put("root.<xmlattr>.status_code"s, 401);
  xml.put("root.<xmlattr>.query"s, request->path);
  xml.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);

  send_xml<SimpleWeb::HTTPS>(response, SimpleWeb::StatusCode::client_error_unauthorized, xml);
}

void startServer(HttpsServer *server, const immer::box<state::AppState> state, int port) {
  server->config.port = port;
  server->config.address = "0.0.0.0";
  server->default_resource["GET"] = endpoints::not_found<SimpleWeb::HTTPS>;
  server->default_resource["POST"] = endpoints::not_found<SimpleWeb::HTTPS>;

  server->resource["^/serverinfo$"]["GET"] = [&state](auto resp, auto req) {
    if (auto client = get_client_if_paired(state, req)) {
      auto client_session = state::get_session_by_client(state->running_sessions->load(), client.value());
      endpoints::serverinfo<SimpleWeb::HTTPS>(resp, req, client_session, state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/pair$"]["GET"] = [&state](auto resp, auto req) {
    if (get_client_if_paired(state, req)) {
      endpoints::https::pair(resp, req);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/applist$"]["GET"] = [&state](auto resp, auto req) {
    if (get_client_if_paired(state, req)) {
      endpoints::https::applist(resp, req, state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/launch"]["GET"] = [&state](auto resp, auto req) {
    if (auto client = get_client_if_paired(state, req)) {
      endpoints::https::launch(resp, req, client.value(), state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/resume$"]["GET"] = [&state](auto resp, auto req) {
    if (auto client = get_client_if_paired(state, req)) {
      endpoints::https::resume(resp, req, client.value(), state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/cancel$"]["GET"] = [&state](auto resp, auto req) {
    if (auto client = get_client_if_paired(state, req)) {
      endpoints::https::cancel(resp, req, client.value(), state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/appasset$"]["GET"] = [&state](auto resp, auto req) {
    if (get_client_if_paired(state, req)) {
      endpoints::https::appasset(resp, req, state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->start([](unsigned short port) { logs::log(logs::info, "HTTPS server listening on port: {} ", port); });
}

} // namespace HTTPServers