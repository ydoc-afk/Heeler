#pragma once

#include <algorithm>
#include <control/control.hpp>
#include <events/events.hpp>
#include <moonlight/control.hpp>
#include <optional>
#include <string_view>

namespace control {

using namespace moonlight::control::pkts;
using namespace wolf::core;

/**
 * Side effect: session devices might be updated when hotplugging
 */
void handle_input(events::StreamSession &session,
                  immer::box<std::shared_ptr<ENetPeer>> connected_client,
                  INPUT_PKT *pkt);

/**
 * Big enough to hold any of the input packets that handle_input() can cast an INPUT_PKT to
 */
constexpr std::size_t MAX_INPUT_PACKET_SIZE = std::max({sizeof(MOUSE_MOVE_REL_PACKET),
                                                        sizeof(MOUSE_MOVE_ABS_PACKET),
                                                        sizeof(MOUSE_BUTTON_PACKET),
                                                        sizeof(MOUSE_SCROLL_PACKET),
                                                        sizeof(MOUSE_HSCROLL_PACKET),
                                                        sizeof(KEYBOARD_PACKET),
                                                        sizeof(UTF8_TEXT_PACKET),
                                                        sizeof(CONTROLLER_MULTI_PACKET),
                                                        sizeof(HAPTICS_PACKET),
                                                        sizeof(TOUCH_PACKET),
                                                        sizeof(PEN_PACKET),
                                                        sizeof(CONTROLLER_ARRIVAL_PACKET),
                                                        sizeof(CONTROLLER_TOUCH_PACKET),
                                                        sizeof(CONTROLLER_MOTION_PACKET),
                                                        sizeof(CONTROLLER_BATTERY_PACKET)});

struct InputPacketBuffer {
  alignas(std::max_align_t) char data[MAX_INPUT_PACKET_SIZE] = {};

  INPUT_PKT *packet() {
    return reinterpret_cast<INPUT_PKT *>(data);
  }
};

/**
 * Copies an untrusted input packet (ex: from the API) into a zeroed buffer that can hold any input packet,
 * so that handle_input() never reads past the end of what the caller sent.
 * Returns nullopt when the packet is shorter than the INPUT_PKT header or declares more text than it carries.
 */
std::optional<InputPacketBuffer> sanitize_input_packet(std::string_view raw);

void mouse_move_rel(const MOUSE_MOVE_REL_PACKET &pkt, events::StreamSession &session);

void mouse_move_abs(const MOUSE_MOVE_ABS_PACKET &pkt, events::StreamSession &session);

void mouse_button(const MOUSE_BUTTON_PACKET &pkt, events::StreamSession &session);

void mouse_scroll(const MOUSE_SCROLL_PACKET &pkt, events::StreamSession &session);

void mouse_h_scroll(const MOUSE_HSCROLL_PACKET &pkt, events::StreamSession &session);

void keyboard_key(const KEYBOARD_PACKET &pkt, events::StreamSession &session);

void utf8_text(const UTF8_TEXT_PACKET &pkt, events::StreamSession &session);

void touch(const TOUCH_PACKET &pkt, events::StreamSession &session);

void pen(const PEN_PACKET &pkt, events::StreamSession &session);

void controller_arrival(const CONTROLLER_ARRIVAL_PACKET &pkt,
                        events::StreamSession &session,
                        immer::box<std::shared_ptr<ENetPeer>> connected_client);

void controller_multi(const CONTROLLER_MULTI_PACKET &pkt,
                      events::StreamSession &session,
                      immer::box<std::shared_ptr<ENetPeer>> connected_client);

void controller_touch(const CONTROLLER_TOUCH_PACKET &pkt, events::StreamSession &session);

void controller_motion(const CONTROLLER_MOTION_PACKET &pkt, events::StreamSession &session);

void controller_battery(const CONTROLLER_BATTERY_PACKET &pkt, events::StreamSession &session);

} // namespace control
