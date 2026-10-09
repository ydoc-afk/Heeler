#pragma once

#include <state/data-structures.hpp>
#include <string>

namespace state {

/** PBKDF2-HMAC-SHA256 with a random salt. Returns "pbkdf2-sha256$iterations$salt$hash" (hex). */
std::string hash_password(const std::string &password);

/** Constant-time check of a password against a hash made by hash_password. False for anything malformed. */
bool verify_password(const std::string &password, const std::string &stored_hash);

/** A random code like "K7QM-2XPD" (no look-alike characters) */
std::string make_setup_code();

/** Upper-cases the code and drops dashes and spaces, so "k7qm 2xpd" matches "K7QM-2XPD" */
std::string normalize_setup_code(const std::string &code);

/** Constant-time comparison of two strings */
bool secure_equals(const std::string &a, const std::string &b);

/**
 * Called once the config is loaded: picks the admin account (the first account, for installs from before this
 * existed), honours a "reset-admin-password" file next to config.toml, and, while there is no password, creates the
 * setup code and writes it to "setup-code.txt" next to config.toml and to the log.
 */
void init_admin(const Config &cfg);

/** Stores the admin state in memory and in config.toml */
void update_admin(const Config &cfg, const AdminState &admin);

/** The profile that administers Heeler, if any account exists yet */
std::optional<std::string> admin_profile_id(const Config &cfg);

enum class Attempt {
  Allowed,
  Locked
};

/** Whether a guess may be checked now. Seconds left of the lock go to `retry_after` when locked. */
Attempt admin_attempt_allowed(const Config &cfg, long &retry_after);

/** Records a guess: a failure counts towards a growing lock-out, a success clears it */
void admin_attempt_result(const Config &cfg, bool success);

} // namespace state
