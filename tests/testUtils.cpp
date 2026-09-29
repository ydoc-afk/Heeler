#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

#include <helpers/utils.hpp>

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
