#pragma once

#include <chrono>
#include <crypto/crypto.hpp>
#include <immer/map.hpp>
#include <string>
#include <string_view>
#include <utility>

/**
 * Protects the PIN landing page (GET /pin/pending) with a preset admin key (HEELER_PAIRING_KEY).
 *
 * The pending list hands out the pair secrets that are needed to submit a PIN, so without a key anybody
 * that can reach the HTTP port could start pairing, read their own secret and approve themselves.
 */
namespace pairing_key {

using clock = std::chrono::steady_clock;

constexpr int MAX_FAILURES = 5;
constexpr auto LOCKOUT = std::chrono::minutes(1);

struct Attempts {
  int failures = 0;
  clock::time_point window_start;
};

using AttemptsMap = immer::map<std::string /* client ip */, Attempts>;

enum class Result {
  OK,
  WRONG_KEY,
  LOCKED_OUT
};

/**
 * Constant time comparison: hashing first makes both sides the same length
 * so that neither the content nor the length of the key leaks through timing.
 */
inline bool matches(std::string_view provided, std::string_view expected) {
  auto a = crypto::sha256(provided);
  auto b = crypto::sha256(expected);
  unsigned char diff = a.size() ^ b.size();
  for (std::size_t i = 0; i < a.size() && i < b.size(); i++) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

/**
 * Checks the key sent by `client_ip`, after MAX_FAILURES wrong keys the client is locked out for LOCKOUT.
 * Pure: returns the result together with the updated attempts map.
 */
inline std::pair<Result, AttemptsMap> check(const AttemptsMap &attempts,
                                            const std::string &client_ip,
                                            std::string_view provided,
                                            std::string_view expected,
                                            clock::time_point now) {
  auto current = attempts.find(client_ip);
  auto window_expired = !current || now - current->window_start >= LOCKOUT;

  if (!window_expired && current->failures >= MAX_FAILURES) {
    return {Result::LOCKED_OUT, attempts};
  }

  if (matches(provided, expected)) {
    return {Result::OK, attempts.erase(client_ip)};
  }

  auto updated = window_expired ? Attempts{.failures = 1, .window_start = now}
                                : Attempts{.failures = current->failures + 1, .window_start = current->window_start};
  return {Result::WRONG_KEY, attempts.set(client_ip, updated)};
}

} // namespace pairing_key
