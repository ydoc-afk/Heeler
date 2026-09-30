#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace utils {

std::optional<std::string> curl_get(std::string_view url);

std::optional<std::string> get_file_content(const std::filesystem::path &path);

/**
 * Returns the raw content of the icon (if available).
 *
 * icon_path can be a URL, a relative or absolute path
 */
std::optional<std::string> get_icon(std::string_view base_local_path, std::string_view icon_path);

/**
 * Hosts that the API may download icons from even when they aren't in the configuration (ex: the app catalog)
 */
inline const std::vector<std::string> DEFAULT_ICON_URL_HOSTS = {"games-on-whales.github.io"};

/**
 * Whether an untrusted caller may fetch `icon_path` through get_icon(), it has to be one of:
 *  - an icon set in the configuration
 *  - a file that lives under base_local_path
 *  - an https:// URL on one of the trusted_url_hosts (exact host, no port or user info)
 * Anything else (arbitrary files, arbitrary URLs) is refused.
 */
bool is_icon_allowed(std::string_view icon_path,
                     std::string_view base_local_path,
                     const std::vector<std::string> &configured_icons,
                     const std::vector<std::string> &trusted_url_hosts = DEFAULT_ICON_URL_HOSTS);

} // namespace utils