#pragma once

#include <gst/audio/gstaudioencoder.h>
#include <opus/opus_multistream.h>
#include <string>
#include <vector>

/**
 * moonlightopusenc: an Opus multistream encoder where the stream layout (streams, coupled streams and channel
 * mapping) is set explicitly.
 *
 * GStreamer's opusenc always picks the layout itself (for 5.1/7.1 it forces coupled streams), but Moonlight's
 * high quality surround mode needs one uncoupled stream per channel, with the exact layout advertised in the RTSP
 * DESCRIBE response.
 */

G_BEGIN_DECLS

#define GST_TYPE_MOONLIGHT_OPUS_ENC (gst_moonlight_opus_enc_get_type())
#define GST_MOONLIGHT_OPUS_ENC(obj)                                                                                    \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_MOONLIGHT_OPUS_ENC, GstMoonlightOpusEnc))

typedef struct _GstMoonlightOpusEnc GstMoonlightOpusEnc;
typedef struct _GstMoonlightOpusEncClass GstMoonlightOpusEncClass;

struct _GstMoonlightOpusEnc {
  GstAudioEncoder parent;

  /* Properties */
  int bitrate;
  int frame_size_ms;
  int streams;
  int coupled_streams;
  gchar *mapping; // comma separated channel -> stream mapping, NULL/empty means identity

  /* State */
  OpusMSEncoder *encoder;
  int channels;
  int frame_samples;
};

struct _GstMoonlightOpusEncClass {
  GstAudioEncoderClass parent_class;
};

GType gst_moonlight_opus_enc_get_type(void);

G_END_DECLS

namespace moonlight_opus {

/**
 * Parses a comma separated channel mapping ("0,1,2,3,4,5"), an empty string means identity.
 * Returns an empty vector when it's invalid for `channels` channels and `streams + coupled` decoded channels.
 */
std::vector<unsigned char> parse_mapping(const std::string &mapping, int channels, int streams, int coupled);

} // namespace moonlight_opus
