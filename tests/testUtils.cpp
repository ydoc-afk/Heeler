#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

#include <boost/asio.hpp>
#include <filesystem>
#include <fstream>
#include <helpers/utils.hpp>
#include <rest/pairing_webhook.hpp>
#include <sessions/common.hpp>
#include <state/utils.hpp>
#include <thread>

TEST_CASE("get_env reads a set variable", "[utils]") {
  setenv("HEELER_UTILS_TEST_PLAIN", "plain-value", 1);
  REQUIRE(std::string(utils::get_env("HEELER_UTILS_TEST_PLAIN")) == "plain-value");
  unsetenv("HEELER_UTILS_TEST_PLAIN");
}

TEST_CASE("get_env returns the default (or null) for a missing variable", "[utils]") {
  unsetenv("HEELER_UTILS_TEST_MISSING");
  unsetenv("HEALER_UTILS_TEST_MISSING");
  unsetenv("WOLF_UTILS_TEST_MISSING");
  REQUIRE(std::string(utils::get_env("HEELER_UTILS_TEST_MISSING", "fallback")) == "fallback");
  REQUIRE(utils::get_env("HEELER_UTILS_TEST_MISSING") == nullptr);
}

TEST_CASE("get_env HEELER_ name falls back to the old HEALER_ spelling", "[utils]") {
  unsetenv("HEELER_UTILS_TEST_TYPO");
  unsetenv("WOLF_UTILS_TEST_TYPO");
  setenv("HEALER_UTILS_TEST_TYPO", "typo-value", 1);
  REQUIRE(std::string(utils::get_env("HEELER_UTILS_TEST_TYPO")) == "typo-value");
  unsetenv("HEALER_UTILS_TEST_TYPO");
}

TEST_CASE("get_env HEELER_ name falls back to legacy WOLF_ name", "[utils]") {
  unsetenv("HEELER_UTILS_TEST_LEGACY");
  unsetenv("HEALER_UTILS_TEST_LEGACY");
  setenv("WOLF_UTILS_TEST_LEGACY", "legacy-value", 1);
  REQUIRE(std::string(utils::get_env("HEELER_UTILS_TEST_LEGACY")) == "legacy-value");
  unsetenv("WOLF_UTILS_TEST_LEGACY");
}

TEST_CASE("get_env order is HEELER_, then HEALER_, then WOLF_", "[utils]") {
  setenv("WOLF_UTILS_TEST_BOTH", "wolf-value", 1);
  setenv("HEALER_UTILS_TEST_BOTH", "healer-value", 1);
  setenv("HEELER_UTILS_TEST_BOTH", "heeler-value", 1);
  REQUIRE(std::string(utils::get_env("HEELER_UTILS_TEST_BOTH")) == "heeler-value");
  unsetenv("HEELER_UTILS_TEST_BOTH");
  REQUIRE(std::string(utils::get_env("HEELER_UTILS_TEST_BOTH")) == "healer-value");
  unsetenv("HEALER_UTILS_TEST_BOTH");
  REQUIRE(std::string(utils::get_env("HEELER_UTILS_TEST_BOTH")) == "wolf-value");
  unsetenv("WOLF_UTILS_TEST_BOTH");
}

TEST_CASE("get_env still accepts a HEALER_ name in code and aliases it", "[utils]") {
  unsetenv("HEALER_UTILS_TEST_OLD");
  setenv("HEELER_UTILS_TEST_OLD", "new-value", 1);
  REQUIRE(std::string(utils::get_env("HEALER_UTILS_TEST_OLD")) == "new-value");
  unsetenv("HEELER_UTILS_TEST_OLD");
}

TEST_CASE("get_env has no legacy fallback for other names", "[utils]") {
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

TEST_CASE("Pairing webhook payload", "[PAIRING]") {
  auto payload = pairing_webhook::make_payload("10.0.0.5", "steamdeck", "http://10.0.0.1:47989/pin/#abc");
  auto parsed = rfl::json::read<pairing_webhook::Payload>(payload).value();
  REQUIRE(parsed.client_ip == "10.0.0.5");
  REQUIRE(parsed.hostname == "steamdeck");
  REQUIRE(parsed.pin_url == "http://10.0.0.1:47989/pin/#abc");
  // Slack reads `text`, Discord reads `content`
  REQUIRE(parsed.text == "Heeler: steamdeck (10.0.0.5) wants to pair, enter its PIN at http://10.0.0.1:47989/pin/#abc");
  REQUIRE(parsed.content == parsed.text);
  // Without reverse DNS the IP is used on its own
  REQUIRE(rfl::json::read<pairing_webhook::Payload>(pairing_webhook::make_payload("10.0.0.5", "", "u")).value().text ==
          "Heeler: 10.0.0.5 wants to pair, enter its PIN at u");
}

TEST_CASE("Pairing webhook POSTs the payload", "[PAIRING]") {
  // A one-shot HTTP server that records the request
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  auto port = acceptor.local_endpoint().port();
  std::string received;
  std::thread server([&]() {
    auto socket = acceptor.accept();
    std::string buffer(8192, '\0');
    std::size_t total = 0;
    // Read until we have the headers and the whole body
    while (true) {
      total += socket.read_some(boost::asio::buffer(buffer.data() + total, buffer.size() - total));
      auto header_end = buffer.find("\r\n\r\n");
      if (header_end != std::string::npos && header_end < total) {
        auto cl = buffer.find("Content-Length: ");
        auto length = std::stoul(buffer.substr(cl + 16, buffer.find("\r\n", cl) - cl - 16));
        if (total >= header_end + 4 + length) {
          break;
        }
      }
    }
    received = buffer.substr(0, total);
    std::string reply = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(reply));
  });

  auto payload = pairing_webhook::make_payload("10.0.0.5", "", "http://host/pin/#s");
  auto status = pairing_webhook::post(fmt::format("http://127.0.0.1:{}/hook", port), payload);
  server.join();

  REQUIRE(status == 204);
  REQUIRE(received.starts_with("POST /hook HTTP/1.1\r\n"));
  REQUIRE(received.find("Content-Type: application/json") != std::string::npos);
  REQUIRE(received.ends_with(payload));

  // An unreachable webhook is reported, not thrown
  REQUIRE(pairing_webhook::post("http://127.0.0.1:1/hook", payload) == 0);
}

TEST_CASE("pointer_input_nodes picks the nodes the compositor reads", "[utils]") {
  std::vector<std::map<std::string, std::string>> pen = {
      {{"DEVNAME", "/dev/input/event20"}, {"ID_INPUT_TABLET", "1"}},
      {{"DEVNAME", "/dev/input/mouse5"}, {"ID_INPUT_TABLET", "1"}}, // legacy mousedev node: not for libinput
  };
  REQUIRE(wolf::core::sessions::pointer_input_nodes(pen) == std::vector<std::string>{"/dev/input/event20"});

  std::vector<std::map<std::string, std::string>> dualsense = {
      {{"DEVNAME", "/dev/input/event21"}, {"ID_INPUT_JOYSTICK", "1"}},
      {{"DEVNAME", "/dev/input/event22"}, {"ID_INPUT_TOUCHPAD", "1"}},
      {{"DEVNAME", "/dev/hidraw3"}},
      {{"ID_INPUT_TOUCHSCREEN", "1"}}, // no DEVNAME
  };
  REQUIRE(wolf::core::sessions::pointer_input_nodes(dualsense) == std::vector<std::string>{"/dev/input/event22"});

  REQUIRE(wolf::core::sessions::pointer_input_nodes({}).empty());
}
