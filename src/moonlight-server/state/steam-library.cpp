#include <algorithm>
#include <cctype>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <set>
#include <state/steam-library.hpp>

namespace state {

std::optional<std::string> normalize_library_path(std::string_view path) {
  std::string result{path};
  while (result.size() > 1 && result.back() == '/') {
    result.pop_back();
  }
  if (result.size() < 2 || result.front() != '/') {
    return std::nullopt;
  }
  // The path ends up between quotes in a vdf file and in a Docker mount spec
  if (std::any_of(result.begin(), result.end(), [](unsigned char c) {
        return c == '"' || c == '\\' || c == ':' || std::iscntrl(c);
      })) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::string> steam_library_from_env() {
  auto value = utils::get_env("HEALER_STEAM_LIBRARY");
  if (!value || !*value) {
    return std::nullopt;
  }
  auto path = normalize_library_path(value);
  if (!path) {
    logs::log(logs::warning,
              "HEALER_STEAM_LIBRARY={} is ignored: it must be an absolute path on the host, not / and without "
              "quotes, backslashes or colons",
              value);
  }
  return path;
}

bool is_steam_image(std::string_view image) {
  std::string lower{image};
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  return lower.find("steam") != std::string::npos;
}

std::string add_library_folder(std::string_view vdf, std::string_view path) {
  auto entry = [&](int index, std::string_view p) {
    return "\t\"" + std::to_string(index) + "\"\n\t{\n\t\t\"path\"\t\t\"" + std::string{p} +
           "\"\n\t\t\"label\"\t\t\"\"\n\t\t\"apps\"\n\t\t{\n\t\t}\n\t}\n";
  };

  if (vdf.find("\"" + std::string{path} + "\"") != std::string_view::npos) {
    return std::string{vdf};
  }

  auto close = vdf.rfind('}');
  if (vdf.find("\"libraryfolders\"") == std::string_view::npos || close == std::string_view::npos) {
    // No usable file: start one, listing Steam's own folder first
    return "\"libraryfolders\"\n{\n" + entry(0, "/home/retro/.local/share/Steam") + entry(1, path) + "}\n";
  }

  // Next free index: one more than the highest numbered entry
  int next = 0;
  for (size_t pos = vdf.find("\t\""); pos != std::string_view::npos; pos = vdf.find("\t\"", pos + 1)) {
    size_t digits = pos + 2, end = digits;
    while (end < vdf.size() && std::isdigit(static_cast<unsigned char>(vdf[end]))) {
      ++end;
    }
    // Only top level entries: a number key one tab deep that is followed by a closing quote
    if (end > digits && end < vdf.size() && vdf[end] == '"' && (pos == 0 || vdf[pos - 1] == '\n') && end - digits < 6) {
      next = std::max(next, std::stoi(std::string{vdf.substr(digits, end - digits)}) + 1);
    }
  }

  return std::string{vdf.substr(0, close)} + entry(next, path) + std::string{vdf.substr(close)};
}

} // namespace state
