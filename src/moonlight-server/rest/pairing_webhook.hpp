#pragma once

#include <chrono>
#include <curl/curl.h>
#include <fmt/format.h>
#include <helpers/logger.hpp>
#include <memory>
#include <rfl/json.hpp>
#include <string>
#include <thread>

/**
 * Notifies an external service (HEALER_PAIRING_WEBHOOK) when a Moonlight client starts pairing,
 * so that the admin gets the PIN page link on their phone instead of having to look at the log.
 */
namespace pairing_webhook {

struct Payload {
  /* Human readable message: `text` is what Slack displays, `content` is what Discord displays */
  std::string text;
  std::string content;
  std::string client_ip;
  std::string hostname;
  std::string pin_url;
};

inline std::string make_payload(const std::string &client_ip, const std::string &hostname, const std::string &pin_url) {
  auto who = hostname.empty() ? client_ip : fmt::format("{} ({})", hostname, client_ip);
  auto message = fmt::format("Heeler: {} wants to pair, enter its PIN at {}", who, pin_url);
  return rfl::json::write(
      Payload{.text = message, .content = message, .client_ip = client_ip, .hostname = hostname, .pin_url = pin_url});
}

constexpr auto TIMEOUT = std::chrono::seconds(5);

/**
 * POSTs the JSON payload to url, returns the HTTP status code (0 when the request couldn't be sent)
 */
inline long post(const std::string &url, const std::string &payload) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) {
    return 0;
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      curl_slist_append(nullptr, "Content-Type: application/json"),
      curl_slist_free_all);
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(TIMEOUT.count()));
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(
      curl.get(),
      CURLOPT_WRITEFUNCTION,
      +[](char *, size_t size, size_t nmemb, void *) {
        return size * nmemb; // Ignore the response body
      });
  if (auto res = curl_easy_perform(curl.get()); res != CURLE_OK) {
    logs::log(logs::warning, "[PAIRING] Webhook request failed: {}", curl_easy_strerror(res));
    return 0;
  }
  long status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
  return status;
}

/**
 * Fire and forget: runs on its own thread so that a slow webhook (or DNS lookup) never delays pairing
 */
template <typename HostnameResolver>
void notify(std::string url, std::string client_ip, std::string pin_url, HostnameResolver resolve_hostname) {
  std::thread([url = std::move(url),
               client_ip = std::move(client_ip),
               pin_url = std::move(pin_url),
               resolve_hostname = std::move(resolve_hostname)]() {
    auto status = post(url, make_payload(client_ip, resolve_hostname(client_ip), pin_url));
    if (status >= 200 && status < 300) {
      logs::log(logs::debug, "[PAIRING] Webhook notified ({})", status);
    } else if (status != 0) {
      logs::log(logs::warning, "[PAIRING] Webhook answered with HTTP {}", status);
    }
  }).detach();
}

} // namespace pairing_webhook
