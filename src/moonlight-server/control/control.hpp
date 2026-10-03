#pragma once

#include <chrono>
#include <enet/enet.h>
#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <moonlight/control.hpp>
#include <range/v3/view.hpp>
#include <state/data-structures.hpp>
#include <thread>
#include <vector>

namespace control {

using namespace std::chrono_literals;
using namespace wolf::core;

void run_control(int port,
                 const state::SessionsAtoms &running_sessions,
                 const std::shared_ptr<events::EventBusType> &event_bus,
                 int peers = 20,
                 std::chrono::milliseconds timeout = 1000ms,
                 const std::string &host_ip = "0.0.0.0");

using enet_clients_map = immer::map<ENetPeer *, immer::box<events::StreamSession>>;

std::shared_ptr<ENetPeer> to_shared_ptr(ENetPeer *peer);

/**
 * The other peers that are still mapped to session_id when new_peer connects for it.
 * They belong to a previous connection of the same client (ex: before a resume) and must be dropped:
 * once they time out their disconnect would pause the stream that new_peer just started.
 */
std::vector<ENetPeer *>
stale_peers(const enet_clients_map &connected_clients, std::size_t session_id, const ENetPeer *new_peer);

bool encrypt_and_send(std::string_view payload,
                      std::string_view aes_key,
                      const std::shared_ptr<std::atomic<std::uint32_t>> &seq,
                      immer::box<std::shared_ptr<ENetPeer>> connected_client);

bool init();

} // namespace control