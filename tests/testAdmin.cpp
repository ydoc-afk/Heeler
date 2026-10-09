#include <catch2/catch_test_macros.hpp>
#include <state/admin.hpp>

TEST_CASE("Admin passwords", "[ADMIN]") {
  auto hash = state::hash_password("correct horse");
  REQUIRE(hash.rfind("pbkdf2-sha256$", 0) == 0);
  REQUIRE(hash.find("correct horse") == std::string::npos);

  REQUIRE(state::verify_password("correct horse", hash));
  REQUIRE_FALSE(state::verify_password("correct horse ", hash));
  REQUIRE_FALSE(state::verify_password("", hash));

  // Same password, different salt
  REQUIRE(state::hash_password("correct horse") != hash);

  // Broken or hostile hashes never verify (or take forever)
  REQUIRE_FALSE(state::verify_password("x", ""));
  REQUIRE_FALSE(state::verify_password("x", "plain"));
  REQUIRE_FALSE(state::verify_password("x", "pbkdf2-sha256$600000$zz$zz"));
  REQUIRE_FALSE(state::verify_password("x", "pbkdf2-sha256$999999999$00ff$00ff"));
}

TEST_CASE("Admin setup codes", "[ADMIN]") {
  auto code = state::make_setup_code();
  REQUIRE(code.size() == 9);
  REQUIRE(code[4] == '-');
  REQUIRE(code.find_first_of("01OI") == std::string::npos);
  REQUIRE(state::make_setup_code() != code);

  REQUIRE(state::normalize_setup_code("k7qm-2xpd") == "K7QM2XPD");
  REQUIRE(state::normalize_setup_code(" k7qm 2xpd ") == "K7QM2XPD");
  REQUIRE(state::secure_equals("K7QM2XPD", state::normalize_setup_code("K7QM-2XPD")));
  REQUIRE_FALSE(state::secure_equals("K7QM2XPD", "K7QM2XPE"));
  REQUIRE_FALSE(state::secure_equals("K7QM2XPD", "K7QM"));
}
