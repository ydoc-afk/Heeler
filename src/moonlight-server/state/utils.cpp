#include <algorithm>
#include <curl/curl.h>
#include <fstream>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <state/utils.hpp>

namespace utils {
std::optional<std::string> curl_get(std::string_view url) {
  auto curl = curl_easy_init();
  auto url_str = utils::to_string(url);
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
    /* Set custom writer (in order to receive back the response) */
    curl_easy_setopt(curl,
                     CURLOPT_WRITEFUNCTION,
                     static_cast<size_t (*)(char *, size_t, size_t, void *)>(
                         [](char *ptr, size_t size, size_t nmemb, void *read_buf) {
                           *(static_cast<std::string *>(read_buf)) += std::string{ptr, size * nmemb};
                           return size * nmemb;
                         }));
    std::string read_buf;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    auto res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      logs::log(logs::warning, "Could not download asset from {}, {}", url, curl_easy_strerror(res));
    } else {
      long response_code;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
      if (response_code == 200) {
        curl_easy_cleanup(curl);
        return read_buf;
      } else {
        logs::log(logs::warning, "Could not download asset from {}, response code: {}", url, response_code);
      }
    }
  }
  curl_easy_cleanup(curl);
  return {};
}

std::optional<std::string> get_file_content(const std::filesystem::path &path) {
  std::ifstream asset_file(path, std::ios::binary);
  if (!asset_file) {
    return {};
  }
  std::string content((std::istreambuf_iterator<char>(asset_file)), std::istreambuf_iterator<char>());
  return content;
}

std::optional<std::string> get_icon(std::string_view base_local_path, std::string_view icon_path) {
  if (icon_path.starts_with("/")) { // Absolute path
    auto asset_path = std::filesystem::path(icon_path);
    if (auto file_content = utils::get_file_content(asset_path)) {
      return file_content.value();
    }
    logs::log(logs::warning, "Could not open configured asset: {}", asset_path.string());
  } else if (icon_path.starts_with("http")) { // URL
    if (auto file_content = utils::curl_get(icon_path)) {
      return file_content.value();
    }
    logs::log(logs::warning, "Could not download configured asset: {}", icon_path);
  } else { // Assume it's a relative path (legacy setting)
    auto asset_path = std::filesystem::path(base_local_path) / icon_path;
    if (auto file_content = utils::get_file_content(asset_path)) {
      return file_content.value();
    }
    logs::log(logs::warning, "Could not open configured asset: {}", asset_path.string());
  }

  return std::nullopt;
}

bool is_icon_allowed(std::string_view icon_path,
                     std::string_view base_local_path,
                     const std::vector<std::string> &configured_icons,
                     const std::vector<std::string> &trusted_url_hosts) {
  if (std::find(configured_icons.begin(), configured_icons.end(), icon_path) != configured_icons.end()) {
    return true;
  }
  if (icon_path.starts_with("http")) {
    constexpr std::string_view https = "https://";
    if (!icon_path.starts_with(https)) {
      return false;
    }
    auto authority = icon_path.substr(https.size());
    authority = authority.substr(0, authority.find_first_of("/?#"));
    // No user info (https://trusted@evil) and no custom port, just the bare host
    if (authority.find_first_of("@:") != std::string_view::npos) {
      return false;
    }
    return std::find(trusted_url_hosts.begin(), trusted_url_hosts.end(), authority) != trusted_url_hosts.end();
  }
  if (icon_path.empty()) {
    return false;
  }

  std::error_code ec;
  auto base = std::filesystem::weakly_canonical(std::filesystem::path(base_local_path), ec);
  if (ec || base.empty()) {
    return false;
  }
  if (!base.has_filename()) { // trailing slash, ex: /etc/wolf/
    base = base.parent_path();
  }
  // Same resolution as get_icon(): absolute paths as-is, anything else relative to the base folder
  auto requested = icon_path.starts_with("/") ? std::filesystem::path(icon_path) : base / icon_path;
  auto resolved = std::filesystem::weakly_canonical(requested, ec);
  if (ec) {
    return false;
  }
  // resolved must be strictly inside base (`..` and symlinks have been resolved above)
  auto [base_end, _] = std::mismatch(base.begin(), base.end(), resolved.begin(), resolved.end());
  return base_end == base.end() && resolved != base;
}

} // namespace utils
