#include <gst-plugin/gstmoonlightopusenc.hpp>
#include <gst/audio/audio.h>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>

GST_DEBUG_CATEGORY_STATIC(gst_moonlight_opus_enc_debug_category);
#define GST_CAT_DEFAULT gst_moonlight_opus_enc_debug_category

namespace moonlight_opus {

std::vector<unsigned char> parse_mapping(const std::string &mapping, int channels, int streams, int coupled) {
  auto decoded_channels = streams + coupled;
  std::vector<unsigned char> result;
  if (mapping.empty()) { // identity
    if (decoded_channels < channels) {
      return {};
    }
    for (int i = 0; i < channels; i++) {
      result.push_back(static_cast<unsigned char>(i));
    }
    return result;
  }
  for (auto part : utils::split(mapping, ',')) {
    if (part.empty() || part.find_first_not_of("0123456789") != std::string_view::npos || part.size() > 3) {
      return {};
    }
    auto value = std::stoi(std::string(part));
    // 255 means "silent channel" in the Opus mapping
    if (value >= decoded_channels && value != 255) {
      return {};
    }
    result.push_back(static_cast<unsigned char>(value));
  }
  if (static_cast<int>(result.size()) != channels) {
    return {};
  }
  return result;
}

} // namespace moonlight_opus

enum {
  PROP_0,
  PROP_BITRATE,
  PROP_FRAME_SIZE,
  PROP_STREAMS,
  PROP_COUPLED_STREAMS,
  PROP_MAPPING,
};

constexpr int SAMPLE_RATE = 48000;

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "audio/x-raw, format = (string) " GST_AUDIO_NE(S16) ", "
                                                            "layout = (string) interleaved, rate = (int) 48000, "
                                                            "channels = (int) [ 1, 255 ]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("audio/x-opus"));

G_DEFINE_TYPE_WITH_CODE(GstMoonlightOpusEnc,
                        gst_moonlight_opus_enc,
                        GST_TYPE_AUDIO_ENCODER,
                        GST_DEBUG_CATEGORY_INIT(gst_moonlight_opus_enc_debug_category,
                                                "moonlightopusenc",
                                                0,
                                                "Opus multistream encoder with an explicit stream layout"));

static void free_encoder(GstMoonlightOpusEnc *enc) {
  if (enc->encoder) {
    opus_multistream_encoder_destroy(enc->encoder);
    enc->encoder = nullptr;
  }
}

static gboolean gst_moonlight_opus_enc_set_format(GstAudioEncoder *base, GstAudioInfo *info) {
  auto enc = GST_MOONLIGHT_OPUS_ENC(base);
  free_encoder(enc);

  enc->channels = GST_AUDIO_INFO_CHANNELS(info);
  // Default layout: one stream per channel (as used by Moonlight high quality mode)
  auto streams = enc->streams > 0 ? enc->streams : enc->channels;
  auto coupled = enc->streams > 0 ? enc->coupled_streams : 0;
  auto mapping = moonlight_opus::parse_mapping(enc->mapping ? enc->mapping : "", enc->channels, streams, coupled);
  if (mapping.empty() || streams < 1 || coupled < 0 || coupled > streams || streams + coupled > 255) {
    GST_ELEMENT_ERROR(enc,
                      LIBRARY,
                      SETTINGS,
                      (nullptr),
                      ("Invalid Opus layout for %d channels: streams=%d coupled-streams=%d mapping=%s",
                       enc->channels,
                       streams,
                       coupled,
                       enc->mapping ? enc->mapping : "(identity)"));
    return FALSE;
  }

  int error = OPUS_OK;
  enc->encoder = opus_multistream_encoder_create(SAMPLE_RATE,
                                                 enc->channels,
                                                 streams,
                                                 coupled,
                                                 mapping.data(),
                                                 OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                                                 &error);
  if (error != OPUS_OK || !enc->encoder) {
    GST_ELEMENT_ERROR(enc, LIBRARY, INIT, (nullptr), ("Unable to create the Opus encoder: %s", opus_strerror(error)));
    return FALSE;
  }
  opus_multistream_encoder_ctl(enc->encoder, OPUS_SET_BITRATE(enc->bitrate));
  opus_multistream_encoder_ctl(enc->encoder, OPUS_SET_VBR(0)); // CBR, like the default opusenc settings
  opus_multistream_encoder_ctl(enc->encoder, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));

  enc->frame_samples = SAMPLE_RATE * enc->frame_size_ms / 1000;
  gst_audio_encoder_set_frame_samples_min(base, enc->frame_samples);
  gst_audio_encoder_set_frame_samples_max(base, enc->frame_samples);
  gst_audio_encoder_set_frame_max(base, 1);

  auto caps = gst_caps_new_simple("audio/x-opus",
                                  "rate",
                                  G_TYPE_INT,
                                  SAMPLE_RATE,
                                  "channels",
                                  G_TYPE_INT,
                                  enc->channels,
                                  "channel-mapping-family",
                                  G_TYPE_INT,
                                  255,
                                  "stream-count",
                                  G_TYPE_INT,
                                  streams,
                                  "coupled-count",
                                  G_TYPE_INT,
                                  coupled,
                                  nullptr);
  auto ok = gst_audio_encoder_set_output_format(base, caps);
  gst_caps_unref(caps);
  return ok;
}

static GstFlowReturn gst_moonlight_opus_enc_handle_frame(GstAudioEncoder *base, GstBuffer *buffer) {
  auto enc = GST_MOONLIGHT_OPUS_ENC(base);
  if (!buffer || !enc->encoder) { // Draining
    return GST_FLOW_OK;
  }

  GstMapInfo in;
  if (!gst_buffer_map(buffer, &in, GST_MAP_READ)) {
    return GST_FLOW_ERROR;
  }
  auto samples = static_cast<int>(in.size / (sizeof(opus_int16) * enc->channels));
  // Opus only encodes whole frames: pad the last (short) frame with silence
  std::vector<opus_int16> pcm(static_cast<size_t>(enc->frame_samples) * enc->channels, 0);
  std::memcpy(pcm.data(), in.data, std::min(in.size, pcm.size() * sizeof(opus_int16)));
  gst_buffer_unmap(buffer, &in);

  // 1275 bytes is the biggest possible Opus frame, per stream
  std::vector<unsigned char> packet(1275 * static_cast<size_t>(enc->channels) + 7);
  auto len = opus_multistream_encode(enc->encoder,
                                     pcm.data(),
                                     enc->frame_samples,
                                     packet.data(),
                                     static_cast<opus_int32>(packet.size()));
  if (len < 0) {
    GST_ELEMENT_ERROR(enc, STREAM, ENCODE, (nullptr), ("Opus encoding failed: %s", opus_strerror(len)));
    return GST_FLOW_ERROR;
  }

  auto out = gst_audio_encoder_allocate_output_buffer(base, len);
  gst_buffer_fill(out, 0, packet.data(), len);
  return gst_audio_encoder_finish_frame(base, out, std::min(samples, enc->frame_samples));
}

static gboolean gst_moonlight_opus_enc_stop(GstAudioEncoder *base) {
  free_encoder(GST_MOONLIGHT_OPUS_ENC(base));
  return TRUE;
}

static void
gst_moonlight_opus_enc_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  auto enc = GST_MOONLIGHT_OPUS_ENC(object);
  switch (prop_id) {
  case PROP_BITRATE:
    enc->bitrate = g_value_get_int(value);
    break;
  case PROP_FRAME_SIZE:
    enc->frame_size_ms = g_value_get_int(value);
    break;
  case PROP_STREAMS:
    enc->streams = g_value_get_int(value);
    break;
  case PROP_COUPLED_STREAMS:
    enc->coupled_streams = g_value_get_int(value);
    break;
  case PROP_MAPPING:
    g_free(enc->mapping);
    enc->mapping = g_value_dup_string(value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gst_moonlight_opus_enc_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  auto enc = GST_MOONLIGHT_OPUS_ENC(object);
  switch (prop_id) {
  case PROP_BITRATE:
    g_value_set_int(value, enc->bitrate);
    break;
  case PROP_FRAME_SIZE:
    g_value_set_int(value, enc->frame_size_ms);
    break;
  case PROP_STREAMS:
    g_value_set_int(value, enc->streams);
    break;
  case PROP_COUPLED_STREAMS:
    g_value_set_int(value, enc->coupled_streams);
    break;
  case PROP_MAPPING:
    g_value_set_string(value, enc->mapping);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gst_moonlight_opus_enc_finalize(GObject *object) {
  auto enc = GST_MOONLIGHT_OPUS_ENC(object);
  free_encoder(enc);
  g_free(enc->mapping);
  G_OBJECT_CLASS(gst_moonlight_opus_enc_parent_class)->finalize(object);
}

static void gst_moonlight_opus_enc_class_init(GstMoonlightOpusEncClass *klass) {
  auto gobject_class = G_OBJECT_CLASS(klass);
  auto element_class = GST_ELEMENT_CLASS(klass);
  auto encoder_class = GST_AUDIO_ENCODER_CLASS(klass);

  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);
  gst_element_class_set_static_metadata(element_class,
                                        "Moonlight Opus multistream encoder",
                                        "Codec/Encoder/Audio",
                                        "Opus multistream encoder with an explicit stream layout",
                                        "Heeler");

  gobject_class->set_property = gst_moonlight_opus_enc_set_property;
  gobject_class->get_property = gst_moonlight_opus_enc_get_property;
  gobject_class->finalize = gst_moonlight_opus_enc_finalize;

  encoder_class->set_format = GST_DEBUG_FUNCPTR(gst_moonlight_opus_enc_set_format);
  encoder_class->handle_frame = GST_DEBUG_FUNCPTR(gst_moonlight_opus_enc_handle_frame);
  encoder_class->stop = GST_DEBUG_FUNCPTR(gst_moonlight_opus_enc_stop);

  g_object_class_install_property(
      gobject_class,
      PROP_BITRATE,
      g_param_spec_int("bitrate", "bitrate", "Bitrate in bps", 6000, 4096000, 64000, G_PARAM_READWRITE));
  g_object_class_install_property(
      gobject_class,
      PROP_FRAME_SIZE,
      g_param_spec_int("frame-size", "frame-size", "Frame duration in ms (5, 10, 20)", 5, 20, 5, G_PARAM_READWRITE));
  g_object_class_install_property(gobject_class,
                                  PROP_STREAMS,
                                  g_param_spec_int("streams",
                                                   "streams",
                                                   "Number of Opus streams, 0 means one per channel",
                                                   0,
                                                   255,
                                                   0,
                                                   G_PARAM_READWRITE));
  g_object_class_install_property(gobject_class,
                                  PROP_COUPLED_STREAMS,
                                  g_param_spec_int("coupled-streams",
                                                   "coupled-streams",
                                                   "Number of coupled (stereo) streams",
                                                   0,
                                                   127,
                                                   0,
                                                   G_PARAM_READWRITE));
  g_object_class_install_property(gobject_class,
                                  PROP_MAPPING,
                                  g_param_spec_string("mapping",
                                                      "mapping",
                                                      "Comma separated channel to stream mapping, empty for identity",
                                                      nullptr,
                                                      G_PARAM_READWRITE));
}

static void gst_moonlight_opus_enc_init(GstMoonlightOpusEnc *enc) {
  enc->bitrate = 64000;
  enc->frame_size_ms = 5;
  enc->streams = 0;
  enc->coupled_streams = 0;
  enc->mapping = nullptr;
  enc->encoder = nullptr;
  enc->channels = 0;
  enc->frame_samples = 0;
}
