#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <gst-plugin/gstmoonlightopusenc.hpp>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <opus/opus_multistream.h>
#include <state/data-structures.hpp>
#include <streaming/streaming.hpp>

using Catch::Matchers::Equals;

TEST_CASE("moonlightopusenc: channel mapping parsing", "[GSTPlugin]") {
  using moonlight_opus::parse_mapping;
  REQUIRE(parse_mapping("", 6, 6, 0) == std::vector<unsigned char>{0, 1, 2, 3, 4, 5});
  REQUIRE(parse_mapping("0,4,1,2,3,5", 6, 4, 2) == std::vector<unsigned char>{0, 4, 1, 2, 3, 5});
  REQUIRE(parse_mapping("0,1,255", 3, 2, 0) == std::vector<unsigned char>{0, 1, 255}); // 255 = silent
  REQUIRE(parse_mapping("0,1,2", 6, 6, 0).empty());        // wrong length
  REQUIRE(parse_mapping("0,1,2,3,4,9", 6, 6, 0).empty());  // stream out of range
  REQUIRE(parse_mapping("0,1,a,3,4,5", 6, 6, 0).empty());  // garbage
  REQUIRE(parse_mapping("", 6, 3, 0).empty());             // not enough streams for identity
}

/**
 * Encodes `frames` frames of 6 channel audio where channel i is a constant tone of amplitude (i+1)*1000,
 * decodes them with a libopus decoder using `decoder_streams/coupled/mapping` and returns the decoded PCM
 */
static std::vector<opus_int16> encode_decode_5_1(const std::string &encoder_props,
                                                 int decoder_streams,
                                                 int decoder_coupled,
                                                 std::vector<unsigned char> decoder_mapping,
                                                 std::string *caps_out) {
  constexpr int channels = 6, frame = 240, frames = 40; // 5ms frames
  auto pipeline_desc = fmt::format("appsrc name=src format=time caps=\"audio/x-raw,format=S16LE,layout=interleaved,"
                                   "rate=48000,channels=6,channel-mask=(bitmask)0x3f\" ! "
                                   "moonlightopusenc {} ! appsink name=sink sync=false",
                                   encoder_props);
  GError *error = nullptr;
  auto pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
  REQUIRE(error == nullptr);
  auto src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
  auto sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  for (int f = 0; f < frames; f++) {
    std::vector<opus_int16> pcm(frame * channels);
    for (int s = 0; s < frame; s++) {
      for (int c = 0; c < channels; c++) {
        // A different frequency per channel so that channels can be told apart after lossy coding
        pcm[s * channels + c] = static_cast<opus_int16>(8000 * std::sin(2 * M_PI * (200 + 300 * c) * (f * frame + s) / 48000.0));
      }
    }
    auto buf = gst_buffer_new_memdup(pcm.data(), pcm.size() * sizeof(opus_int16));
    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(f * frame, GST_SECOND, 48000);
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(frame, GST_SECOND, 48000);
    gst_app_src_push_buffer(GST_APP_SRC(src), buf);
  }
  gst_app_src_end_of_stream(GST_APP_SRC(src));

  int error_code = 0;
  auto decoder = opus_multistream_decoder_create(48000,
                                                 channels,
                                                 decoder_streams,
                                                 decoder_coupled,
                                                 decoder_mapping.data(),
                                                 &error_code);
  REQUIRE(error_code == OPUS_OK);
  std::vector<opus_int16> decoded;
  while (auto sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND)) {
    if (caps_out && caps_out->empty()) {
      auto caps_str = gst_caps_to_string(gst_sample_get_caps(sample));
      *caps_out = caps_str;
      g_free(caps_str);
    }
    GstMapInfo map;
    auto buffer = gst_sample_get_buffer(sample);
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    std::vector<opus_int16> out(frame * channels);
    auto n = opus_multistream_decode(decoder, map.data, static_cast<opus_int32>(map.size), out.data(), frame, 0);
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    REQUIRE(n == frame);
    decoded.insert(decoded.end(), out.begin(), out.end());
  }
  opus_multistream_decoder_destroy(decoder);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(src);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  return decoded;
}

/**
 * Amplitude of the tone that was fed into input channel `input`, as found in decoded channel `c`.
 */
static double tone_match(const std::vector<opus_int16> &pcm, int c, int input) {
  // Correlate against both sin and cos so that the (codec delay) phase shift doesn't matter
  double dot_sin = 0, dot_cos = 0, energy = 0;
  auto samples = pcm.size() / 6;
  for (size_t s = 2400; s < samples; s++) { // skip the encoder warm up
    auto phase = 2 * M_PI * (200 + 300 * input) * s / 48000.0;
    dot_sin += pcm[s * 6 + c] * std::sin(phase);
    dot_cos += pcm[s * 6 + c] * std::cos(phase);
    energy += std::sin(phase) * std::sin(phase);
  }
  return std::sqrt(dot_sin * dot_sin + dot_cos * dot_cos) / energy;
}

TEST_CASE("moonlightopusenc: Moonlight high quality 5.1 layout", "[GSTPlugin]") {
  std::string caps;
  // What we advertise in DESCRIBE for high quality 5.1 and what Moonlight decodes with
  auto pcm = encode_decode_5_1("bitrate=1536000 frame-size=5 streams=6 coupled-streams=0", 6, 0, {0, 1, 2, 3, 4, 5}, &caps);
  REQUIRE_THAT(caps, Catch::Matchers::ContainsSubstring("stream-count=(int)6"));
  REQUIRE_THAT(caps, Catch::Matchers::ContainsSubstring("coupled-count=(int)0"));
  REQUIRE(pcm.size() >= 40 * 240 * 6 - 240 * 6);

  // Every channel comes out where it went in (and not in another channel)
  for (int c = 0; c < 6; c++) {
    CAPTURE(c);
    REQUIRE(tone_match(pcm, c, c) > 6000);
    REQUIRE(tone_match(pcm, c, (c + 1) % 6) < 500);
  }
}

TEST_CASE("moonlightopusenc: explicit coupled layout", "[GSTPlugin]") {
  // The normal quality 5.1 layout that Moonlight gets from our DESCRIBE (4 streams, 2 coupled)
  auto pcm =
      encode_decode_5_1("bitrate=256000 streams=4 coupled-streams=2 mapping=0,4,1,2,3,5", 4, 2, {0, 4, 1, 2, 3, 5}, nullptr);
  for (int c = 0; c < 6; c++) {
    CAPTURE(c);
    REQUIRE(tone_match(pcm, c, c) > 6000);
  }
}

TEST_CASE("High quality audio swaps opusenc for moonlightopusenc", "[GSTPlugin]") {
  auto hq = state::get_audio_mode(6, true);
  REQUIRE(state::is_high_quality(hq));
  REQUIRE(hq.streams == 6);
  REQUIRE(hq.coupled_streams == 0);
  // Stereo has no high quality variant
  REQUIRE(!state::is_high_quality(state::get_audio_mode(2, true)));
  REQUIRE(!state::is_high_quality(state::get_audio_mode(6, false)));

  auto pipeline = std::string("interpipesrc ! audioconvert ! opusenc bitrate=256000 bitrate-type=cbr frame-size=5 "
                              "audio-type=restricted-lowdelay ! queue ! rtpmoonlightpay_audio");
  auto swapped = streaming::use_high_quality_opus_encoder(pipeline, hq, 5);
  REQUIRE_THAT(swapped,
               Equals("interpipesrc ! audioconvert ! moonlightopusenc bitrate=1536000 frame-size=5 streams=6 "
                      "coupled-streams=0 ! queue ! rtpmoonlightpay_audio"));
  // Parses and links with the real elements (minus the source)
  GError *error = nullptr;
  auto bin = gst_parse_launch("audiotestsrc num-buffers=1 ! audio/x-raw,channels=6 ! audioconvert ! moonlightopusenc "
                              "bitrate=1536000 frame-size=5 streams=6 coupled-streams=0 ! fakesink",
                              &error);
  REQUIRE(error == nullptr);
  gst_object_unref(bin);
}
