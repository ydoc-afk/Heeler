#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace state {

/// Where the shared Steam library is mounted inside every Steam container
constexpr std::string_view STEAM_LIBRARY_MOUNT = "/steam-library";

/// Keeps the Proton prefixes (compatdata) per account: this folder of the account's state is mounted over the
/// library's own steamapps/compatdata, so game files are shared but saves kept in a prefix are not
constexpr std::string_view STEAM_COMPATDATA_STATE_DIR = "steam-compatdata";

/**
 * A usable HEELER_STEAM_LIBRARY value: absolute (Heeler doesn't expand ~ or $HOME in mounts), not the root folder,
 * no trailing slash. Returns nullopt for anything else.
 */
std::optional<std::string> normalize_library_path(std::string_view path);

/// The shared library from HEELER_STEAM_LIBRARY, if set and valid (an invalid value is logged and ignored)
std::optional<std::string> steam_library_from_env();

/// True for the Steam app image (the library only makes sense there)
bool is_steam_image(std::string_view image);

/**
 * Returns Steam's libraryfolders.vdf with `path` added as a library folder. Empty input gives a fresh file that also
 * lists Steam's own folder; text that already has the path is returned unchanged. Steam keeps the entry
 * (and any other it finds) when it rewrites the file.
 */
std::string add_library_folder(std::string_view vdf, std::string_view path);

} // namespace state
