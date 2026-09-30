#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::Equals;

#include <control/input_handler.hpp>
#include <moonlight/control.hpp>
using namespace moonlight::control;

static std::string to_string(const ControlEncryptedPacket &packet) {
  return {(char *)&packet, packet.full_size()};
}

TEST_CASE("Control AES Encryption", "CONTROL") {
  // A bunch of packets taken from a real session

  std::string aes_key = "EDF04A215C4FBEA20934120C8480D855";

  SECTION("30 bytes") { // original packet: 01001A0000000000BF0EB6DA10E47C702EC8644EB87D9CF7B6FAC9FF75CA
    std::string payload = crypto::hex_to_str("020302000000");
    std::uint32_t seq = 0;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("01001A0000000000BF0EB6DA10E47C702EC8644EB87D9CF7B6FAC9FF75CA"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("IDR_FRAME"));
  }

  SECTION("29 bytes") { // original packet: 010019000100000021DBB8DC0590AF3A2B20BCE5A347DE31D366E5B9C5"
    std::string payload = crypto::hex_to_str("0703010000");
    std::uint32_t seq = 1;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("010019000100000021DBB8DC0590AF3A2B20BCE5A347DE31D366E5B9C5"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("START_B"));
  }

  SECTION("36 bytes") { // original packet: 0100200002000000220722FBADED58A03F2E8898F0F1DCB7C93F6235590618E4186AD990
    std::string payload = crypto::hex_to_str("000208000400000000000000");
    std::uint32_t seq = 2;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("0100200002000000220722FBADED58A03F2E8898F0F1DCB7C93F6235590618E4186AD990"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("PERIODIC_PING"));
  }

  SECTION("46 bytes") { // original packet:
                        // 01002A00060000005A4D999FB2542F85BDD39D99F77EB825254569D2C04E21241B5CEC01BD3F93129718ECC1F153
    std::string payload = crypto::hex_to_str("060212000000000E05000000033400C00000059F0329");
    std::uint32_t seq = 6;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(
        crypto::str_to_hex(to_string(encrypted_packet)),
        Equals("01002A00060000005A4D999FB2542F85BDD39D99F77EB825254569D2C04E21241B5CEC01BD3F93129718ECC1F153"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("INPUT_DATA"));
  }
}

TEST_CASE("control joypad input packets") {
  std::string payload =
      crypto::hex_to_str("060222000000001E0C0000001A000000010014000010000000000000000000009C0000005500");

  auto input_data = (pkts::CONTROLLER_MULTI_PACKET *)payload.data();
  auto pressed_btns = input_data->button_flags | (input_data->buttonFlags2 << 16);

  REQUIRE(input_data->type == pkts::CONTROLLER_MULTI);
  REQUIRE(input_data->active_gamepad_mask == 1);
  REQUIRE(pressed_btns & pkts::CONTROLLER_BTN::A);
}
TEST_CASE("Sanitize untrusted input packets", "[CONTROL]") {
  using namespace moonlight::control::pkts;

  // Shorter than the INPUT_PKT header
  REQUIRE(!control::sanitize_input_packet(std::string(sizeof(INPUT_PKT) - 1, '\0')));

  // A bare header is accepted and padded with zeroes up to the biggest packet
  INPUT_PKT header{.packet_type = 0x0206, .packet_len = 0, .data_size = 0, .type = MOUSE_MOVE_REL};
  auto sanitized = control::sanitize_input_packet({reinterpret_cast<char *>(&header), sizeof(header)});
  REQUIRE(sanitized);
  REQUIRE(sanitized->packet()->type == MOUSE_MOVE_REL);
  auto move = static_cast<MOUSE_MOVE_REL_PACKET *>(sanitized->packet());
  REQUIRE(move->delta_x == 0);
  REQUIRE(move->delta_y == 0);

  // Overlong packets are truncated, not rejected
  REQUIRE(control::sanitize_input_packet(std::string(control::MAX_INPUT_PACKET_SIZE + 100, '\0')));

  SECTION("UTF8_TEXT declared size must match what was sent") {
    UTF8_TEXT_PACKET text{};
    text.type = UTF8_TEXT;
    text.text[0] = 'a';
    auto header_size = sizeof(INPUT_PKT::packet_type) + 2;
    auto with_data_size = [&](std::size_t data_size, std::size_t sent_text) {
      text.data_size = boost::endian::native_to_big(static_cast<unsigned int>(data_size));
      return control::sanitize_input_packet({reinterpret_cast<char *>(&text), sizeof(INPUT_PKT) + sent_text});
    };
    REQUIRE(with_data_size(header_size + 1, 1));                          // 1 char, 1 char sent
    REQUIRE(!with_data_size(header_size - 1, 1));                         // would underflow
    REQUIRE(!with_data_size(header_size + 4, 1));                         // claims more than was sent
    REQUIRE(!with_data_size(header_size + UTF8_TEXT_MAX_LEN + 1, UTF8_TEXT_MAX_LEN)); // bigger than the text buffer
  }
}
