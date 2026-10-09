#include <core/docker.hpp>
#include <fstream>
#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <platforms/hw.hpp>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>
#include <sstream>
#include <state/steam-library.hpp>
#include <unistd.h>

namespace wolf::core::sessions {

/**
 * Docker creates a missing mount point as root, and the library's steamapps folder is the parent of the per-account
 * compatdata mount. Left to Docker, Steam (running as the app user) couldn't install anything into it, so a short
 * lived container makes the folder first.
 */
static bool prepare_library_folder(
    const std::string &library, const std::string &image, uint uid, uint gid, const std::string &session_id) {
  auto docker_socket = utils::get_env("HEELER_DOCKER_SOCKET", "/var/run/docker.sock");
  docker::DockerAPI api(docker_socket);
  auto script = fmt::format("mkdir -p /library/steamapps && chown {}:{} /library/steamapps", uid, gid);
  auto options =
      boost::json::serialize(boost::json::object{{"Entrypoint", boost::json::array{"/bin/sh", "-c", script}}});
  docker::Container helper = {
      .id = "",
      .name = fmt::format("heeler-steam-library-prep-{}", session_id),
      .image = image,
      .status = docker::CREATED,
      .mounts = {docker::MountPoint{.source = library, .destination = "/library", .mode = "rw"}}};
  auto created = api.create(helper, options);
  if (!created) {
    return false;
  }
  api.start_by_id(created->id);
  bool done = false;
  for (int i = 0; i < 100 && !done; ++i) {
    auto state = api.get_by_id(created->id);
    done = !state || state->status == docker::EXITED;
    if (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }
  api.remove_by_id(created->id, false, true);
  return done;
}

/**
 * Mounts the shared Steam library (HEELER_STEAM_LIBRARY) into a Steam container and lists it in the account's
 * libraryfolders.vdf, so every account installs into, and plays from, the same folder.
 * The Proton prefixes (compatdata) stay per account.
 */
static void mount_steam_library(const std::string &library,
                                const std::string &image,
                                const RunnerArgs &args,
                                immer::array_transient<std::pair<std::string, std::string>> &mounted_paths) {
  namespace fs = std::filesystem;
  const auto uid = static_cast<uid_t>(args.client_settings->run_uid);
  const auto gid = static_cast<gid_t>(args.client_settings->run_gid);
  const auto mount = std::string{state::STEAM_LIBRARY_MOUNT};
  std::error_code ec;

  // Saves that live in a Proton prefix must not be shared between accounts
  auto compatdata_local = fs::path(args.app_local_state_folder) / state::STEAM_COMPATDATA_STATE_DIR;
  fs::create_directories(compatdata_local, ec);
  if (ec || chown(compatdata_local.c_str(), uid, gid) != 0) {
    logs::log(logs::warning,
              "[STEAM_LIBRARY] Can't prepare {}, not using the shared library",
              compatdata_local.string());
    return;
  }

  // Tell Steam about the library. Folders are created one by one because only $HOME itself is handed to the user.
  auto dir = fs::path(args.app_local_state_folder);
  for (const char *part : {".local", "share", "Steam", "config"}) {
    dir /= part;
    if (!fs::exists(dir, ec)) {
      fs::create_directory(dir, ec);
      if (chown(dir.c_str(), uid, gid) != 0) {
        logs::log(logs::warning, "[STEAM_LIBRARY] Can't hand {} to the user", dir.string());
      }
    }
  }
  auto vdf_path = dir / "libraryfolders.vdf";
  std::string vdf;
  if (std::ifstream in{vdf_path}) {
    std::stringstream buffer;
    buffer << in.rdbuf();
    vdf = buffer.str();
  }
  auto updated = state::add_library_folder(vdf, mount);
  if (updated != vdf) {
    std::ofstream{vdf_path, std::ios::trunc} << updated;
    if (chown(vdf_path.c_str(), uid, gid) != 0) {
      logs::log(logs::warning, "[STEAM_LIBRARY] Can't hand {} to the user", vdf_path.string());
    }
  }

  if (!prepare_library_folder(library, image, uid, gid, args.session_id)) {
    logs::log(logs::warning,
              "[STEAM_LIBRARY] Couldn't prepare {}/steamapps, Steam may not be able to install games there",
              library);
  }

  mounted_paths.push_back({library, mount});
  mounted_paths.push_back({(fs::path(args.app_host_state_folder) / state::STEAM_COMPATDATA_STATE_DIR).string(),
                           mount + "/steamapps/compatdata"});
  logs::log(logs::info, "[STEAM_LIBRARY] Using {} as the shared Steam library", library);
}

void start_runner(std::shared_ptr<events::Runner> runner,
                  std::shared_ptr<events::devices_atom_queue> plugged_devices_queue,
                  immer::box<RunnerArgs> args) {
  /* Setup devices paths */
  auto all_devices = immer::array_transient<std::string>();

  /* Setup mounted paths */
  immer::array_transient<std::pair<std::string, std::string>> mounted_paths;

  /* Setup environment paths */
  immer::map_transient<std::string, std::string> full_env;
  full_env.set("XDG_RUNTIME_DIR", args->xdg_runtime_dir);
  full_env.set("WOLF_SESSION_ID", args->session_id);

  if (args->audio_server && args->audio_server->server) {
    auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, args->session_id);
    auto audio_server_name = audio::get_server_name(args->audio_server->server);
    // TODO: properly separate XDG_RUNTIME_DIR from <pulse socket path>
    // for example on my dev machine it's ${XDG_RUNTIME_DIR}/pulse/native
    // but we know that on our images it's ${XDG_RUNTIME_DIR}/pulse-socket so this should be fine..
    auto audio_server_on_host = std::filesystem::path(args->host->host_xdg_runtime_dir) /
                                std::filesystem::path(audio_server_name).filename();
    full_env.set("PULSE_SINK", pulse_sink_name);
    full_env.set("PULSE_SOURCE", pulse_sink_name + ".monitor");
    full_env.set("PULSE_SERVER", audio_server_name);
    mounted_paths.push_back({audio_server_on_host, audio_server_name});
  } else {
    // Without a server there is no socket to mount: an empty mount destination
    // makes the Docker API reject the container creation
    logs::log(logs::warning, "[STREAM_SESSION] No audio server, container will run without audio");
  }

  full_env.set("GAMESCOPE_WIDTH", std::to_string(args->video_settings.width));
  full_env.set("GAMESCOPE_HEIGHT", std::to_string(args->video_settings.height));
  full_env.set("GAMESCOPE_REFRESH", std::to_string(args->video_settings.refresh_rate));
  full_env.set("WOLF_VIDEO_BUFFER_CAPS", args->video_settings.video_producer_buffer_caps);

  if (auto w_display = args->wayland_display.get()) {
    auto socket_name = virtual_display::get_wayland_socket_name(*w_display);
    auto local_wayland_socket = std::filesystem::path(args->xdg_runtime_dir) / socket_name;
    auto host_wayland_socket = std::filesystem::path(args->host->host_xdg_runtime_dir) / socket_name;
    mounted_paths.push_back({host_wayland_socket, local_wayland_socket});
    full_env.set("WAYLAND_DISPLAY", socket_name);
  }

  /* Adding custom state folder */
  mounted_paths.push_back({args->app_host_state_folder, "/home/retro"});

  /* Shared Steam library, for the Steam app only */
  if (auto library = state::steam_library_from_env()) {
    auto serialized = runner->serialize();
    if (rfl::holds_alternative<wolf::config::AppDocker>(serialized.variant())) {
      auto image = rfl::get<wolf::config::AppDocker>(serialized.variant()).image;
      if (state::is_steam_image(image)) {
        mount_steam_library(*library, image, *args, mounted_paths);
      }
    }
  }

  /* GPU specific adjustments */
  auto render_node = args->video_settings.runner_render_node;
  auto additional_devices = linked_devices(render_node);
  std::copy(additional_devices.begin(), additional_devices.end(), std::back_inserter(all_devices));

  auto gpu_vendor = get_vendor(render_node);
  if (gpu_vendor == NVIDIA) {
    if (auto driver_volume = utils::get_env("NVIDIA_DRIVER_VOLUME_NAME")) {
      logs::log(logs::info, "Mounting nvidia driver {}:/usr/nvidia", driver_volume);
      mounted_paths.push_back({driver_volume, "/usr/nvidia"});
    }
  } else if (gpu_vendor == INTEL) {
    full_env.set("INTEL_DEBUG", "norbc"); // see: https://github.com/games-on-whales/wolf/issues/50
  }

  full_env.set("PUID", std::to_string(args->client_settings->run_uid));
  full_env.set("PGID", std::to_string(args->client_settings->run_gid));

  // Add fake-udev and udev mounts
  mounted_paths.push_back(
      {std::filesystem::path(args->host->host_base_state_folder) / "fake-udev", "/usr/bin/fake-udev"});
  mounted_paths.push_back({std::filesystem::path(args->app_host_state_folder) / "udev", "/run/udev/"});

  /* Finally run the app, this will stop here until over */
  runner->run(args->session_id,
              args->app_local_state_folder,
              args->host->host_xdg_runtime_dir,
              plugged_devices_queue,
              all_devices.persistent(),
              mounted_paths.persistent(),
              full_env.persistent(),
              render_node);

  if (args->audio_server && args->audio_sink) {
    logs::log(logs::debug, "[STREAM_SESSION] Remove virtual audio sink");
    audio::delete_virtual_sink(args->audio_server->server, args->audio_sink);
  }
}

} // namespace wolf::core::sessions
