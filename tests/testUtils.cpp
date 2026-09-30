#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

#include <filesystem>
#include <fstream>
#include <helpers/utils.hpp>
#include <state/utils.hpp>

TEST_CASE("get_env reads a set variable", "[utils]") {
  setenv("HEALER_UTILS_TEST_PLAIN", "plain-value", 1);
  REQUIRE(std::string(utils::get_env("HEALER_UTILS_TEST_PLAIN")) == "plain-value");
  unsetenv("HEALER_UTILS_TEST_PLAIN");
}

TEST_CASE("get_env returns the default when unset", "[utils]") {
  unsetenv("HEALER_UTILS_TEST_MISSING");
  unsetenv("WOLF_UTILS_TEST_MISSING");
  REQUIRE(std::string(utils::get_env("HEALER_UTILS_TEST_MISSING", "fallback")) == "fallback");
  REQUIRE(utils::get_env("HEALER_UTILS_TEST_MISSING") == nullptr);
}

TEST_CASE("get_env HEALER_ name falls back to legacy WOLF_ name", "[utils]") {
  setenv("WOLF_UTILS_TEST_LEGACY", "legacy-value", 1);
  unsetenv("HEALER_UTILS_TEST_LEGACY");
  REQUIRE(std::string(utils::get_env("HEALER_UTILS_TEST_LEGACY")) == "legacy-value");
  unsetenv("WOLF_UTILS_TEST_LEGACY");
}

TEST_CASE("get_env HEALER_ name wins over legacy WOLF_ name", "[utils]") {
  setenv("WOLF_UTILS_TEST_BOTH", "legacy-value", 1);
  setenv("HEALER_UTILS_TEST_BOTH", "new-value", 1);
  REQUIRE(std::string(utils::get_env("HEALER_UTILS_TEST_BOTH")) == "new-value");
  unsetenv("HEALER_UTILS_TEST_BOTH");
  unsetenv("WOLF_UTILS_TEST_BOTH");
}

TEST_CASE("get_env has no legacy fallback for non-HEALER_ names", "[utils]") {
  setenv("WOLF_UTILS_TEST_ONLY", "legacy-value", 1);
  // WOLF_ names are read verbatim (no aliasing in the other direction)
  REQUIRE(std::string(utils::get_env("WOLF_UTILS_TEST_ONLY")) == "legacy-value");
  unsetenv("WOLF_UTILS_TEST_ONLY");
}

TEST_CASE("is_icon_allowed only serves configured icons, the state folder and trusted hosts", "[utils]") {
  namespace fs = std::filesystem;
  auto base = fs::temp_directory_path() / "heeler-icon-test";
  fs::remove_all(base);
  fs::create_directories(base / "icons");
  std::ofstream(base / "icons" / "app.png") << "PNG";
  std::error_code ec;
  fs::create_symlink("/etc/hostname", base / "icons" / "escape.png", ec);

  std::vector<std::string> configured = {"https://example.com/configured.png", "/opt/custom/icon.png"};

  // Configured icons are always allowed, whatever they are
  REQUIRE(utils::is_icon_allowed("https://example.com/configured.png", base.string(), configured));
  REQUIRE(utils::is_icon_allowed("/opt/custom/icon.png", base.string(), configured));

  // Files under the state folder, relative or absolute (with or without trailing slash on the base)
  REQUIRE(utils::is_icon_allowed("icons/app.png", base.string(), configured));
  REQUIRE(utils::is_icon_allowed((base / "icons" / "app.png").string(), base.string() + "/", configured));

  // Arbitrary files are refused, including path traversal and symlinks pointing outside
  REQUIRE(!utils::is_icon_allowed("/etc/shadow", base.string(), configured));
  REQUIRE(!utils::is_icon_allowed("../../../../etc/shadow", base.string(), configured));
  REQUIRE(!utils::is_icon_allowed((base / ".." / "etc").string(), base.string(), configured));
  if (!ec) {
    REQUIRE(!utils::is_icon_allowed("icons/escape.png", base.string(), configured));
  }
  REQUIRE(!utils::is_icon_allowed("", base.string(), configured));
  REQUIRE(!utils::is_icon_allowed(base.string(), base.string(), configured));

  // URLs: only https on a trusted host
  REQUIRE(utils::is_icon_allowed("https://games-on-whales.github.io/wildlife/apps/firefox/assets/icon.png",
                                 base.string(),
                                 configured));
  REQUIRE(!utils::is_icon_allowed("http://games-on-whales.github.io/icon.png", base.string(), configured));
  REQUIRE(!utils::is_icon_allowed("http://169.254.169.254/latest/meta-data/", base.string(), configured));
  REQUIRE(!utils::is_icon_allowed("https://games-on-whales.github.io@127.0.0.1/", base.string(), configured));
  REQUIRE(!utils::is_icon_allowed("https://games-on-whales.github.io:8443/", base.string(), configured));
  REQUIRE(!utils::is_icon_allowed("https://games-on-whales.github.io.evil.com/", base.string(), configured));
  REQUIRE(utils::is_icon_allowed("https://icons.local/a.png", base.string(), configured, {"icons.local"}));

  fs::remove_all(base);
}
