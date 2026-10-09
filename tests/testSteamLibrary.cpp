#include <catch2/catch_test_macros.hpp>
#include <state/steam-library.hpp>

TEST_CASE("Steam library path", "[STEAM_LIBRARY]") {
  REQUIRE(state::normalize_library_path("/mnt/games/steam") == "/mnt/games/steam");
  REQUIRE(state::normalize_library_path("/mnt/games/steam///") == "/mnt/games/steam");

  // Heeler doesn't expand ~ or $HOME, and the path ends up in a mount spec and a vdf file
  REQUIRE_FALSE(state::normalize_library_path("~/steam"));
  REQUIRE_FALSE(state::normalize_library_path("steam"));
  REQUIRE_FALSE(state::normalize_library_path(""));
  REQUIRE_FALSE(state::normalize_library_path("/"));
  REQUIRE_FALSE(state::normalize_library_path("/mnt/a\"b"));
  REQUIRE_FALSE(state::normalize_library_path("/mnt/a:b"));
  REQUIRE_FALSE(state::normalize_library_path("/mnt/a\\b"));
  REQUIRE_FALSE(state::normalize_library_path("/mnt/a\nb"));
}

TEST_CASE("Steam image detection", "[STEAM_LIBRARY]") {
  REQUIRE(state::is_steam_image("ghcr.io/games-on-whales/steam:edge"));
  REQUIRE(state::is_steam_image("Steam"));
  REQUIRE_FALSE(state::is_steam_image("ghcr.io/games-on-whales/firefox:edge"));
}

TEST_CASE("Adding a library folder to libraryfolders.vdf", "[STEAM_LIBRARY]") {
  SECTION("No file yet") {
    auto vdf = state::add_library_folder("", "/steam-library");
    REQUIRE(vdf.find("\"libraryfolders\"") != std::string::npos);
    REQUIRE(vdf.find("\"/home/retro/.local/share/Steam\"") != std::string::npos);
    REQUIRE(vdf.find("\"path\"\t\t\"/steam-library\"") != std::string::npos);
    // Idempotent
    REQUIRE(state::add_library_folder(vdf, "/steam-library") == vdf);
  }

  SECTION("Steam's own file keeps what it has") {
    std::string existing = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/retro/.local/share/Steam\"\n"
                           "\t\t\"apps\"\n\t\t{\n\t\t\t\"228980\"\t\t\"123\"\n\t\t}\n\t}\n"
                           "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/other\"\n\t}\n}\n";
    auto vdf = state::add_library_folder(existing, "/steam-library");
    REQUIRE(vdf.find("\"/other\"") != std::string::npos);
    REQUIRE(vdf.find("\"228980\"") != std::string::npos);
    // The app id inside "apps" must not be mistaken for a library index
    REQUIRE(vdf.find("\t\"2\"\n\t{\n\t\t\"path\"\t\t\"/steam-library\"") != std::string::npos);
    REQUIRE(vdf.rfind('}') == vdf.size() - 2);
    REQUIRE(state::add_library_folder(vdf, "/steam-library") == vdf);
  }

  SECTION("Garbage is replaced by a fresh file") {
    auto vdf = state::add_library_folder("not a vdf", "/steam-library");
    REQUIRE(vdf.find("\"libraryfolders\"") != std::string::npos);
    REQUIRE(vdf.find("/steam-library") != std::string::npos);
  }
}
