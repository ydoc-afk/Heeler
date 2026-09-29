# ydoc-afk/Heeler

[![Linux build and test](https://github.com/ydoc-afk/Heeler/actions/workflows/linux-build-test.yml/badge.svg)](https://github.com/ydoc-afk/Heeler/actions/workflows/linux-build-test.yml/badge.svg)
[![Discord](https://img.shields.io/discord/856434175455133727.svg?label=&logo=discord&logoColor=ffffff&color=7389D8&labelColor=6A7EC2)](https://discord.gg/kRGUDHNHt2)
[![GitHub license](https://img.shields.io/github/license/ydoc-afk/Heeler)](https://github.com/ydoc-afk/Heeler/blob/main/LICENSE)
[![Donate button](https://img.shields.io/badge/Donate-Open%20Collective-blue.svg?color=blue)](https://opencollective.com/games-on-whales/donate)

<p align="center">
  <img src="docs/modules/ROOT/images/heeler.jpg" alt="Heeler: share one Linux host with many Moonlight clients" width="800">
</p>

**Heeler** is a low-latency, open source streaming server for [Moonlight](https://moonlight-stream.org/) that lets you
share a single Linux host with **multiple remote clients** to play videogames. Each client gets an on-demand virtual
desktop (a headless Wayland compositor &mdash; no monitor or dummy plug needed) whose apps run in isolated
Docker/Podman containers.

## Features

- **Multi-user by design** &mdash; many clients stream different content from the same hardware at the same time
- **On-demand virtual desktops** &mdash; any resolution/FPS, created when a session starts and destroyed when it ends
- **Multi-GPU** &mdash; use several GPUs simultaneously (e.g. encode on the iGPU while gaming on the dGPU)
- **Low latency** &mdash; H.264/HEVC/AV1 hardware encoding (NVIDIA, Intel, AMD) with full gamepad, mouse and keyboard
  support, including hot-plugging controllers mid-session
- **Linux & Docker first** &mdash; games run with low privileges in containers
  (based on [Games On Whales](https://github.com/games-on-whales/gow))
- **Hackable** &mdash; the whole audio/video pipeline is a GStreamer config string in `config.toml`: change encoders,
  parameters or Docker details without touching code
- **Lobbies** &mdash; clients can share/join a running desktop for co-op sessions
- A control API over a Unix socket, used by [wolf-ui](https://github.com/games-on-whales/wolf-ui) for pairing,
  user management and session control

Heeler is a specific tool for a specific need. Looking for a general purpose single-user streaming solution?
Try out [Sunshine](https://github.com/LizardByte/Sunshine)!

## Get started

Heeler runs as a single container and spins up additional containers on demand. The short version (full details,
including NVIDIA and Podman, in the [quickstart](https://games-on-whales.github.io/wolf/stable/user/quickstart.html)):

```bash
docker run \
    --name heeler \
    --network=host \
    -v /etc/wolf:/etc/wolf:rw \
    -v /var/run/docker.sock:/var/run/docker.sock:rw \
    --device /dev/dri/ \
    --device /dev/uinput \
    --device /dev/uhid \
    -v /dev/:/dev/:rw \
    -v /run/udev:/run/udev:rw \
    --device-cgroup-rule "c 13:* rmw" \
    ghcr.io/ydoc-afk/wolf:stable
```

Then point any Moonlight client at your host, pair it (open `http://<host>:47989/pin/` and enter the 4-digit PIN
shown on the client), and start streaming. Prebuilt guest-app containers (Steam, Pegasus, PrismLauncher, ...) are
available from [games-on-whales/gow](https://github.com/games-on-whales/gow).

## Documentation

Heeler is a fork of [Wolf](https://github.com/games-on-whales/wolf); its documentation currently lives with the
original project:

- [User guide](https://games-on-whales.github.io/wolf/stable/) &mdash; quickstart, configuration, wolf-ui,
  troubleshooting
- [Developer guide](https://games-on-whales.github.io/wolf/stable/dev/how-it-works.html) &mdash; how it works under
  the hood, building from source, the control API
- [Protocol documentation](https://games-on-whales.github.io/wolf/stable/protocols/index.html) &mdash; the Moonlight
  protocol as implemented (pairing, RTSP, RTP, control)
- [FAQ](https://games-on-whales.github.io/wolf/stable/faq.html)
- Questions? Join the [Discord](https://discord.gg/kRGUDHNHt2)

## Acknowledgements

Heeler is a fork of [Wolf](https://github.com/games-on-whales/wolf) &mdash; thanks to [abeltra](https://github.com/abeltra)
and everyone else for the incredible work on the original project.

- [@Drakulix](https://github.com/Drakulix) for the incredible help given in developing Wolf
- [@zb140](https://github.com/zb140) for the constant help and support in [GOW](https://github.com/games-on-whales/gow)
- [@loki-47-6F-64](https://github.com/loki-47-6F-64) for creating and
  sharing [Sunshine](https://github.com/loki-47-6F-64/sunshine)
- [@ReenigneArcher](https://github.com/ReenigneArcher) for being the first stargazer of the project and taking care of
  keeping [Sunshine alive](https://github.com/LizardByte/Sunshine)
- All the guys at the [Moonlight](https://moonlight-stream.org/) Discord channel, for the tireless help they provide to
  anyone
