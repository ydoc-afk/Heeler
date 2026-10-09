## What Heeler is

Heeler is a low-latency streaming server for [Moonlight](https://moonlight-stream.org/) that lets multiple
remote clients share a single Linux host to play games. Each client gets an on-demand virtual desktop
(Wayland compositor, no physical monitor needed) whose apps run in isolated Docker/Podman containers.
It implements the Moonlight protocol (pairing over HTTPS, RTSP handshake, ENet control channel, RTP
video/audio) and hands video/audio off to GStreamer pipelines. Linux + Docker first; C++20.

## Agent workflow rules

- **Never block on `sleep` in a foreground shell command.** Waiting on CI (`gh pr checks`,
  `gh run view`) with `sleep N && gh ...` freezes the whole agent loop with no output. Poll in a
  background job instead (e.g. `bg_start` in pi, `run_in_background` in Claude Code) and check it
  when it completes. One-shot short waits (≤ 20s) are fine; anything longer must be backgrounded.
- **Git must never open an editor.** `git rebase --continue`, `cherry-pick`, `rebase -i`,
  `commit --amend`, etc. will hang a non-interactive shell waiting for a commit message. Always set
  `GIT_EDITOR=true` (or pass `--no-edit` / use `git commit --no-edit`) for these commands, and put a
  timeout on any git command that could prompt.
- **Timebox long-running commands** (builds, tests, pushes) so a hung command surfaces as a timeout
  instead of stalling the session indefinitely.

## Companion repositories

Heeler is split across three repos; the other two are integral and pulled in at build time — when
touching virtual input or virtual-display behavior, the real implementation often lives there:

- **[games-on-whales/inputtino](https://github.com/games-on-whales/inputtino)** — virtual input device
  library (`uinput`/`uhid`). Heeler uses it for gamepads (incl. gyro/accel) and pen/touch; mouse and
  keyboard don't go through it (see Architecture). Surfaced via `src/core/.../input.hpp`.
- **[games-on-whales/gst-wayland-display](https://github.com/games-on-whales/gst-wayland-display)** —
  the custom micro Wayland compositor (Rust, built on [Smithay](https://github.com/Smithay/smithay);
  we track a fork at [games-on-whales/smithay](https://github.com/games-on-whales/smithay)). Creates
  on-demand desktops and exposes the raw framebuffer as a GStreamer plugin + C API. Installed as
  `libgstwaylanddisplay` (surfaced via `src/core/.../virtual-display.hpp`); its `.so` must be on
  `GST_PLUGIN_PATH` at runtime and to run the tests.

Prebuilt guest-app containers live in [games-on-whales/gow](https://github.com/games-on-whales/gow).

## Build & test

Two supported paths, both documented in `docs/modules/dev/pages/manual_build.adoc`:

- **Devcontainer (recommended)** — `docker/wolf.Dockerfile` target `wolf-builder` via `.devcontainer/`,
  so you build in the exact environment of the official image with all deps preinstalled (VS Code:
  *Dev Containers: Clone Repository in Container Volume*, pick the Clang kit).
- **Manual host build** — build Heeler outside Docker (Docker must still be installed for Heeler to do
  anything useful). The doc covers building GStreamer and `gst-wayland-display` from source, apt deps,
  the required `LD_LIBRARY_PATH`/`PKG_CONFIG_PATH`/etc. env, and a `runwolf.sh` template of `HEELER_*`
  runtime vars.

Most C++ deps are fetched at configure time via CMake `FetchContent` (fmt, tomlplusplus, reflect-cpp,
eventbus, Catch2, immer, boost via `BoostLoader`); system libs still needed include Boost, GStreamer,
Wayland, libinput, libevdev, libudev, OpenSSL, PulseAudio, libdrm, libpci.

```bash
# Configure (matches CI). CI uses C++20; manual_build.adoc still shows 17 — prefer 20.
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20 -DCATCH_DEVELOPMENT_BUILD=ON

ninja -C build wolf         # server binary → build/src/moonlight-server/wolf
ninja -C build wolftests    # test suite (Catch2)
cd build/tests && ./wolftests
```

Run a single test / subset with Catch2 selectors: `./wolftests "test name"`, `-c "section"`,
`"[tag]"`, or `--list-tests`. Tests need `HOST_APPS_STATE_FOLDER`, `GST_PLUGIN_PATH` (where
`libgstwaylanddisplay` is installed), `XDG_RUNTIME_DIR`, and `RUST_LOG` set (see the `test` job in
`.github/workflows/linux-build-test.yml`).

Hardware/environment-dependent tests are gated by CMake options so CI can skip what a runner lacks:
`TEST_DOCKER`, `TEST_NVIDIA`, `TEST_VIRTUAL_INPUT`, `TEST_UHID`, `TEST_RUST_WAYLAND`, `TEST_EXCEPTIONS`,
`TEST_SDL`. CI runs a g++/clang matrix with `BUILD_SHARED_LIBS` both ON and OFF — keep both working.

C++ is auto-formatted with clang-format (`.clang-format`, 120 col) and enforced in CI; run
`clang-format -i` on changed files before committing. `.clang-tidy` is also present.

## Architecture

**Style (deliberate, per the original author):** functional — no global state, side effects avoided,
immutable inputs → new outputs. Shared/persistent state lives in [immer](https://github.com/arximboldi/immer)
persistent containers wrapped in `immer::atom<...>` (e.g. `SessionsAtoms`, `PairedClientList`): treat a
snapshot as read-only and swap in a new one to update, never mutate in place. This is what makes Heeler's
heavy concurrency (many simultaneous users) lock-free and safe. Prefer pure functions over shared
mutable state, and keep protocol logic decoupled from the server runtime.

**Coordination:** components talk through a shared **event bus** (`dp::event_bus`, `EventBusType` in
`events/events.hpp`) rather than direct calls — `PairSignal`, `StreamSession`, `CreateLobbyEvent`,
`StartRunner`, `PlugDeviceEvent`, docker lifecycle events, etc. For cross-component behavior, define/
handle an event instead of adding a direct dependency. New events go in `events/events.hpp`;
`reflectors.hpp` exposes them for serialization (all config/API/event serialization uses reflect-cpp —
annotate types rather than hand-writing parsers).

### How a stream works

See `docs/modules/dev/pages/how-it-works.adoc` for the full picture.

- **Virtual desktop** — `gst-wayland-display` creates a desktop on demand and feeds its raw framebuffer
  into the encode pipeline. Our images run [Sway](https://swaywm.org/) (optionally
  [Gamescope](https://github.com/ValveSoftware/gamescope)) as a Wayland client inside it. The
  compositor has no XWayland; apps needing X (e.g. Steam) rely on Gamescope for it.
- **Virtual audio** — by default PulseAudio runs **inside the Heeler container** under supervisord
  (`docker/startup.sh` sets `HEELER_EMBED_PULSE=true`; Heeler waits for the PA socket before starting). With
  an external `PULSE_SERVER`, or if `pulseaudio` isn't installed, Heeler falls back to the legacy
  standalone `WolfPulseAudio` sidecar. Either way it uses `libpulse` for per-session virtual sinks.
- **Virtual input** — mouse/keyboard events go **directly to the Wayland compositor** (no host device);
  gamepads and pen/touch are real `uinput`/`uhid` devices created by `inputtino`. Those are visible on
  the host, so `85-wolf.rules` restricts them to a group/seat for isolation. The virtual DualSense uses
  `uhid` (plain `uinput` wasn't enough) — see the author's blog:
  [1](https://abeltra.me/blog/inputtino-uhid-1/),
  [2](https://abeltra.me/blog/inputtino-uhid-2/),
  [3](https://abeltra.me/blog/inputtino-uhid-3/).
- **Hotplug** — devices added mid-stream are injected into the running container via the `src/fake-udev`
  CLI; see [Docker hotplug](https://abeltra.me/blog/docker-hotplug/) for how fake-udev achieves this.
- **Guest apps** — run in containers. The Docker runner (`runners/docker.cpp`) builds the per-session
  spec: mounts a per-app state folder, exposes the GPU render node (`/dev/dri/renderD*`), on NVIDIA adds
  the driver (custom driver volume, or `--gpus all` + `NVIDIA_VISIBLE_DEVICES`/`_DRIVER_CAPABILITIES` +
  the `nvidia` runtime), passes through Heeler's virtual input devices, sets `DeviceCgroupRules` for the
  dynamic `hidraw`/`input` majors (needed for the virtual DualSense), and wires up fake-udev. It then
  blocks for the container's lifetime plugging/unplugging devices via the event bus, and on exit
  stops/removes it (`HEELER_STOP_CONTAINER_ON_EXIT`) and cleans up the udev scratch dir.
- **Streaming** — GStreamer encodes video/audio (HW accel via CUDA/QuickSync/VAAPI; the whole pipeline
  is a config-string in `config.toml`, overridable without code). Custom plugins in `gst-plugin/`
  (`rtpmoonlightpay_video`/`_audio`) split, RTP-encode, and add FEC to Moonlight's format. The pipeline
  is zero-copy from framebuffer to encoded frames — on by default, disable with `HEELER_USE_ZERO_COPY=FALSE`
  (auto-falls back to legacy when an encoder can't support it); see
  [The road to zero-copy in Wolf](https://abeltra.me/blog/road-to-zero-copy-in-wolf/).

### Code map

`CMakeLists.txt` composes these targets:

- **`src/moonlight-protocol`** — platform-agnostic, mostly stateless Moonlight library: HTTP/S
  `protocol.hpp`, control-packet `control.hpp`, Reed-Solomon `fec.hpp` (on
  [nanors](https://github.com/sleepybishop/nanors)), and an RTSP parser built from a PEG grammar via
  [cpp-peglib](https://github.com/yhirose/cpp-peglib). No server deps.
- **`src/core`** (`wolf::core`) — reusable platform abstractions: `docker.hpp` (Docker/Podman REST over
  libcurl + boost::json), `input.hpp` (inputtino), `virtual-display.hpp` (gst-wayland-display),
  `audio.hpp` (libpulse), `gstreamer.hpp`. Split into `platforms/{all,linux,unknown}`; `unknown` holds
  no-op stubs so non-Linux configs still compile — provide one when adding platform code.
- **`src/gst-video-context`** — GStreamer video context helpers.
- **`src/fake-udev`** — CLI (Linux only) that generates/injects udev events (hotplug, above).
- **`src/moonlight-server`** (`wolf::runner` → the `wolf` binary) — the full server:
  - `wolf.cpp` — `main`: loads config, sets up certs, starts servers, wires the event bus, runs the
    boost::asio loop, graceful shutdown via a signal flag.
  - `state/` — `AppState` + config model (TOML via `configTOML.cpp`, tomlplusplus + reflect-cpp; see
    `tests/assets/config.test.toml`).
  - `rest/` `control/` `rtsp/` `rtp/` — the Moonlight protocol stack: HTTPS pairing/REST, RTSP setup,
    ENet control channel + input handling, RTP ping/transport.
  - `streaming/` `gst-plugin/` `audio/` — GStreamer video/audio; `audio/pulse_router` bridges container
    audio.
  - `runners/` — `docker.cpp` (containers, default) and `process.cpp` (host process), fired by
    `StartRunner`.
  - `sessions/` — session/lobby lifecycle (`moonlight.cpp`, `lobbies.cpp`); a "lobby" lets clients
    share/join a running desktop.
  - `api/` — a separate control API over a Unix socket (`unix_socket_server.cpp`) with an OpenAPI spec
    (`openapi.cpp`), used by external tools (e.g. wolf-ui). Distinct from the Moonlight-facing `rest/`.

## Conventions & pitfalls

Learned from the codebase itself — read before touching the areas below:

- **Exceptions are fatal by design (mostly).** Uncaught exceptions inside Simple-Web-Server
  resource handlers, the Wolf API thread pool (`api/http_server.hpp`), boost::asio async
  lambdas, or event-bus handlers propagate out and `std::terminate` the whole process. There is
  no central try/catch. When handling untrusted input (query headers, JSON bodies, env vars),
  prefer `value_or` / validated parsing over `optional::value()` and `std::stoi`.
- **Event bus** (`dp::eventbus`): `fire_event` is *synchronous* (handlers run on the firing
  thread) and nested `fire_event` from inside a handler is common (lobbies do it constantly) —
  handlers must stay short and non-blocking; long work goes to `std::thread(...).detach()`. The
  returned `EventBusHandlers` is RAII: unregistered when it goes out of scope, so keep it alive
  exactly as long as the handler must stay armed (a local in a blocking `run()` loop is the
  usual pattern).
- **Protocol reference implementation:** for Moonlight packet layouts (control packets, RTP
  video header/FEC, GCM IV construction), the [Sunshine server](https://github.com/LizardByte/Sunshine)
  is the de-facto reference (e.g. `src/stream.cpp` for the video/FEC header, `session` control
  seq handling). When in doubt about a wire format, diff against it before "fixing" wolf —
  several wolf layouts intentionally match it (e.g. `multiFecBlocks = (block << 4) | (nblocks-1) << 6`).
- **Crypto helpers are non-throwing on purpose-ish:** `crypto::handle_openssl_error`
  (`src/moonlight-protocol/crypto/src/utils.cpp`) only prints and returns; `aes::init`/`create_key`
  can therefore return a broken/null context that callers must not assume is valid. New crypto
  code should check return values explicitly.
- **Config is both runtime state and a file.** `state::pair`/`unpair`/`update_*` mutate the
  in-memory `immer` atom *and* rewrite `config.toml` via reflect-cpp. Keep both in sync in any
  new mutation (see the duplicate-entry history in `pair()`, issue #211).
- **GStreamer pipelines are config strings** built with `fmt::format(fmt::runtime(...))` from
  `config.toml` (placeholders like `{session_id}`, `{client_ip}`). `run_pipeline`
  (`streaming/streaming.hpp`) blocks on its own `GMainLoop` per thread; the `on_pipeline_ready`
  callback runs synchronously before the loop starts and its returned handlers are unregistered
  when the loop exits.
- **`src/core` platform split:** anything platform-specific needs a `platforms/unknown` no-op
  stub so non-Linux builds keep compiling (see `platforms/all` vs `platforms/linux`).
- **Testing:** `tests/` is Catch2; protocol/packet tests live in `testControl.cpp`/
  `testMoonlight.cpp` — when changing packet structs, check the packed layouts there and in
  `src/moonlight-protocol/moonlight/control.hpp` (`#pragma pack(push, 1)`).

## Branches, nightlies and releases

- `stable` is what gets released. Tags `vYYYY.MM.N` build `ghcr.io/ydoc-afk/wolf:YYYY.MM.N`.
- `nightly` is the integration branch: feature PRs target it first, so changes run on a real host before they reach
  `stable`. Pushes to it build `wolf:nightly`; a scheduled run (03:00 UTC, only when `nightly` got commits) also
  tags `wolf:nightly-YYYYMMDD`. The schedule lives in `docker-build.yml` on `stable` (GitHub only runs scheduled
  workflows from the default branch) and checks out `nightly` itself.
- Promote with a PR `nightly` -> `stable`, then tag. Hotfixes go to `stable` and are merged back into `nightly`.

## Runtime configuration (env vars)

Behavior is driven by `HEELER_*` env vars read via `utils::get_env` (full working set in `wolf.cpp` and
`.devcontainer/devcontainer.json`): `HEELER_CFG_FILE`, `HEELER_PRIVATE_KEY_FILE`/`HEELER_PRIVATE_CERT_FILE`,
`HEELER_LOG_LEVEL`, `HEELER_DOCKER_SOCKET`, `HEELER_RENDER_NODE`/`HEELER_ENCODER_NODE` (GPU DRI nodes),
`HEELER_PULSE_IMAGE`, `HEELER_INTERNAL_IP`/`HEELER_INTERNAL_MAC`, `HEELER_USE_ZERO_COPY`,
`HEELER_STOP_CONTAINER_ON_EXIT`, `HEELER_STEAM_LIBRARY` (shared Steam library, see the user docs). The old `HEALER_*` spelling and the legacy `WOLF_*` names
are still accepted as deprecated aliases (`HEELER_*` wins, then `HEALER_*`, then `WOLF_*`). A few (e.g. `HEELER_EMBED_PULSE`, `PULSE_SERVER`) are set/consumed
by `docker/startup.sh` + `supervisord.conf`, not by Heeler itself. Container-boundary vars that guest apps
read (`WOLF_SOCKET_PATH`, `WOLF_SESSION_ID`, `WOLF_VIDEO_BUFFER_CAPS`) intentionally keep the `WOLF_`
prefix.
