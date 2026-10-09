#include <algorithm>
#include <api/api.hpp>
#include <control/input_handler.hpp>
#include <core/docker.hpp>
#include <rtp/udp-ping.hpp>
#include <state/admin.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>

namespace wolf::api {

void UnixSocketServer::endpoint_Events(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // curl -N --unix-socket /tmp/wolf.sock http://localhost/api/v1/events
  {
    // Runs on the request thread pool while the io thread iterates/erases the vector
    std::lock_guard lock(state_->sockets_mutex);
    state_->sockets.push_back(socket);
  }
  send_http(socket,
            200,
            {{"Content-Type: text/event-stream"}, {"Connection: keep-alive"}, {"Cache-Control: no-cache"}},
            ""); // Inform clients this is going to be SS
}

void UnixSocketServer::endpoint_Health(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  const auto &app_state = state_->app_state;
  auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - state_->started_at);
  auto res = HealthResponse{
      .uptime_seconds = static_cast<long>(uptime.count()),
      .paired_clients = app_state->config->paired_clients ? app_state->config->paired_clients->load()->size() : 0,
      .pending_pair_requests = app_state->pairing_atom ? app_state->pairing_atom->load()->size() : 0,
      .running_sessions = app_state->running_sessions ? app_state->running_sessions->load()->size() : 0,
      .lobbies = app_state->lobbies ? app_state->lobbies->load()->size() : 0,
  };
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_PendingPairRequest(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto requests = std::vector<PendingPairClient>();
  for (auto [secret, pair_request] : *(state_->app_state)->pairing_atom->load()) {
    requests.push_back({.pair_secret = secret, .client_ip = pair_request->client_ip});
  }
  send_http(socket, 200, rfl::json::write(PendingPairRequestsResponse{.requests = requests}));
}

void UnixSocketServer::endpoint_Pair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<PairRequest>(req.body);
  if (event) {
    if (auto pair_request = state_->app_state->pairing_atom->load()->find(event.value().pair_secret)) {
      pair_request->get().user_pin->set_value(event.value().pin.value()); // Resolve the promise
      state_->app_state->pairing_atom->update(
          [pair_secret = event.value().pair_secret](auto pairing_map) { return pairing_map.erase(pair_secret); });
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid pair secret: {}", event.value().pair_secret);
      auto res = GenericErrorResponse{.error = "Invalid pair secret"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_PairedClients(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = PairedClientsResponse{.success = true};
  auto clients = state_->app_state->config->paired_clients->load();
  for (const config::PairedClient &client : clients.get()) {
    res.clients.push_back(PairedClient{.client_id = std::to_string(state::get_client_id(client)),
                                       .app_state_folder = client.app_state_folder,
                                       .settings = client.settings});
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_UnpairClient(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  try {
    auto payload_result = rfl::json::read<UnpairClientRequest>(req.body);
    if (!payload_result) {
      auto res = GenericErrorResponse{.error = "Invalid request format"};
      send_http(socket, 400, rfl::json::write(res));
      return;
    }

    const auto &payload = payload_result.value(); // Unwrap the Result
    auto client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
    if (!client) {
      auto res = GenericErrorResponse{.error = "Client not found"};
      send_http(socket, 404, rfl::json::write(res));
      return;
    }

    state::unpair(this->state_->app_state->config, *client);

    auto res = GenericSuccessResponse{.success = true};
    send_http(socket, 200, rfl::json::write(res));
  } catch (const std::exception &e) {
    auto res = GenericErrorResponse{.error = e.what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Apps(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = AppListResponse{.success = true};
  auto moonlight_profile = state::get_moonlight_profile(state_->app_state->config);
  if (!moonlight_profile) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Moonlight profile not found"}));
    return;
  }
  immer::vector<immer::box<events::App>> app_list = moonlight_profile.value()->apps->load();
  for (const immer::box<events::App> &app : app_list) {
    res.apps.push_back(rfl::Reflector<events::App>::from(app));
  }
  send_http(socket, 200, rfl::json::write(res));
}

/**
 * Apps added over the API don't come with the Wayland producer caps that parse_apps() computes for the ones in
 * config.toml; without them the producer pipeline can't negotiate and no frame is ever produced (#489).
 * Reuse the caps of an app that encodes on the same GPU, or the ones picked for the default encoder.
 */
static events::App
with_producer_caps(events::App app, const state::Config &config, const state::ProfilesList &profiles) {
  if (!app.video_producer_buffer_caps.empty()) {
    return app;
  }
  app.video_producer_buffer_caps = config.default_video_producer_buffer_caps;
  for (const auto &profile : profiles) {
    for (const auto &existing : profile->apps->load().get()) {
      if (existing->encoder_render_node == app.encoder_render_node && !existing->video_producer_buffer_caps.empty()) {
        app.video_producer_buffer_caps = existing->video_producer_buffer_caps;
        return app;
      }
    }
  }
  return app;
}

void UnixSocketServer::endpoint_AddApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<rfl::Reflector<events::App>::ReflType>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    auto new_app = with_producer_caps(rfl::Reflector<events::App>::to(app.value(), state_->app_state->event_bus),
                                      state_->app_state->config.get(),
                                      profiles);
    state::update_profiles(state_->app_state->config,
                           profiles | //
                               ranges::views::transform([new_app](const immer::box<events::Profile> &profile) {
                                 if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                                   profile->apps->update([new_app](auto &apps) { return apps.push_back(new_app); });
                                 }
                                 return profile;
                               }) |
                               ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<AppDeleteRequest>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app = app.value(), this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app, this](auto &apps) {
                  return apps | //
                         ranges::views::filter(
                             [&app](const immer::box<events::App> &a) { return a->base.id != app.id; }) | //
                         ranges::to<immer::vector<immer::box<events::App>>>();
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Profiles(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profiles = state_->app_state->config->profiles->load().get();
  auto res = ProfileListResponse{.success = true,
                                 .profiles = profiles | //
                                             ranges::views::filter([](const immer::box<events::Profile> &p) {
                                               return p->id != events::MOONLIGHT_PROFILE_ID &&
                                                      p->id != events::TEMPLATE_PROFILE_ID;
                                             }) |                                                              //
                                             ranges::views::transform(rfl::Reflector<events::Profile>::from) | //
                                             ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<rfl::Reflector<events::Profile>::ReflType>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();

    auto reserved = p.id == events::MOONLIGHT_PROFILE_ID || p.id == events::TEMPLATE_PROFILE_ID;
    auto taken = std::any_of(profiles.begin(), profiles.end(), [&](const immer::box<events::Profile> &e) {
      return e->id == p.id;
    });
    if (p.id.empty() || p.name.empty() || reserved || taken) {
      logs::log(logs::warning, "[API] Rejected profile with id '{}' and name '{}'", p.id, p.name);
      auto res = GenericErrorResponse{.error = "A profile needs a name and an unused id"};
      send_http(socket, 400, rfl::json::write(res));
      return;
    }

    auto new_profile = rfl::Reflector<events::Profile>::to(p, this->state_->app_state->event_bus);
    if (new_profile.apps->load()->empty()) {
      // A new account with no apps of its own starts with the template's apps, or else a copy of the first
      // account's (installs from before the template existed)
      auto source = std::find_if(profiles.begin(), profiles.end(), [](const immer::box<events::Profile> &e) {
        return e->id == events::TEMPLATE_PROFILE_ID;
      });
      if (source == profiles.end())
        source = std::find_if(profiles.begin(), profiles.end(), [](const immer::box<events::Profile> &e) {
          return e->id != events::MOONLIGHT_PROFILE_ID;
        });
      if (source != profiles.end()) {
        auto source_apps = (*source)->apps->load();
        new_profile.apps->update([&](const auto &) { return source_apps.get(); });
      }
    }
    new_profile.apps->update([&](const auto &apps) {
      return apps | //
             ranges::views::transform([&](const immer::box<events::App> &app) {
               return immer::box<events::App>(with_producer_caps(app.get(), state_->app_state->config.get(), profiles));
             }) |
             ranges::to<immer::vector<immer::box<events::App>>>();
    });
    state::update_profiles(state_->app_state->config, profiles.push_back(new_profile));

    // The first account that is created administers Heeler
    auto admin = state_->app_state->config->admin->load().get();
    if (!admin.profile_id) {
      admin.profile_id = p.id;
      state::update_admin(state_->app_state->config, admin);
      logs::log(logs::info, "[API] The account '{}' is now the admin", p.id);
    }
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<ProfileRemoveRequest>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    if (state::admin_profile_id(*state_->app_state->config) == p.id) {
      auto res = GenericErrorResponse{.error = "The admin account can't be removed"};
      send_http(socket, 409, rfl::json::write(res));
      return;
    }

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(state_->app_state->config,
                           profiles | //
                               ranges::views::remove_if([&p](const immer::box<events::Profile> &profile) {
                                 return profile.get().id == p.id;
                               }) | //
                               ranges::to<state::ProfilesList>());
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_AdminStatus(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  const auto &cfg = *state_->app_state->config;
  auto admin = cfg.admin->load();
  auto res = AdminStatusResponse{.admin_profile_id = admin->profile_id,
                                 .password_set = admin->password_hash.has_value(),
                                 .setup_required = !admin->password_hash.has_value() && admin->profile_id.has_value()};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AdminSetup(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto request = rfl::json::read<AdminSetupRequest>(req.body);
  if (!request) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid request"}));
    return;
  }
  const auto &cfg = *state_->app_state->config;
  auto admin = cfg.admin->load().get();

  if (admin.password_hash) {
    send_http(socket, 409, rfl::json::write(GenericErrorResponse{.error = "The password is already set"}));
    return;
  }
  if (!admin.profile_id) {
    send_http(socket, 409, rfl::json::write(GenericErrorResponse{.error = "Create the first account first"}));
    return;
  }

  long retry_after = 0;
  if (state::admin_attempt_allowed(cfg, retry_after) == state::Attempt::Locked) {
    send_http(socket,
              429,
              {{"Retry-After: " + std::to_string(retry_after)}},
              rfl::json::write(GenericErrorResponse{.error = "Too many wrong tries, wait and try again"}));
    return;
  }

  auto code = cfg.setup_code->load().get();
  if (code.empty() || !state::secure_equals(code, state::normalize_setup_code(request.value().setup_code.value()))) {
    state::admin_attempt_result(cfg, false);
    logs::log(logs::warning, "[API] Wrong setup code");
    send_http(socket, 403, rfl::json::write(GenericErrorResponse{.error = "Wrong setup code"}));
    return;
  }

  auto password = request.value().password.value();
  if (password.size() < 8) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "The password needs at least 8 characters"}));
    return;
  }

  auto hash = state::hash_password(password);
  if (hash.empty()) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Couldn't hash the password"}));
    return;
  }

  admin.password_hash = hash;
  state::update_admin(cfg, admin);
  cfg.setup_code->store("");
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(cfg.config_source).parent_path() / "setup-code.txt", ec);
  state::admin_attempt_result(cfg, true);
  logs::log(logs::info, "[API] The web admin password was set");
  send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
}

void UnixSocketServer::endpoint_AdminLogin(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto request = rfl::json::read<AdminLoginRequest>(req.body);
  if (!request) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid request"}));
    return;
  }
  const auto &cfg = *state_->app_state->config;

  long retry_after = 0;
  if (state::admin_attempt_allowed(cfg, retry_after) == state::Attempt::Locked) {
    send_http(socket,
              429,
              {{"Retry-After: " + std::to_string(retry_after)}},
              rfl::json::write(GenericErrorResponse{.error = "Too many wrong tries, wait and try again"}));
    return;
  }

  auto hash = cfg.admin->load()->password_hash;
  auto ok = hash && state::verify_password(request.value().password, *hash);
  state::admin_attempt_result(cfg, ok);
  if (!ok) {
    logs::log(logs::warning, "[API] Wrong admin password");
    send_http(socket, 401, rfl::json::write(GenericErrorResponse{.error = "Wrong password"}));
    return;
  }
  send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
}

void UnixSocketServer::endpoint_StreamSessions(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = StreamSessionListResponse{.success = true};
  auto sessions = state_->app_state->running_sessions->load();
  for (const auto &session : sessions.get()) {
    res.sessions.push_back(rfl::Reflector<events::StreamSession>::from(session));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_StreamSessionAdd(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<rfl::Reflector<events::StreamSession>::ReflType>(req.body);
  if (session) {
    immer::box<events::App> choosen_app;
    auto ss = session.value();
    if (auto app_id = ss.app_id) {
      auto app = state::get_moonlight_app_by_id(this->state_->app_state->config, *app_id);
      if (!app) {
        logs::log(logs::warning, "[API] Invalid app_id: {}", *app_id);
        auto res = GenericErrorResponse{.error = "Invalid app_id"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      choosen_app = *app;
    } else {
      auto moonlight_profile = state::get_moonlight_profile(this->state_->app_state->config);
      if (!moonlight_profile) {
        logs::log(logs::warning, "[API] No moonlight profile found, unable to automatically create an app.");
        auto res = GenericErrorResponse{.error = "No moonlight profile found"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
      if (apps.empty()) {
        logs::log(logs::warning, "[API] No apps in the moonlight profile, unable to automatically create an app.");
        send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "No apps configured"}));
        return;
      }
      immer::box<events::App> sample_app = apps.front();
      choosen_app = events::App{
          .base = {.title = "dummy", .id = state::gen_uuid(), .support_hdr = false, .icon_png_path = ""},

          .video_producer_buffer_caps = sample_app->video_producer_buffer_caps,

          .h264_gst_pipeline = sample_app->h264_gst_pipeline,
          .hevc_gst_pipeline = sample_app->hevc_gst_pipeline,
          .av1_gst_pipeline = sample_app->av1_gst_pipeline,

          .render_node = sample_app->render_node,
          .encoder_render_node = sample_app->encoder_render_node,
          .opus_gst_pipeline = sample_app->opus_gst_pipeline,
          .start_virtual_compositor = true,
          .start_audio_server = true,

          .runner = std::make_shared<process::RunProcess>(state_->app_state->event_bus,
                                                          "sh -c \"while :; do echo 'running...'; sleep 10; done\"")};
    }

    config::PairedClient choosen_client;
    if (auto client_id = ss.client_id) {
      auto client = state::get_client_by_id(this->state_->app_state->config, *client_id);
      if (!client) {
        logs::log(logs::warning, "[API] Invalid client_id: {}", *client_id);
        auto res = GenericErrorResponse{.error = "Invalid client_id"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      choosen_client = *client;
    } else {
      // Create a dummy client
      choosen_client = {.client_cert = "", .app_state_folder = state::gen_uuid(), .settings = {}};
    }

    choosen_client.settings = ss.client_settings.value_or(config::ClientSettings{});

    auto new_session = state::create_stream_session( //
        state_->app_state,
        choosen_app,
        choosen_client,
        moonlight::DisplayMode{.width = ss.video_width,
                               .height = ss.video_height,
                               .refreshRate = ss.video_refresh_rate,
                               .hevc_supported = state_->app_state->config->support_hevc,
                               .av1_supported = state_->app_state->config->support_av1},
        ss.audio_channel_count,
        ss.aes_key,
        ss.aes_iv);
    new_session->ip = ss.client_ip;
    new_session->rtsp_fake_ip = ss.rtsp_fake_ip;

    state_->app_state->running_sessions->update(
        [new_session](const immer::vector<events::StreamSession> &ses_v) { return ses_v.push_back(*new_session); });
    state_->app_state->event_bus->fire_event(immer::box<events::StreamSession>(*new_session));

    auto res = StreamSessionCreated{.success = true, .session_id = std::to_string(new_session->session_id)};
    send_http(socket, 200, rfl::json::write(res));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto start_req = rfl::json::read<StreamSessionStartRequest>(req.body);
  if (start_req) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(start_req.value().session_id);
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto video_session = start_req.value().video_session;
      video_session.session_id = session_id; // Can't be JSON encoded
      if (video_session.render_node.empty()) {
        video_session.render_node = session->app->encoder_render_node.empty() ? session->app->render_node
                                                                              : session->app->encoder_render_node;
      }
      state_->app_state->event_bus->fire_event(immer::box<events::VideoSession>(video_session));

      auto audio_session = start_req.value().audio_session;
      audio_session.session_id = session_id; // Can't be JSON encoded
      state_->app_state->event_bus->fire_event(immer::box<events::AudioSession>(audio_session));

      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, start_req.error().what());
    auto res = GenericErrorResponse{.error = start_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionPause(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionPauseRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(session.value().session_id);
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::PauseStreamEvent>(events::PauseStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionStopRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(session.value().session_id);
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
      return;
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionHandleInput(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_request = rfl::json::read<StreamSessionHandleInputRequest>(req.body);
  if (input_request) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(input_request.value().session_id);
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto hex_pkt = input_request.value().input_packet_hex.get();
      auto input_pkt = control::sanitize_input_packet(crypto::hex_to_str(hex_pkt));
      if (!input_pkt) {
        logs::log(logs::warning, "[API] Malformed input packet: {}", hex_pkt);
        send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Malformed input packet"}));
        return;
      }
      control::handle_input(session.value(), {}, input_pkt->packet());

      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", input_request.value().session_id);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, input_request.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = input_request.error().what()}));
  }
}

void UnixSocketServer::endpoint_Lobbies(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  immer::vector<events::Lobby> lobbies = state_->app_state->lobbies->load();
  auto res = LobbiesResponse{.lobbies = lobbies | //
                                        ranges::views::transform([](const events::Lobby &lobby) {
                                          return rfl::Reflector<events::Lobby>::from(lobby);
                                        }) | //
                                        ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_LobbyCreate(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<CreateLobbyRequest>(req.body);
  if (event) {
    auto default_client_settings = state::ClientSettings{};
    auto client_settings = event.value().client_settings.value().value_or(PartialClientSettings{});
    auto lobby_id = state::gen_uuid();
    auto create_lobby_ev = events::CreateLobbyEvent{
        .id = lobby_id,
        .profile_id = event.value().profile_id.get(),
        .name = event.value().name,
        .icon_png_path = event.value().icon_png_path,
        .pin = event.value().pin.get(),
        .multi_user = event.value().multi_user,
        .stop_when_everyone_leaves = event.value().stop_when_everyone_leaves,
        .video_settings = event.value().video_settings,
        .audio_settings = event.value().audio_settings,
        .client_settings =
            state::ClientSettings{
                .run_uid = client_settings.run_uid.value_or(default_client_settings.run_uid),
                .run_gid = client_settings.run_gid.value_or(default_client_settings.run_gid),
                .controllers_override =
                    client_settings.controllers_override.value_or(default_client_settings.controllers_override),
                .mouse_acceleration =
                    client_settings.mouse_acceleration.value_or(default_client_settings.mouse_acceleration),
                .v_scroll_acceleration =
                    client_settings.v_scroll_acceleration.value_or(default_client_settings.v_scroll_acceleration),
                .h_scroll_acceleration =
                    client_settings.h_scroll_acceleration.value_or(default_client_settings.h_scroll_acceleration),
                .motion_controller_override = client_settings.motion_controller_override.value_or(
                    default_client_settings.motion_controller_override)},
        .runner_state_folder = event.value().runner_state_folder,
        .runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus)};
    // Fire the event
    state_->app_state->event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create_lobby_ev));

    auto setup_over_future = create_lobby_ev.on_setup_over.get()->get_future();
    auto result = setup_over_future.wait_for(std::chrono::seconds(20));
    if (result == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby setup timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby setup timed out"}));
    } else {
      auto res = LobbyCreateResponse{.lobby_id = lobby_id};
      send_http(socket, 200, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

std::optional<std::string /* Error message */> check_lobby_pin(const immer::vector<events::Lobby> &lobbies,
                                                               std::string_view lobby_id,
                                                               const std::optional<std::vector<short>> &pin) {
  auto lobby = state::get_lobby_by_id(lobbies, lobby_id);
  if (!lobby) {
    return "Invalid lobby ID";
  }
  if (lobby->pin != pin) {
    return "Invalid PIN";
  }
  return std::nullopt;
}

void UnixSocketServer::endpoint_LobbyJoin(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::JoinLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    auto lobby_ev = event.value();
    lobby_ev.error_message = std::make_shared<std::promise<std::string>>();
    state_->app_state->event_bus->fire_event(immer::box<events::JoinLobbyEvent>(lobby_ev));

    auto error_message_fut = lobby_ev.error_message.get()->get_future();
    auto future_status = error_message_fut.wait_for(std::chrono::seconds(2));
    if (future_status == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby join timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby join timed out"}));
    } else if (auto error_message = error_message_fut.get(); !error_message.empty()) {
      logs::log(logs::warning, "[API] Lobby join failed: {}", error_message);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = utils::to_string(error_message)}));
    } else {
      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyLeave(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::LeaveLobbyEvent>(req.body);
  if (event) {
    state_->app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyStop(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::StopLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    state_->app_state->event_bus->fire_event(immer::box<events::StopLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_RunnerStart(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<RunnerStartRequest>(req.body);
  if (event) {
    auto session = state::get_session_by_id(this->state_->app_state->running_sessions->load(),
                                            std::stoul(event.value().session_id));
    if (!session) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", event.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
      return;
    }

    auto runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus);
    state_->app_state->event_bus->fire_event(immer::box<events::StartRunner>(
        events::StartRunner{.stop_stream_when_over = event.value().stop_stream_when_over,
                            .runner = runner,
                            .stream_session = std::make_shared<events::StreamSession>(*session)}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_UpdateClientSettings(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload_result = rfl::json::read<UpdateClientSettingsRequest>(req.body);
  if (!payload_result) {
    auto res = GenericErrorResponse{.error = "Invalid request format"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  const auto &payload = payload_result.value();
  auto current_client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
  if (!current_client) {
    auto res = GenericErrorResponse{.error = "Client not found"};
    send_http(socket, 404, rfl::json::write(res));
    return;
  }

  // Edit only the settings that are being passed in the payload
  auto current_settings = current_client->settings;
  auto new_settings = payload.settings.get().value_or(PartialClientSettings{});
  auto merged_client = config::PairedClient{
      .client_cert = current_client->client_cert, // Immutable, changing this would mean a new client
      .app_state_folder = payload.app_state_folder.get().value_or(current_client->app_state_folder),
      .settings = config::ClientSettings{
          .run_uid = new_settings.run_uid.value_or(current_settings.run_uid),
          .run_gid = new_settings.run_gid.value_or(current_settings.run_gid),
          .controllers_override = new_settings.controllers_override.value_or(current_settings.controllers_override),
          .mouse_acceleration = new_settings.mouse_acceleration.value_or(current_settings.mouse_acceleration),
          .v_scroll_acceleration = new_settings.v_scroll_acceleration.value_or(current_settings.v_scroll_acceleration),
          .h_scroll_acceleration = new_settings.h_scroll_acceleration.value_or(current_settings.h_scroll_acceleration),
          .motion_controller_override =
              new_settings.motion_controller_override.value_or(current_settings.motion_controller_override),
      }};

  update_client_settings(this->state_->app_state->config, std::stoull(payload.client_id.value()), merged_client);

  auto res = GenericSuccessResponse{.success = true};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_GetIcon(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto icon_path = utils::split(req.query_string, '=');
  if (icon_path.size() != 2 || icon_path[0] != "icon_path") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'icon_path' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }
  // Only serve icons that are configured or live in the state folder: this must not be a generic file/URL fetcher
  std::vector<std::string> configured_icons;
  for (const auto &profile : state_->app_state->config->profiles->load().get()) {
    configured_icons.push_back(profile->icon_png_path);
    for (const auto &app : profile->apps->load().get()) {
      if (app->base.icon_png_path) {
        configured_icons.push_back(*app->base.icon_png_path);
      }
    }
  }
  if (state_->app_state->lobbies) {
    for (const auto &lobby : state_->app_state->lobbies->load().get()) {
      if (lobby.icon_png_path) {
        configured_icons.push_back(*lobby.icon_png_path);
      }
    }
  }
  auto trusted_hosts = utils::DEFAULT_ICON_URL_HOSTS;
  for (auto host : utils::split(utils::get_env("HEALER_ICON_URL_HOSTS", ""), ',')) {
    if (!host.empty()) {
      trusted_hosts.emplace_back(host);
    }
  }
  auto base_folder = state_->app_state->host->local_base_state_folder;
  if (!utils::is_icon_allowed(icon_path[1], base_folder, configured_icons, trusted_hosts)) {
    logs::log(logs::warning, "[API] Refusing to serve icon outside the config/state folder: {}", icon_path[1]);
    send_http(socket, 403, rfl::json::write(GenericErrorResponse{.error = "Icon not allowed"}));
    return;
  }

  // TODO: implement coroutines for CURL
  std::thread([this, socket, icon_path = utils::to_string(icon_path[1])]() {
    if (auto icon = utils::get_icon(this->state_->app_state->host->local_base_state_folder, icon_path)) {
      send_http(socket,
                200,
                {"Content-Length: " + std::to_string(icon->size()), "Content-Type: image/png"},
                icon.value());
    } else {
      auto res = GenericErrorResponse{.error = "Icon not found"};
      send_http(socket, 404, rfl::json::write(res));
    }
  }).detach();
}

void UnixSocketServer::endpoint_DockerInspectImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto image_name = utils::split(req.query_string, '=');
  if (image_name.size() != 2 || image_name[0] != "image_name") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'image_name' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  docker::DockerAPI docker_api(utils::get_env("HEALER_DOCKER_SOCKET", "/var/run/docker.sock"));
  if (auto response = docker_api.inspect_image(image_name[1])) {
    send_http(socket, 200, response.value());
  } else {
    auto res = GenericErrorResponse{.error = "Image not found"};
    send_http(socket, 404, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_DockerPullImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_payload = rfl::json::read<DockerPullImageRequest>(req.body);
  if (input_payload) {
    // TODO: implement coroutines for CURL
    std::thread([this, socket, image = input_payload.value().image_name]() {
      docker::DockerAPI docker_api(utils::get_env("HEALER_DOCKER_SOCKET", "/var/run/docker.sock"),
                                   utils::get_env("HEALER_DOCKER_CONFIG", ""));
      bool first_send = true;
      broadcast_event("DockerPullImageStartEvent",
                      rfl::json::write(events::DockerPullImageStartEvent{.image_name = image}));
      if (docker_api.pull_image(image,
                                {},
                                [this, &first_send, socket](const docker::DockerAPI::DockerProgressEvent &progress_ev) {
                                  if (first_send) {
                                    send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
                                    first_send = false;
                                  }
                                  auto serialized_ev = rfl::json::write(progress_ev) + "\r\n";
                                  send_data(socket, serialized_ev);
                                })) {
        if (first_send) {
          send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
        }
        auto final_result = rfl::json::write(GenericSuccessResponse{.success = true});
        send_data(socket, final_result + "\r\n");
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = true}));
      } else {
        send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Failed to pull image"}));
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = false}));
      }
    }).detach();
  }
}

} // namespace wolf::api