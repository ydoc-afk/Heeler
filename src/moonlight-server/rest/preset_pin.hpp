#pragma once

#include <chrono>
#include <helpers/logger.hpp>
#include <immer/map.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

/**
 * Optional preset pairing PIN (HEELER_PAIRING_PIN): pairing requests are answered with it right away,
 * for clients where the user can choose the PIN (ex: `moonlight pair <host> --pin 1234` with moonlight-qt).
 *
 * A 4 digit PIN can be guessed, so auto-answers are rate limited per client IP; past the limit the request falls
 * back to the normal flow (PIN page / API) where the admin has to approve it.
 */
namespace preset_pin {

using clock = std::chrono::steady_clock;

constexpr int MAX_ATTEMPTS = 5;
constexpr auto WINDOW = std::chrono::minutes(1);

struct Attempts {
  int count = 0;
  clock::time_point window_start;
};

using AttemptsMap = immer::map<std::string /* client ip */, Attempts>;

/**
 * Moonlight PINs are exactly 4 digits, anything else can't ever match and is ignored
 */
inline std::optional<std::string> parse(const char *value) {
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  std::string_view pin(value);
  if (pin.size() != 4 || pin.find_first_not_of("0123456789") != std::string_view::npos) {
    logs::log(logs::warning, "Ignoring HEELER_PAIRING_PIN: it must be exactly 4 digits");
    return std::nullopt;
  }
  return std::string(pin);
}

/**
 * Whether `client_ip` may get another auto-answered pairing attempt.
 * Pure: returns the decision together with the updated attempts map.
 */
inline std::pair<bool, AttemptsMap>
allow(const AttemptsMap &attempts, const std::string &client_ip, clock::time_point now) {
  auto current = attempts.find(client_ip);
  if (!current || now - current->window_start >= WINDOW) {
    return {true, attempts.set(client_ip, Attempts{.count = 1, .window_start = now})};
  }
  if (current->count >= MAX_ATTEMPTS) {
    return {false, attempts};
  }
  return {true, attempts.set(client_ip, Attempts{.count = current->count + 1, .window_start = current->window_start})};
}

} // namespace preset_pin
