#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <events/events.hpp>
#include <filesystem>
#include <fstream>
#include <helpers/logger.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <rfl/toml.hpp>
#include <state/admin.hpp>
#include <state/serialised_config.hpp>

namespace state {

constexpr int PBKDF2_ITERATIONS = 600000;
constexpr size_t SALT_BYTES = 16;
constexpr size_t HASH_BYTES = 32;

// No 0/O, 1/I: the code is read off a screen and typed in
constexpr std::string_view CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

static std::string to_hex(const unsigned char *data, size_t len) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0f]);
  }
  return out;
}

static std::optional<std::vector<unsigned char>> from_hex(std::string_view hex) {
  if (hex.size() % 2 != 0)
    return std::nullopt;
  std::vector<unsigned char> out;
  for (size_t i = 0; i < hex.size(); i += 2) {
    unsigned int byte = 0;
    auto res = std::from_chars(hex.data() + i, hex.data() + i + 2, byte, 16);
    if (res.ec != std::errc() || res.ptr != hex.data() + i + 2)
      return std::nullopt;
    out.push_back(static_cast<unsigned char>(byte));
  }
  return out;
}

static std::optional<std::vector<unsigned char>>
pbkdf2(const std::string &password, const std::vector<unsigned char> &salt, int iterations) {
  std::vector<unsigned char> out(HASH_BYTES);
  if (PKCS5_PBKDF2_HMAC(password.data(),
                        static_cast<int>(password.size()),
                        salt.data(),
                        static_cast<int>(salt.size()),
                        iterations,
                        EVP_sha256(),
                        static_cast<int>(out.size()),
                        out.data()) != 1)
    return std::nullopt;
  return out;
}

std::string hash_password(const std::string &password) {
  std::vector<unsigned char> salt(SALT_BYTES);
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
    return "";
  auto hash = pbkdf2(password, salt, PBKDF2_ITERATIONS);
  if (!hash)
    return "";
  return fmt::format("pbkdf2-sha256${}${}${}",
                     PBKDF2_ITERATIONS,
                     to_hex(salt.data(), salt.size()),
                     to_hex(hash->data(), hash->size()));
}

bool verify_password(const std::string &password, const std::string &stored_hash) {
  // pbkdf2-sha256$iterations$salt$hash
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t pos; (pos = stored_hash.find('$', start)) != std::string::npos; start = pos + 1)
    parts.push_back(stored_hash.substr(start, pos - start));
  parts.push_back(stored_hash.substr(start));
  if (parts.size() != 4 || parts[0] != "pbkdf2-sha256")
    return false;

  int iterations = 0;
  auto res = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), iterations);
  // A hand-edited config must not be able to make a login take forever
  if (res.ec != std::errc() || iterations < 1000 || iterations > 5000000)
    return false;

  auto salt = from_hex(parts[2]);
  auto expected = from_hex(parts[3]);
  if (!salt || !expected || salt->empty() || expected->size() != HASH_BYTES)
    return false;

  auto actual = pbkdf2(password, *salt, iterations);
  return actual && CRYPTO_memcmp(actual->data(), expected->data(), HASH_BYTES) == 0;
}

std::string make_setup_code() {
  std::array<unsigned char, 8> random{};
  RAND_bytes(random.data(), static_cast<int>(random.size()));
  std::string code;
  for (size_t i = 0; i < random.size(); i++) {
    if (i == 4)
      code.push_back('-');
    code.push_back(CODE_ALPHABET[random[i] & 31]); // 32 symbols: no modulo bias
  }
  return code;
}

std::string normalize_setup_code(const std::string &code) {
  std::string out;
  for (char c : code)
    if (c != '-' && c != ' ')
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  return out;
}

bool secure_equals(const std::string &a, const std::string &b) {
  return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

static std::filesystem::path config_dir(const Config &cfg) {
  auto dir = std::filesystem::path(cfg.config_source).parent_path();
  return dir.empty() ? std::filesystem::path(".") : dir;
}

std::optional<std::string> admin_profile_id(const Config &cfg) {
  return cfg.admin->load()->profile_id;
}

void update_admin(const Config &cfg, const AdminState &admin) {
  cfg.admin->store(admin);

  auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>(cfg.config_source).value();
  tml.admin = wolf::config::AdminConfig{.profile_id = admin.profile_id, .password_hash = admin.password_hash};
  rfl::toml::save(cfg.config_source, tml);
}

void init_admin(const Config &cfg) {
  auto dir = config_dir(cfg);
  auto code_file = dir / "setup-code.txt";
  auto reset_file = dir / "reset-admin-password";

  // The tml's [admin] section, if it has one
  auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>(cfg.config_source).value();
  AdminState admin{.profile_id = tml.admin ? tml.admin->profile_id : std::nullopt,
                   .password_hash = tml.admin ? tml.admin->password_hash : std::nullopt};

  // Only a password reset is written back at start. The admin account picked below is worked out again on every
  // start, so config.toml is not rewritten (and its comments lost) just by starting Heeler.
  auto persist = false;

  // Installs from before the admin existed: the first account is the admin
  auto profiles = cfg.profiles->load();
  auto exists = [&](const std::string &id) {
    return std::any_of(profiles->begin(), profiles->end(), [&](const immer::box<events::Profile> &p) {
      return p->id == id;
    });
  };
  if (admin.profile_id && !exists(*admin.profile_id)) {
    logs::log(logs::warning, "The admin account '{}' no longer exists", *admin.profile_id);
    admin.profile_id = std::nullopt;
  }
  if (!admin.profile_id) {
    auto first = std::find_if(profiles->begin(), profiles->end(), [](const immer::box<events::Profile> &p) {
      return p->id != events::MOONLIGHT_PROFILE_ID && p->id != events::TEMPLATE_PROFILE_ID;
    });
    if (first != profiles->end()) {
      admin.profile_id = (*first)->id;
      logs::log(logs::info, "The account '{}' is the admin", (*first)->id);
    }
  }

  std::error_code ec;
  if (std::filesystem::exists(reset_file, ec)) {
    std::filesystem::remove(reset_file, ec);
    if (admin.password_hash) {
      admin.password_hash = std::nullopt;
      persist = true;
      logs::log(logs::warning, "The web admin password was reset ({} was found)", reset_file.string());
    }
  }

  if (persist)
    update_admin(cfg, admin);
  else
    cfg.admin->store(admin);

  if (admin.password_hash) {
    cfg.setup_code->store("");
    std::filesystem::remove(code_file, ec);
    return;
  }

  auto code = make_setup_code();
  cfg.setup_code->store(normalize_setup_code(code));
  {
    std::ofstream out(code_file, std::ios::trunc);
    out << code << "\n";
  }
  std::filesystem::permissions(code_file,
                               std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace,
                               ec);
  logs::log(logs::warning,
            "No web admin password is set. Setup code: {} (also in {}). It stops working once the password is set.",
            code,
            code_file.string());
}

Attempt admin_attempt_allowed(const Config &cfg, long &retry_after) {
  auto limiter = cfg.admin_limiter->load();
  auto now = std::chrono::steady_clock::now();
  if (limiter->locked_until > now) {
    retry_after = std::chrono::duration_cast<std::chrono::seconds>(limiter->locked_until - now).count() + 1;
    return Attempt::Locked;
  }
  return Attempt::Allowed;
}

void admin_attempt_result(const Config &cfg, bool success) {
  cfg.admin_limiter->update([&](AdminLimiter l) {
    if (success)
      return AdminLimiter{};
    l.failures++;
    // Five free guesses, then 30 s, 60 s, 120 s ... up to 15 minutes
    if (l.failures >= 5) {
      auto shift = std::min(l.failures - 5, 5);
      auto wait = std::min(30 * (1 << shift), 15 * 60);
      l.locked_until = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
    }
    return l;
  });
}

} // namespace state
