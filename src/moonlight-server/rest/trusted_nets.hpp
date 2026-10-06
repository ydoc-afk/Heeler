#pragma once

#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <charconv>
#include <helpers/logger.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Networks that may use the PIN landing page (GET /pin/pending) without the HEALER_PAIRING_KEY.
 *
 * Heeler is meant to run on a LAN or behind a VPN (ex: Tailscale), where "who can reach the port" is already the
 * access control, so asking for a second secret just to type the 4-digit PIN Moonlight shows is friction.
 * Clients from other networks still need the key.
 *
 * Only ever feed this the TCP peer address (`remote_endpoint()`), never a forwarded header: a header is chosen by
 * the client. Loopback is deliberately NOT trusted by default, reverse proxies and `tailscale serve`/Funnel connect
 * from 127.0.0.1 and would make any internet client look local.
 */
namespace trusted_nets {

/**
 * Used when HEALER_PIN_TRUSTED_NETS is unset: the private ranges plus Tailscale's CGNAT range.
 * (Tailscale's IPv6 range fd7a:115c:a1e0::/48 is inside the unique-local fc00::/7.)
 */
constexpr std::string_view DEFAULT_NETS =
    "10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,100.64.0.0/10,fc00::/7,fe80::/10";

struct Net {
  boost::asio::ip::address address;
  int prefix_len;
};

/**
 * An IPv4 client reaching a dual-stack socket shows up as ::ffff:a.b.c.d, treat it as the IPv4 address
 */
inline boost::asio::ip::address normalize(const boost::asio::ip::address &addr) {
  if (addr.is_v6()) {
    auto v6 = addr.to_v6();
    if (v6.is_v4_mapped()) {
      return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
    }
  }
  return addr;
}

/**
 * Same first `prefix_len` bits?
 */
template <std::size_t N>
inline bool prefix_matches(const std::array<unsigned char, N> &a, const std::array<unsigned char, N> &b, int prefix_len) {
  for (std::size_t i = 0; i < N && prefix_len > 0; i++, prefix_len -= 8) {
    int bits = std::min(prefix_len, 8);
    auto mask = static_cast<unsigned char>(0xFF << (8 - bits));
    if ((a[i] & mask) != (b[i] & mask)) {
      return false;
    }
  }
  return true;
}

/**
 * "192.168.0.0/16", "fd7a:115c:a1e0::/48" or a single address (a /32 or /128)
 */
inline std::optional<Net> parse_net(std::string_view text) {
  auto slash = text.find('/');
  auto address_text = std::string(text.substr(0, slash));

  boost::system::error_code ec;
  auto address = boost::asio::ip::make_address(address_text, ec);
  if (ec) {
    return std::nullopt;
  }
  int max_len = address.is_v4() ? 32 : 128;

  int prefix_len = max_len;
  if (slash != std::string_view::npos) {
    auto len_text = text.substr(slash + 1);
    auto [end, err] = std::from_chars(len_text.data(), len_text.data() + len_text.size(), prefix_len);
    if (err != std::errc() || end != len_text.data() + len_text.size() || prefix_len < 0 || prefix_len > max_len) {
      return std::nullopt;
    }
  }
  return Net{.address = address, .prefix_len = prefix_len};
}

/**
 * A list separated by commas, semicolons and/or whitespace. Invalid entries are skipped (with a warning):
 * a typo must not silently trust more than intended, and must not take the server down either.
 */
inline std::vector<Net> parse_list(std::string_view list) {
  std::vector<Net> nets;
  while (!list.empty()) {
    auto start = list.find_first_not_of(",; \t\r\n");
    if (start == std::string_view::npos) {
      break;
    }
    list.remove_prefix(start);
    auto end = list.find_first_of(",; \t\r\n");
    auto entry = list.substr(0, end);
    if (auto net = parse_net(entry)) {
      nets.push_back(*net);
    } else {
      logs::log(logs::warning, "Ignoring invalid entry in HEALER_PIN_TRUSTED_NETS: {}", entry);
    }
    list = end == std::string_view::npos ? std::string_view{} : list.substr(end);
  }
  return nets;
}

/**
 * HEALER_PIN_TRUSTED_NETS: unset -> the defaults, set but empty -> nobody is trusted (the key is always required)
 */
inline std::vector<Net> from_env(const char *value) {
  return parse_list(value == nullptr ? DEFAULT_NETS : std::string_view(value));
}

inline bool is_trusted(const boost::asio::ip::address &client, const std::vector<Net> &nets) {
  auto addr = normalize(client);
  return std::any_of(nets.begin(), nets.end(), [&](const Net &net) {
    if (addr.is_v4() && net.address.is_v4()) {
      return prefix_matches(addr.to_v4().to_bytes(), net.address.to_v4().to_bytes(), net.prefix_len);
    }
    if (addr.is_v6() && net.address.is_v6()) {
      return prefix_matches(addr.to_v6().to_bytes(), net.address.to_v6().to_bytes(), net.prefix_len);
    }
    return false;
  });
}

} // namespace trusted_nets
