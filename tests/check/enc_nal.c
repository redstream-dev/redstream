/* rdsh264enc / rdsh265enc Annex-B helpers: filler stripping (positions, start-code lengths, both codecs, metas and
 * flags kept), the timestamp-shift removal on times and segments, the H.265 SPS conformance-window fix on SPSs
 * captured from vah265enc, intra access units on hand-coded slice headers, and the level strings and the level
 * rewrite on parameter sets captured from vah264enc and vah265enc, and every helper on every prefix of those and on
 * fixed-seed random buffers, which must neither crash nor read out of bounds (cpu); the test walker on streams from
 * the VA encoders, filler stripping on a real CBR stream, the VA encoders' measured shift and an encode → fix →
 * decode round trip (gpu).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-enc-nal.h"
#include "rds-test-media.h"
#include "rds-test-nal.h"

#include <string.h>

/* ------------------------------------------------------------------------------------------------------------ */
/* Captured SPSs */

/* HEVC SPS NAL units (2-byte header, no start code) from the stock vah265enc on an RDNA2 iGPU (Mesa radeonsi, VCN 3)
 * in the redstream:dev container: `videotestsrc num-buffers=3 ! video/x-raw,format=NV12,width=W,height=H,
 * framerate=30/1 ! vah265enc ! video/x-h265,stream-format=byte-stream,alignment=au ! filesink`, the SPS cut out of
 * the first access unit. pic_width is padded to a multiple of 64 but the window computed from a multiple of 16:
 * 720x480 is coded 768x480 with no window (decodes 768 wide), 1366x768 1408x768 with right offset 5 (1398),
 * 1000x600 1024x608 with right 4 and bottom 4 (1016x600); 1280x720 is right. */
static const guint8 sps_720x480[] = {
  0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x5a, 0xa0, 0x06, 0x02, 0x01, 0xe1, 0x68, 0xb9, 0x24, 0x29, 0x5a, 0x7f, 0xf0, 0x00, 0x10,
  0x00, 0x10, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x01, 0xe0, 0x80,
};

static const guint8 sps_1366x768[] = {
  0x42, 0x01, 0x01, 0x21, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x78, 0xa0, 0x02, 0xc0, 0x80, 0x30, 0x1c, 0xde, 0x8b, 0x92, 0x42, 0x95, 0xa7, 0xff, 0x00,
  0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x1e, 0x08,
};

static const guint8 sps_1000x600[] = {
  0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x5d, 0xa0, 0x02, 0x00, 0x80, 0x26, 0x1c, 0xb2, 0xe8, 0xb9, 0x24, 0x29, 0x5a, 0x7f, 0xf0,
  0x00, 0x10, 0x00, 0x10, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x01, 0xe0, 0x80,
};

static const guint8 sps_1280x720[] = {
  0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x5d, 0xa0, 0x02, 0x80, 0x80, 0x2d, 0x16, 0x8b, 0x92, 0x42, 0x95, 0xa7, 0xff, 0x00, 0x01,
  0x00, 0x01, 0x01, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x1e, 0x08,
};

/* H.264 SPS NAL units from vah264enc the same way: 1920x1080 (coded 1920x1088, crop bottom 4) and 1281x721 (coded
 * 1296x736, crop right 7 and bottom 7: 1282x722, the next even size). */
static const guint8 h264_sps_1920x1080[] = {
  0x27, 0x64, 0x00, 0x28, 0xac, 0x56, 0x80, 0x78, 0x02, 0x27, 0xe5, 0xff, 0xc0, 0x00, 0x40, 0x00,
  0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xf2, 0x7c, 0x22, 0x11, 0xb8,
};

static const guint8 h264_sps_1281x721[] = {
  0x27, 0x64, 0x00, 0x20, 0xac, 0x56, 0x80, 0x51, 0x05, 0xde, 0x22, 0x23, 0xff, 0x00, 0x01, 0x00,
  0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xc9, 0xf0, 0x88, 0x46, 0xe0,
};

/* Parameter sets at 1280x720 (30 fps) for the level rewrite, captured the same way: vah264enc's SPS with no profile
 * downstream (High, level 3.1) and with `profile=constrained-baseline` downstream (profile_idc 66 with
 * constraint_set0 and set1, level 3.1); vah265enc's VPS (level 3.1; its SPS is sps_1280x720, also 3.1). */
static const guint8 h264_sps_high_720p[] = {
  0x27, 0x64, 0x00, 0x1f, 0xac, 0x56, 0x80, 0x50, 0x05, 0xbb, 0xff, 0x00, 0x01, 0x00, 0x01, 0x10,
  0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xc9, 0xf0, 0x88, 0x46, 0xe0,
};

static const guint8 h264_sps_cb_720p[] = {
  0x27, 0x42, 0xc0, 0x1f, 0xab, 0x40, 0x28, 0x02, 0xdd, 0xff, 0x80, 0x00, 0x80, 0x00, 0x88, 0x00,
  0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0xe4, 0xf8, 0x44, 0x23, 0x70,
};

static const guint8 vps_1280x720[] = {
  0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x5d, 0x2c, 0x09,
};

/* ------------------------------------------------------------------------------------------------------------ */
/* Buffers */

#define TEST_PTS (5 * GST_SECOND)
#define TEST_DTS (4 * GST_SECOND)
#define TEST_DURATION (GST_SECOND / 30)
#define TEST_FLAGS (GST_BUFFER_FLAG_DISCONT | GST_BUFFER_FLAG_DELTA_UNIT | GST_BUFFER_FLAG_HEADER)

static GstCaps *ref_caps;

/* A buffer of @bytes with timestamps, offsets, flags and two reference-timestamp metas. */
static GstBuffer *
make_buffer (const guint8 * bytes, gsize size)
{
  GstBuffer *b = size > 0 ? gst_buffer_new_memdup (bytes, size) : gst_buffer_new ();

  GST_BUFFER_PTS (b) = TEST_PTS;
  GST_BUFFER_DTS (b) = TEST_DTS;
  GST_BUFFER_DURATION (b) = TEST_DURATION;
  GST_BUFFER_OFFSET (b) = 7;
  GST_BUFFER_OFFSET_END (b) = 8;
  GST_BUFFER_FLAG_SET (b, TEST_FLAGS);
  gst_buffer_add_reference_timestamp_meta (b, ref_caps, 11 * GST_SECOND, GST_CLOCK_TIME_NONE);
  gst_buffer_add_reference_timestamp_meta (b, ref_caps, 12 * GST_SECOND, 3);
  return b;
}

static void
assert_like_input (GstBuffer * out)
{
  GstReferenceTimestampMeta *m;
  gpointer state = NULL;
  guint n = 0;

  g_assert_cmpuint (GST_BUFFER_PTS (out), ==, TEST_PTS);
  g_assert_cmpuint (GST_BUFFER_DTS (out), ==, TEST_DTS);
  g_assert_cmpuint (GST_BUFFER_DURATION (out), ==, TEST_DURATION);
  g_assert_cmpuint (GST_BUFFER_OFFSET (out), ==, 7);
  g_assert_cmpuint (GST_BUFFER_OFFSET_END (out), ==, 8);
  g_assert_cmphex (GST_BUFFER_FLAGS (out) & TEST_FLAGS, ==, TEST_FLAGS);
  while ((m = (GstReferenceTimestampMeta *) gst_buffer_iterate_meta_filtered (out, &state,
              GST_REFERENCE_TIMESTAMP_META_API_TYPE)) != NULL) {
    g_assert_true (gst_caps_is_equal (m->reference, ref_caps));
    g_assert_cmpuint (m->timestamp, ==, (11 + n) * GST_SECOND);
    n++;
  }
  g_assert_cmpuint (n, ==, 2);
}

static void
assert_buffer_bytes (GstBuffer * b, const guint8 * bytes, gsize size)
{
  GstMapInfo map;

  g_assert_true (gst_buffer_map (b, &map, GST_MAP_READ));
  g_assert_cmpmem (map.data, map.size, bytes, size);
  gst_buffer_unmap (b, &map);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Filler stripping */

/* A byte stream part: a start code of 3 or 4 bytes (0: none) and a NAL unit. */
typedef struct
{
  guint sc;
  const guint8 *nal;
  gsize size;
  gboolean filler;
} Part;

static const guint8 h264_sps[] = { 0x67, 0x64, 0x00, 0x1f, 0xac };
static const guint8 h264_pps[] = { 0x68, 0xee, 0x3c, 0x80 };
static const guint8 h264_idr[] = { 0x65, 0x88, 0x84, 0x00, 0x10 };
static const guint8 h264_fill[] = { 0x0c, 0xff, 0xff, 0xff, 0x80 };
static const guint8 h265_vps[] = { 0x40, 0x01, 0x0c, 0x01 };
static const guint8 h265_sps[] = { 0x42, 0x01, 0x01, 0x60 };
static const guint8 h265_slice[] = { 0x26, 0x01, 0xaf, 0x00, 0x05 };
static const guint8 h265_fill[] = { 0x4c, 0x01, 0xff, 0xff, 0x80 };

static GByteArray *
join (const Part * parts, guint n, gboolean keep_filler)
{
  static const guint8 sc4[] = { 0, 0, 0, 1 };
  GByteArray *a = g_byte_array_new ();

  for (guint i = 0; i < n; i++) {
    if (parts[i].filler && !keep_filler)
      continue;
    if (parts[i].sc > 0)
      g_byte_array_append (a, sc4 + 4 - parts[i].sc, parts[i].sc);
    g_byte_array_append (a, parts[i].nal, (guint) parts[i].size);
  }
  return a;
}

#define P(sc, nal, f) { sc, nal, sizeof (nal), f }

/* Strips @parts (with their fillers) and checks the result is @parts without the fillers, metas and flags kept;
 * the same buffer back when there is no filler. */
static void
check_strip (const Part * parts, guint n, RdsEncCodec codec)
{
  GByteArray *in = join (parts, n, TRUE), *want = join (parts, n, FALSE);
  GstBuffer *b = make_buffer (in->data, in->len), *out = rds_enc_strip_filler (b, codec);
  gboolean any = FALSE;

  for (guint i = 0; i < n; i++)
    any |= parts[i].filler;
  if (any) {
    g_assert_true (out != b);
    assert_like_input (out);
  } else {
    g_assert_true (out == b);
    g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (b), ==, 2);
  }
  assert_buffer_bytes (out, want->data, want->len);
  gst_buffer_unref (out);
  gst_buffer_unref (b);
  g_byte_array_unref (in);
  g_byte_array_unref (want);
}

static void
test_filler_positions (gconstpointer data)
{
  RdsEncCodec codec = GPOINTER_TO_INT (data);
  gboolean hevc = codec == RDS_ENC_CODEC_H265;
  const guint8 *a = hevc ? h265_vps : h264_sps, *b = hevc ? h265_sps : h264_pps, *c = hevc ? h265_slice : h264_idr;
  const guint8 *f = hevc ? h265_fill : h264_fill;
  gsize la = hevc ? sizeof (h265_vps) : sizeof (h264_sps), lb = hevc ? sizeof (h265_sps) : sizeof (h264_pps);
  gsize lc = hevc ? sizeof (h265_slice) : sizeof (h264_idr), lf = sizeof (h264_fill);

  for (guint sc = 3; sc <= 4; sc++) {
    for (guint sc2 = 3; sc2 <= 4; sc2++) {
      const Part none[] = { {sc, a, la, FALSE}, {sc2, b, lb, FALSE}, {sc, c, lc, FALSE} };
      const Part first[] = { {sc, f, lf, TRUE}, {sc2, a, la, FALSE}, {sc, c, lc, FALSE} };
      const Part middle[] = { {sc, a, la, FALSE}, {sc2, f, lf, TRUE}, {sc, b, lb, FALSE}, {sc2, c, lc, FALSE} };
      const Part last[] = { {sc, a, la, FALSE}, {sc2, c, lc, FALSE}, {sc, f, lf, TRUE} };
      const Part all[] = { {sc, f, lf, TRUE}, {sc2, f, lf, TRUE}, {sc, f, lf, TRUE} };
      const Part several[] = { {sc, f, lf, TRUE}, {sc2, a, la, FALSE}, {sc, f, lf, TRUE}, {sc2, f, lf, TRUE},
        {sc, c, lc, FALSE}, {sc2, f, lf, TRUE} };

      check_strip (none, G_N_ELEMENTS (none), codec);
      check_strip (first, G_N_ELEMENTS (first), codec);
      check_strip (middle, G_N_ELEMENTS (middle), codec);
      check_strip (last, G_N_ELEMENTS (last), codec);
      check_strip (all, G_N_ELEMENTS (all), codec);
      check_strip (several, G_N_ELEMENTS (several), codec);
    }
  }
}

/* Strips @in and compares with @want byte for byte. */
static void
check_strip_bytes (const guint8 * in, gsize in_size, const guint8 * want, gsize want_size, RdsEncCodec codec)
{
  GstBuffer *b = make_buffer (in, in_size), *out = rds_enc_strip_filler (b, codec);

  if (in_size == want_size && memcmp (in, want, in_size) == 0)
    g_assert_true (out == b);
  else
    assert_like_input (out);
  assert_buffer_bytes (out, want, want_size);
  gst_buffer_unref (out);
  gst_buffer_unref (b);
}

#define CHECK_STRIP(in, want, codec) check_strip_bytes (in, sizeof (in), want, sizeof (want), codec)

static void
test_filler_zero_bytes (void)
{
  /* zero bytes after a NAL unit belong to none: the filler takes those before it (the SPS's trailing zeros and its
   * own zero_byte) and after it, but the slice keeps the zero_byte of its 4-byte start code */
  const guint8 in1[] = { 0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0, 0, 0, 0, 1, 0x65, 0x88 };
  const guint8 want1[] = { 0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x65, 0x88 };
  /* a 3-byte start code after the filler stays 3 bytes */
  const guint8 in2[] = { 0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0, 1, 0x65, 0x88 };
  const guint8 want2[] = { 0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x65, 0x88 };
  /* leading zero bytes before a first filler go with it */
  const guint8 in3[] = { 0, 0, 0, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0, 1, 0x65, 0x88 };
  const guint8 want3[] = { 0, 0, 1, 0x65, 0x88 };
  /* trailing zero bytes after a last filler, and the zeros before it, go with it */
  const guint8 in4[] = { 0, 0, 1, 0x65, 0x88, 0, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0 };
  const guint8 want4[] = { 0, 0, 1, 0x65, 0x88 };
  /* trailing zero bytes after a kept NAL unit stay when there is no filler: same buffer */
  const guint8 in5[] = { 0, 0, 1, 0x65, 0x88, 0, 0 };
  /* bytes before the first start code are kept */
  const guint8 in6[] = { 0x55, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0, 1, 0x65, 0x88 };
  const guint8 want6[] = { 0x55, 0, 0, 1, 0x65, 0x88 };
  /* emulation prevention inside a kept NAL unit is untouched */
  const guint8 in7[] = { 0, 0, 1, 0x65, 0x00, 0x00, 0x03, 0x01, 0, 0, 1, 0x0c, 0xff, 0x80 };
  const guint8 want7[] = { 0, 0, 1, 0x65, 0x00, 0x00, 0x03, 0x01 };

  /* adjacent fillers, the second with a zero run longer than one byte before a 3-byte start code */
  const guint8 in8[] = { 0, 0, 1, 0x65, 0x88, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0, 0, 0, 1, 0x0c, 0xff, 0x80,
    0, 0, 1, 0x65, 0x99 };
  const guint8 want8[] = { 0, 0, 1, 0x65, 0x88, 0, 0, 1, 0x65, 0x99 };
  /* three adjacent fillers with zero runs of 3, 1 and 4 bytes before their start codes, then a 4-byte slice */
  const guint8 in9[] = { 0, 0, 0, 1, 0x65, 0x88, 0, 0, 0, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0, 0, 1, 0x0c, 0x80,
    0, 0, 0, 0, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0, 0, 1, 0x65, 0x99 };
  const guint8 want9[] = { 0, 0, 0, 1, 0x65, 0x88, 0, 0, 0, 1, 0x65, 0x99 };
  /* filler, zeros, filler at the end of the buffer (with trailing zeros) */
  const guint8 in10[] = { 0, 0, 1, 0x65, 0x88, 0, 0, 1, 0x0c, 0xff, 0x80, 0, 0, 0, 0, 0, 1, 0x0c, 0x80, 0, 0 };
  const guint8 want10[] = { 0, 0, 1, 0x65, 0x88 };
  /* the same with nothing but fillers and zero runs: empty */
  const guint8 in11[] = { 0, 0, 0, 1, 0x0c, 0x80, 0, 0, 0, 0, 1, 0x0c, 0x80, 0, 0, 1, 0x0c, 0x80, 0 };

  CHECK_STRIP (in8, want8, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in9, want9, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in10, want10, RDS_ENC_CODEC_H264);
  check_strip_bytes (in11, sizeof (in11), NULL, 0, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in1, want1, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in2, want2, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in3, want3, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in4, want4, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in5, in5, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in6, want6, RDS_ENC_CODEC_H264);
  CHECK_STRIP (in7, want7, RDS_ENC_CODEC_H264);
}

static void
test_filler_codec (void)
{
  /* 0x0c is H.264 FILLER (12) but HEVC RASL_N (6); 0x4c 0x01 is HEVC FD_NUT (38) and also H.264 type 12 */
  const guint8 h264_only[] = { 0, 0, 1, 0x0c, 0x01, 0xff, 0x80, 0, 0, 1, 0x26, 0x01, 0xaf };
  const guint8 h264_only_stripped[] = { 0, 0, 1, 0x26, 0x01, 0xaf };
  const guint8 hevc_slice_only[] = { 0, 0, 1, 0x02, 0x01, 0xaf, 0, 0, 1, 0x4c, 0x01, 0xff, 0x80 };
  const guint8 hevc_slice_only_stripped[] = { 0, 0, 1, 0x02, 0x01, 0xaf };
  const guint8 empty_sc[] = { 0, 0, 1 };

  CHECK_STRIP (h264_only, h264_only_stripped, RDS_ENC_CODEC_H264);
  CHECK_STRIP (h264_only, h264_only, RDS_ENC_CODEC_H265);
  CHECK_STRIP (hevc_slice_only, hevc_slice_only_stripped, RDS_ENC_CODEC_H265);
  /* no start code at all, or a lone start code: same buffer */
  CHECK_STRIP (h265_fill, h265_fill, RDS_ENC_CODEC_H265);
  CHECK_STRIP (empty_sc, empty_sc, RDS_ENC_CODEC_H265);
}

static void
test_filler_all (void)
{
  const guint8 in[] = { 0, 0, 0, 1, 0x4c, 0x01, 0xff, 0x80, 0, 0, 1, 0x4c, 0x01, 0x80, 0 };
  GstBuffer *b = make_buffer (in, sizeof (in)), *out = rds_enc_strip_filler (b, RDS_ENC_CODEC_H265);

  g_assert_true (out != b);
  g_assert_cmpuint (gst_buffer_get_size (out), ==, 0);
  assert_like_input (out);
  gst_buffer_unref (out);
  gst_buffer_unref (b);

  /* an empty buffer: same buffer */
  b = make_buffer (NULL, 0);
  out = rds_enc_strip_filler (b, RDS_ENC_CODEC_H264);
  g_assert_true (out == b);
  gst_buffer_unref (out);
  gst_buffer_unref (b);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Timestamp shift */

#define HOURS_1000 (G_GUINT64_CONSTANT (3600000) * GST_SECOND)

static void
test_ts_unshift (void)
{
  g_assert_cmpuint (rds_enc_ts_unshift (10 * GST_SECOND, 0), ==, 10 * GST_SECOND);
  g_assert_cmpuint (rds_enc_ts_unshift (HOURS_1000, HOURS_1000), ==, 0);
  g_assert_cmpuint (rds_enc_ts_unshift (HOURS_1000 + GST_SECOND, HOURS_1000), ==, GST_SECOND);
  g_assert_cmpuint (rds_enc_ts_unshift (HOURS_1000, HOURS_1000 - 10 * GST_SECOND), ==, 10 * GST_SECOND);
  /* invalid stays invalid */
  g_assert_cmpuint (rds_enc_ts_unshift (GST_CLOCK_TIME_NONE, HOURS_1000), ==, GST_CLOCK_TIME_NONE);
  g_assert_cmpuint (rds_enc_ts_unshift (GST_CLOCK_TIME_NONE, 0), ==, GST_CLOCK_TIME_NONE);
  /* below the shift: clamped to 0 (ours), never wrapped */
  g_assert_cmpuint (rds_enc_ts_unshift (HOURS_1000 - 1, HOURS_1000), ==, 0);
  g_assert_cmpuint (rds_enc_ts_unshift (0, 1), ==, 0);
  /* an invalid shift changes nothing */
  g_assert_cmpuint (rds_enc_ts_unshift (5, GST_CLOCK_TIME_NONE), ==, 5);
}

static void
test_segment_shift (void)
{
  GstSegment in, out;

  gst_segment_init (&in, GST_FORMAT_TIME);
  gst_segment_init (&out, GST_FORMAT_TIME);
  g_assert_cmpuint (rds_enc_segment_shift (&in, &out), ==, 0);
  /* as VA does for a first PTS of 0 and of 10 s */
  out.start = HOURS_1000;
  g_assert_cmpuint (rds_enc_segment_shift (&in, &out), ==, HOURS_1000);
  out.start = HOURS_1000 - 10 * GST_SECOND;
  g_assert_cmpuint (rds_enc_segment_shift (&in, &out), ==, HOURS_1000 - 10 * GST_SECOND);
  /* an input segment that doesn't start at 0 */
  in.start = 2 * GST_SECOND;
  out.start = HOURS_1000 + 2 * GST_SECOND;
  g_assert_cmpuint (rds_enc_segment_shift (&in, &out), ==, HOURS_1000);
  /* output before input, or not TIME: no shift */
  out.start = GST_SECOND;
  g_assert_cmpuint (rds_enc_segment_shift (&in, &out), ==, 0);
  gst_segment_init (&out, GST_FORMAT_BYTES);
  out.start = HOURS_1000;
  g_assert_cmpuint (rds_enc_segment_shift (&in, &out), ==, 0);
}

static void
test_segment_unshift (void)
{
  GstClockTime shift = HOURS_1000 - 10 * GST_SECOND;
  GstSegment in, out, orig;

  /* the input segment, and the encoder's output segment as VA makes it: start moved by the shift */
  gst_segment_init (&in, GST_FORMAT_TIME);
  in.base = 7 * GST_SECOND;
  in.time = 3 * GST_SECOND;
  in.stop = 60 * GST_SECOND;
  in.position = 12 * GST_SECOND;
  in.duration = 100 * GST_SECOND;
  out = in;
  out.start += shift;
  out.stop += shift;
  out.position += shift;
  g_assert_cmpuint (rds_enc_segment_shift (&in, &out), ==, shift);

  rds_enc_segment_unshift (&out, shift);
  g_assert_true (gst_segment_is_equal (&out, &in));
  g_assert_cmpuint (out.position, ==, in.position);
  g_assert_cmpuint (out.duration, ==, in.duration);

  /* running and stream time of unshifted buffers equal the input's */
  out = in;
  out.start += shift;
  out.stop += shift;
  for (GstClockTime t = 10 * GST_SECOND; t < 20 * GST_SECOND; t += GST_SECOND / 3) {
    GstClockTime rt_shifted = gst_segment_to_running_time (&out, GST_FORMAT_TIME, t + shift);
    GstSegment un = out;

    rds_enc_segment_unshift (&un, shift);
    g_assert_cmpuint (gst_segment_to_running_time (&un, GST_FORMAT_TIME, rds_enc_ts_unshift (t + shift, shift)),
        ==, gst_segment_to_running_time (&in, GST_FORMAT_TIME, t));
    g_assert_cmpuint (rt_shifted, ==, gst_segment_to_running_time (&in, GST_FORMAT_TIME, t));
    g_assert_cmpuint (gst_segment_to_stream_time (&un, GST_FORMAT_TIME, t), ==,
        gst_segment_to_stream_time (&in, GST_FORMAT_TIME, t));
  }

  /* open stop, NONE position: stay NONE */
  gst_segment_init (&out, GST_FORMAT_TIME);
  out.start = shift;
  out.position = GST_CLOCK_TIME_NONE;
  rds_enc_segment_unshift (&out, shift);
  g_assert_cmpuint (out.start, ==, 0);
  g_assert_cmpuint (out.stop, ==, GST_CLOCK_TIME_NONE);
  g_assert_cmpuint (out.position, ==, GST_CLOCK_TIME_NONE);

  /* shift 0, and other formats: unchanged */
  orig = in;
  rds_enc_segment_unshift (&in, 0);
  g_assert_true (gst_segment_is_equal (&in, &orig));
  gst_segment_init (&out, GST_FORMAT_BYTES);
  out.start = 1000;
  orig = out;
  rds_enc_segment_unshift (&out, 10);
  g_assert_true (gst_segment_is_equal (&out, &orig));
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Walker on captured SPSs */

static void
test_walker_sps (void)
{
  RdsTestSps sps;

  rds_test_h265_sps (sps_720x480, sizeof (sps_720x480), &sps);
  g_assert_cmpuint (sps.profile_idc, ==, 1);    /* Main */
  g_assert_cmpuint (sps.level_idc, ==, 90);     /* 3.0 */
  g_assert_cmpuint (sps.chroma_format_idc, ==, 1);
  g_assert_cmpuint (sps.coded_width, ==, 768);
  g_assert_cmpuint (sps.coded_height, ==, 480);
  g_assert_false (sps.cropping);
  g_assert_cmpuint (sps.width, ==, 768);
  g_assert_cmpuint (sps.height, ==, 480);

  rds_test_h265_sps (sps_1366x768, sizeof (sps_1366x768), &sps);
  g_assert_cmpuint (sps.coded_width, ==, 1408);
  g_assert_cmpuint (sps.coded_height, ==, 768);
  g_assert_cmpuint (sps.width, ==, 1398);
  g_assert_cmpuint (sps.height, ==, 768);

  rds_test_h265_sps (sps_1000x600, sizeof (sps_1000x600), &sps);
  g_assert_cmpuint (sps.coded_width, ==, 1024);
  g_assert_cmpuint (sps.coded_height, ==, 608);
  g_assert_true (sps.cropping);
  g_assert_cmpuint (sps.crop_right, ==, 4);
  g_assert_cmpuint (sps.crop_bottom, ==, 4);
  g_assert_cmpuint (sps.width, ==, 1016);
  g_assert_cmpuint (sps.height, ==, 600);

  rds_test_h265_sps (sps_1280x720, sizeof (sps_1280x720), &sps);
  g_assert_cmpuint (sps.width, ==, 1280);
  g_assert_cmpuint (sps.height, ==, 720);

  rds_test_h264_sps (h264_sps_1920x1080, sizeof (h264_sps_1920x1080), &sps);
  g_assert_cmpuint (sps.profile_idc, ==, 100);  /* High */
  g_assert_cmpuint (sps.level_idc, ==, 40);
  g_assert_cmpuint (sps.coded_width, ==, 1920);
  g_assert_cmpuint (sps.coded_height, ==, 1088);
  g_assert_true (sps.cropping);
  g_assert_cmpuint (sps.crop_bottom, ==, 4);
  g_assert_cmpuint (sps.width, ==, 1920);
  g_assert_cmpuint (sps.height, ==, 1080);

  rds_test_h264_sps (h264_sps_1281x721, sizeof (h264_sps_1281x721), &sps);
  g_assert_cmpuint (sps.coded_width, ==, 1296);
  g_assert_cmpuint (sps.coded_height, ==, 736);
  g_assert_cmpuint (sps.width, ==, 1282);
  g_assert_cmpuint (sps.height, ==, 722);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* H.265 SPS fix */

/* Fixes @nal for @w x @h and checks the result re-parses to the next even size, with everything else unchanged. */
static void
check_sps_fixed (const guint8 * nal, gsize size, guint w, guint h)
{
  RdsTestSps before, after;
  guint8 *fixed = NULL;
  gsize fixed_size = 0;
  gchar *tail_before, *tail_after;

  g_assert_cmpint (rds_enc_h265_fix_sps (nal, size, w, h, &fixed, &fixed_size), ==, RDS_ENC_SPS_FIXED);
  g_assert_nonnull (fixed);
  g_assert_cmpmem (fixed, 2, nal, 2);   /* the NAL header */
  rds_test_h265_sps (nal, size, &before);
  rds_test_h265_sps (fixed, fixed_size, &after);
  g_assert_cmpuint (after.width, ==, (w + 1) & ~1u);
  g_assert_cmpuint (after.height, ==, (h + 1) & ~1u);
  g_assert_cmpuint (after.coded_width, ==, before.coded_width);
  g_assert_cmpuint (after.coded_height, ==, before.coded_height);
  g_assert_cmpuint (after.profile_idc, ==, before.profile_idc);
  g_assert_cmpuint (after.level_idc, ==, before.level_idc);
  g_assert_cmpuint (after.crop_left, ==, before.crop_left);
  g_assert_cmpuint (after.crop_top, ==, before.crop_top);
  g_assert_cmpuint (after.cropping, ==, after.width != after.coded_width || after.height != after.coded_height);
  /* every bit after the window is the same */
  g_assert_cmpuint (rds_test_nal_count (fixed, fixed_size, TRUE, 0), ==, 0);   /* no start code emulated */
  tail_before = rds_test_h265_sps_tail (nal, size);
  tail_after = rds_test_h265_sps_tail (fixed, fixed_size);
  g_assert_cmpstr (tail_after, ==, tail_before);
  {
    /* and every bit before conformance_window_flag */
    GByteArray *rb = rds_test_nal_rbsp (nal, size), *ra = rds_test_nal_rbsp (fixed, fixed_size);

    g_assert_cmpuint (after.window_bit, ==, before.window_bit);
    for (gsize i = 0; i < before.window_bit; i++)
      g_assert_cmpuint ((rb->data[i / 8] >> (7 - i % 8)) & 1, ==, (ra->data[i / 8] >> (7 - i % 8)) & 1);
    g_byte_array_unref (rb);
    g_byte_array_unref (ra);
  }
  g_free (tail_before);
  g_free (tail_after);
  g_free (fixed);
}

static void
test_sps_fix_captured (void)
{
  guint8 *fixed = (guint8 *) "untouched";
  gsize fixed_size = 99;

  check_sps_fixed (sps_720x480, sizeof (sps_720x480), 720, 480);
  check_sps_fixed (sps_1366x768, sizeof (sps_1366x768), 1366, 768);
  check_sps_fixed (sps_1000x600, sizeof (sps_1000x600), 1000, 600);
  /* odd sizes: the next even size */
  check_sps_fixed (sps_1000x600, sizeof (sps_1000x600), 999, 599);
  check_sps_fixed (sps_720x480, sizeof (sps_720x480), 719, 479);
  /* a smaller size than VA was given still fits the coded size */
  check_sps_fixed (sps_1280x720, sizeof (sps_1280x720), 1000, 700);

  /* already right: unchanged, outputs untouched */
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_1280x720, sizeof (sps_1280x720), 1280, 720, &fixed, &fixed_size), ==,
      RDS_ENC_SPS_UNCHANGED);
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_1280x720, sizeof (sps_1280x720), 1279, 719, &fixed, &fixed_size), ==,
      RDS_ENC_SPS_UNCHANGED);
  g_assert_cmpstr ((const gchar *) fixed, ==, "untouched");
  g_assert_cmpuint (fixed_size, ==, 99);
}

static void
test_sps_fix_twice (void)
{
  guint8 *fixed = NULL, *fixed2 = NULL;
  gsize size = 0, size2 = 0;

  /* a fixed SPS is right; fixing it for another size works on the rewritten window too */
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_1000x600, sizeof (sps_1000x600), 1000, 600, &fixed, &size), ==,
      RDS_ENC_SPS_FIXED);
  g_assert_cmpint (rds_enc_h265_fix_sps (fixed, size, 1000, 600, &fixed2, &size2), ==, RDS_ENC_SPS_UNCHANGED);
  check_sps_fixed (fixed, size, 1024, 608);     /* window removed: flag 0 */
  g_assert_cmpint (rds_enc_h265_fix_sps (fixed, size, 1024, 608, &fixed2, &size2), ==, RDS_ENC_SPS_FIXED);
  {
    RdsTestSps s;

    rds_test_h265_sps (fixed2, size2, &s);
    g_assert_false (s.cropping);
  }
  g_free (fixed2);
  g_free (fixed);
}

static void
test_sps_fix_invalid (void)
{
  guint8 *fixed = NULL;
  gsize size = 0;
  guint8 trunc[12];

  /* larger than the coded size */
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_720x480, sizeof (sps_720x480), 770, 480, &fixed, &size), ==,
      RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_720x480, sizeof (sps_720x480), 720, 482, &fixed, &size), ==,
      RDS_ENC_SPS_INVALID);
  /* not an SPS, too short, truncated, no size */
  g_assert_cmpint (rds_enc_h265_fix_sps (h265_vps, sizeof (h265_vps), 720, 480, &fixed, &size), ==,
      RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_720x480, 2, 720, 480, &fixed, &size), ==, RDS_ENC_SPS_INVALID);
  memcpy (trunc, sps_720x480, sizeof (trunc));
  g_assert_cmpint (rds_enc_h265_fix_sps (trunc, sizeof (trunc), 720, 480, &fixed, &size), ==, RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_720x480, 22, 720, 480, &fixed, &size), ==, RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_720x480, sizeof (sps_720x480), 0, 480, &fixed, &size), ==,
      RDS_ENC_SPS_INVALID);
  /* forbidden_zero_bit set, or a layer other than the base layer (nuh_layer_id's high bit in byte 0, its low five
   * bits in byte 1) */
  {
    guint8 bad[sizeof (sps_720x480)];

    memcpy (bad, sps_720x480, sizeof (bad));
    bad[0] |= 0x80;
    g_assert_cmpint (rds_enc_h265_fix_sps (bad, sizeof (bad), 720, 480, &fixed, &size), ==, RDS_ENC_SPS_INVALID);
    memcpy (bad, sps_720x480, sizeof (bad));
    bad[0] |= 0x01;             /* nuh_layer_id 32 */
    g_assert_cmpint (rds_enc_h265_fix_sps (bad, sizeof (bad), 720, 480, &fixed, &size), ==, RDS_ENC_SPS_INVALID);
    memcpy (bad, sps_720x480, sizeof (bad));
    bad[1] = (1 << 3) | 1;      /* nuh_layer_id 1, nuh_temporal_id_plus1 1 */
    g_assert_cmpint (rds_enc_h265_fix_sps (bad, sizeof (bad), 720, 480, &fixed, &size), ==, RDS_ENC_SPS_INVALID);
    memcpy (bad, sps_720x480, sizeof (bad));
    bad[1] = 2;                 /* another temporal id is fine */
    g_assert_cmpint (rds_enc_h265_fix_sps (bad, sizeof (bad), 720, 480, &fixed, &size), ==, RDS_ENC_SPS_FIXED);
    g_clear_pointer (&fixed, g_free);
  }
  g_assert_null (fixed);
}

/* An SPS whose rewrite moves emulation prevention: the 720x480 SPS's RBSP up to the conformance window flag, then
 * @tail_bits ('0'/'1') of our own (not a valid VUI, which the fix doesn't read), then the trailing bits, escaped by
 * the test's own packing. The bits the window adds move the tail's zero runs against the byte boundaries. */
static GByteArray *
synthetic_sps (const gchar * tail_bits)
{
  RdsTestSps s;
  GByteArray *rbsp, *nal = g_byte_array_new ();
  GString *bits = g_string_new (NULL);
  guint8 byte = 0;
  guint zeros = 0;

  rds_test_h265_sps (sps_720x480, sizeof (sps_720x480), &s);
  g_assert_false (s.cropping);
  rbsp = rds_test_nal_rbsp (sps_720x480, sizeof (sps_720x480));
  for (gsize i = 16; i < s.tail_bit; i++)       /* after the header, through conformance_window_flag (0) */
    g_string_append_c (bits, '0' + ((rbsp->data[i / 8] >> (7 - i % 8)) & 1));
  g_string_append (bits, tail_bits);
  g_string_append_c (bits, '1');        /* rbsp_stop_one_bit */
  while (bits->len % 8)
    g_string_append_c (bits, '0');
  g_byte_array_append (nal, sps_720x480, 2);
  for (gsize i = 0; i < bits->len; i++) {
    byte = (guint8) ((byte << 1) | (bits->str[i] == '1'));
    if (i % 8 == 7) {
      if (zeros >= 2 && byte <= 3) {
        static const guint8 epb = 3;

        g_byte_array_append (nal, &epb, 1);
        zeros = 0;
      }
      zeros = byte == 0 ? zeros + 1 : 0;
      g_byte_array_append (nal, &byte, 1);
      byte = 0;
    }
  }
  g_string_free (bits, TRUE);
  g_byte_array_unref (rbsp);
  return nal;
}

static void
test_sps_fix_emulation (void)
{
  gboolean seen_more = FALSE, seen_fewer = FALSE;

  /* zero runs of every length 16..40 behind a 1 bit, after an 8-bit pattern: some need an EPB only before the
   * rewrite, some only after */
  for (guint run = 16; run <= 40; run++) {
    for (guint lead = 0; lead < 8; lead++) {
      GString *tail = g_string_new (NULL);
      GByteArray *nal;
      guint8 *fixed = NULL;
      gsize size = 0;
      guint epb_in, epb_out;
      gchar *t_in, *t_out;
      RdsTestSps s;

      for (guint k = 0; k < lead; k++)
        g_string_append_c (tail, '1');
      for (guint k = 0; k < run; k++)
        g_string_append_c (tail, '0');
      g_string_append (tail, "1011");
      for (guint k = 0; k < run; k++)
        g_string_append_c (tail, '0');
      g_string_append_c (tail, '1');
      nal = synthetic_sps (tail->str);
      g_assert_cmpint (rds_enc_h265_fix_sps (nal->data, nal->len, 720, 480, &fixed, &size), ==, RDS_ENC_SPS_FIXED);
      rds_test_h265_sps (fixed, size, &s);
      g_assert_cmpuint (s.width, ==, 720);
      g_assert_cmpuint (s.height, ==, 480);
      t_in = rds_test_h265_sps_tail (nal->data, nal->len);
      t_out = rds_test_h265_sps_tail (fixed, size);
      g_assert_cmpstr (t_in, ==, tail->str);
      g_assert_cmpstr (t_out, ==, tail->str);
      /* no 0x000000..0x000002 left unescaped */
      for (gsize i = 2; i + 2 < size; i++)
        g_assert_false (fixed[i] == 0 && fixed[i + 1] == 0 && fixed[i + 2] <= 2);
      epb_in = rds_test_nal_epb_count (nal->data, nal->len);
      epb_out = rds_test_nal_epb_count (fixed, size);
      seen_more |= epb_out > epb_in;
      seen_fewer |= epb_out < epb_in;
      g_free (t_in);
      g_free (t_out);
      g_free (fixed);
      g_byte_array_unref (nal);
      g_string_free (tail, TRUE);
    }
  }
  g_assert_true (seen_more);
  g_assert_true (seen_fewer);
}

static void
test_sps_fix_emulation_captured (void)
{
  /* the captured SPSs' VUI has zero runs: after the rewrite their emulation prevention bytes move */
  guint8 *fixed = NULL;
  gsize size = 0;
  GByteArray *rb_in, *rb_out;

  g_assert_cmpint (rds_enc_h265_fix_sps (sps_720x480, sizeof (sps_720x480), 720, 480, &fixed, &size), ==,
      RDS_ENC_SPS_FIXED);
  g_assert_cmpuint (rds_test_nal_epb_count (sps_720x480, sizeof (sps_720x480)), ==, 5);
  rb_in = rds_test_nal_rbsp (sps_720x480, sizeof (sps_720x480));
  rb_out = rds_test_nal_rbsp (fixed, size);
  /* the window (flag, ue 0, ue 24, ue 0, ue 0: 1 + 1 + 9 + 1 + 1 bits) replaces a 0 flag: 12 more bits */
  g_assert_cmpuint (rb_out->len, >=, rb_in->len);
  g_assert_cmpuint (rb_out->len, <=, rb_in->len + 2);
  g_byte_array_unref (rb_in);
  g_byte_array_unref (rb_out);
  g_free (fixed);
}

/* An access unit VPS, SPS, PPS, slice with 4-byte start codes. */
static GByteArray *
make_au (const guint8 * sps, gsize sps_size)
{
  static const guint8 pps[] = { 0x44, 0x01, 0xc1, 0x73, 0xd1, 0x89 };
  const Part au[] = { P (4, h265_vps, FALSE), {4, sps, sps_size, FALSE}, P (4, pps, FALSE),
    P (3, h265_slice, FALSE) };

  return join (au, G_N_ELEMENTS (au), TRUE);
}

static void
test_sps_fix_buffer (void)
{
  GByteArray *au = make_au (sps_1366x768, sizeof (sps_1366x768)), *want;
  GstBuffer *b = make_buffer (au->data, au->len), *out;
  guint8 *fixed = NULL;
  gsize fixed_size = 0;
  GArray *nals;

  out = rds_enc_h265_fix_sps_buffer (b, 1366, 768);
  g_assert_true (out != b);
  assert_like_input (out);
  g_assert_cmpint (rds_enc_h265_fix_sps (sps_1366x768, sizeof (sps_1366x768), 1366, 768, &fixed, &fixed_size), ==,
      RDS_ENC_SPS_FIXED);
  want = make_au (fixed, fixed_size);
  assert_buffer_bytes (out, want->data, want->len);
  {
    GstMapInfo map;
    RdsTestSps s;

    g_assert_true (gst_buffer_map (out, &map, GST_MAP_READ));
    nals = rds_test_nal_split (map.data, map.size, TRUE);
    g_assert_cmpuint (nals->len, ==, 4);
    g_assert_cmpuint (g_array_index (nals, RdsTestNal, 1).type, ==, 33);
    rds_test_h265_sps (g_array_index (nals, RdsTestNal, 1).data, g_array_index (nals, RdsTestNal, 1).size, &s);
    g_assert_cmpuint (s.width, ==, 1366);
    g_assert_cmpuint (s.height, ==, 768);
    g_array_unref (nals);
    gst_buffer_unmap (out, &map);
  }
  gst_buffer_unref (out);

  /* fixed again: nothing to do, same buffer */
  b = (gst_buffer_unref (b), make_buffer (want->data, want->len));
  out = rds_enc_h265_fix_sps_buffer (b, 1366, 768);
  g_assert_true (out == b);
  gst_buffer_unref (out);
  gst_buffer_unref (b);
  g_byte_array_unref (want);
  g_byte_array_unref (au);
  g_free (fixed);

  /* a right SPS, no SPS, an unparseable SPS: same buffer */
  au = make_au (sps_1280x720, sizeof (sps_1280x720));
  b = make_buffer (au->data, au->len);
  out = rds_enc_h265_fix_sps_buffer (b, 1280, 720);
  g_assert_true (out == b);
  gst_buffer_unref (out);
  out = rds_enc_h265_fix_sps_buffer (b, 4000, 720);
  g_assert_true (out == b);
  gst_buffer_unref (out);
  gst_buffer_unref (b);
  g_byte_array_unref (au);
  {
    const Part slice[] = { P (4, h265_slice, FALSE) };

    au = join (slice, 1, TRUE);
    b = make_buffer (au->data, au->len);
    out = rds_enc_h265_fix_sps_buffer (b, 720, 480);
    g_assert_true (out == b);
    gst_buffer_unref (out);
    gst_buffer_unref (b);
    g_byte_array_unref (au);
  }

  /* two SPSs in one buffer: both fixed */
  {
    const Part two[] = { {4, sps_720x480, sizeof (sps_720x480), FALSE}, P (3, h265_slice, FALSE),
      {4, sps_720x480, sizeof (sps_720x480), FALSE} };
    GstMapInfo map;

    au = join (two, G_N_ELEMENTS (two), TRUE);
    b = make_buffer (au->data, au->len);
    out = rds_enc_h265_fix_sps_buffer (b, 720, 480);
    g_assert_true (gst_buffer_map (out, &map, GST_MAP_READ));
    nals = rds_test_nal_split (map.data, map.size, TRUE);
    g_assert_cmpuint (nals->len, ==, 3);
    for (guint i = 0; i < 3; i += 2) {
      RdsTestSps s;

      rds_test_h265_sps (g_array_index (nals, RdsTestNal, i).data, g_array_index (nals, RdsTestNal, i).size, &s);
      g_assert_cmpuint (s.width, ==, 720);
    }
    g_assert_cmpmem (g_array_index (nals, RdsTestNal, 1).data, g_array_index (nals, RdsTestNal, 1).size,
        h265_slice, sizeof (h265_slice));
    g_array_unref (nals);
    gst_buffer_unmap (out, &map);
    gst_buffer_unref (out);
    gst_buffer_unref (b);
    g_byte_array_unref (au);
  }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Levels */

static void
test_level_map (void)
{
  static const gchar *h264[] = { "1", "1b", "1.1", "1.2", "1.3", "2", "2.1", "2.2", "3", "3.1", "3.2", "4", "4.1",
    "4.2", "5", "5.1", "5.2", "6", "6.1", "6.2" };
  static const gint h264_idc[] = { 10, 9, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62 };
  static const gchar *h265[] = { "1", "2", "2.1", "3", "3.1", "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2" };
  static const gint h265_idc[] = { 30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186 };
  static const gchar *unknown[] = { "", "0", "1.0", "1.4", "2.3", "3.3", "7", "6.3", "31", "abc", "3.1 ", " 3.1",
    "3,1", "1B" };
  gint cmp = 99;

  for (guint i = 0; i < G_N_ELEMENTS (h264); i++) {
    g_assert_cmpint (rds_enc_level_to_idc (RDS_ENC_CODEC_H264, h264[i]), ==, h264_idc[i]);
    g_assert_cmpstr (rds_enc_level_from_idc (RDS_ENC_CODEC_H264, (guint) h264_idc[i]), ==, h264[i]);
    /* in increasing order: "1b" sits between "1" and "1.1" */
    if (i > 0) {
      g_assert_true (rds_enc_level_compare (RDS_ENC_CODEC_H264, h264[i], h264[i - 1], &cmp));
      g_assert_cmpint (cmp, >, 0);
      g_assert_true (rds_enc_level_compare (RDS_ENC_CODEC_H264, h264[i - 1], h264[i], &cmp));
      g_assert_cmpint (cmp, <, 0);
    }
    g_assert_true (rds_enc_level_compare (RDS_ENC_CODEC_H264, h264[i], h264[i], &cmp));
    g_assert_cmpint (cmp, ==, 0);
  }
  for (guint i = 0; i < G_N_ELEMENTS (h265); i++) {
    g_assert_cmpint (rds_enc_level_to_idc (RDS_ENC_CODEC_H265, h265[i]), ==, h265_idc[i]);
    g_assert_cmpstr (rds_enc_level_from_idc (RDS_ENC_CODEC_H265, (guint) h265_idc[i]), ==, h265[i]);
    if (i > 0) {
      g_assert_true (rds_enc_level_compare (RDS_ENC_CODEC_H265, h265[i], h265[i - 1], &cmp));
      g_assert_cmpint (cmp, >, 0);
    }
  }
  g_assert_true (rds_enc_level_compare (RDS_ENC_CODEC_H264, "4", "3.1", &cmp));
  g_assert_cmpint (cmp, >, 0);
  g_assert_true (rds_enc_level_compare (RDS_ENC_CODEC_H265, "2", "3.1", &cmp));
  g_assert_cmpint (cmp, <, 0);

  /* unknown strings and idcs, and the levels of one codec only */
  for (guint i = 0; i < G_N_ELEMENTS (unknown); i++) {
    g_assert_cmpint (rds_enc_level_to_idc (RDS_ENC_CODEC_H264, unknown[i]), ==, -1);
    g_assert_cmpint (rds_enc_level_to_idc (RDS_ENC_CODEC_H265, unknown[i]), ==, -1);
  }
  g_assert_cmpint (rds_enc_level_to_idc (RDS_ENC_CODEC_H264, NULL), ==, -1);
  g_assert_cmpint (rds_enc_level_to_idc (RDS_ENC_CODEC_H265, "1b"), ==, -1);
  g_assert_cmpint (rds_enc_level_to_idc (RDS_ENC_CODEC_H265, "1.1"), ==, -1);
  g_assert_cmpint (rds_enc_level_to_idc (RDS_ENC_CODEC_H265, "3.2"), ==, -1);
  g_assert_null (rds_enc_level_from_idc (RDS_ENC_CODEC_H264, 0));
  g_assert_null (rds_enc_level_from_idc (RDS_ENC_CODEC_H264, 14));
  g_assert_null (rds_enc_level_from_idc (RDS_ENC_CODEC_H264, 93));
  g_assert_null (rds_enc_level_from_idc (RDS_ENC_CODEC_H265, 31));
  g_assert_null (rds_enc_level_from_idc (RDS_ENC_CODEC_H265, 96));
  cmp = 99;
  g_assert_false (rds_enc_level_compare (RDS_ENC_CODEC_H264, "3.3", "3.1", &cmp));
  g_assert_false (rds_enc_level_compare (RDS_ENC_CODEC_H264, "3.1", NULL, &cmp));
  g_assert_false (rds_enc_level_compare (RDS_ENC_CODEC_H265, "1b", "1", &cmp));
  g_assert_cmpint (cmp, ==, 99);

  /* "1b" in an H.264 SPS, both ways: level_idc 11 + constraint_set3_flag (0x10) in Baseline, Main and Extended,
   * level_idc 9 in the High profiles; 11 with the flag in a High profile is 1.1 (the flag means something else) */
  g_assert_cmpstr (rds_enc_h264_level_from_sps (66, 0xd0, 11), ==, "1b");
  g_assert_cmpstr (rds_enc_h264_level_from_sps (77, 0x10, 11), ==, "1b");
  g_assert_cmpstr (rds_enc_h264_level_from_sps (88, 0x10, 11), ==, "1b");
  g_assert_cmpstr (rds_enc_h264_level_from_sps (66, 0xc0, 11), ==, "1.1");
  g_assert_cmpstr (rds_enc_h264_level_from_sps (100, 0x00, 9), ==, "1b");
  g_assert_cmpstr (rds_enc_h264_level_from_sps (100, 0x10, 11), ==, "1.1");
  g_assert_cmpstr (rds_enc_h264_level_from_sps (66, 0xd0, 31), ==, "3.1");
  g_assert_cmpstr (rds_enc_h264_level_from_sps (100, 0x00, 51), ==, "5.1");
  g_assert_null (rds_enc_h264_level_from_sps (100, 0x00, 33));
}

/* Rewrites @nal's level to @level and checks: the walker reads @idc, the NAL unit has the same size and the same
 * emulation prevention, and every byte but the level one is the same (also in the RBSP); *@at is the NAL unit
 * byte that changed (G_MAXSIZE: none) and *@rbsp_at the RBSP one. With @constraint_flags >= 0 (H.264) the walker
 * reads them too; the constraint flags byte may then differ as well. */
static void
check_level_set (const guint8 * nal, gsize size, RdsEncCodec codec, const gchar * level, guint idc,
    gint constraint_flags, gsize * at, gsize * rbsp_at)
{
  gboolean vps = codec == RDS_ENC_CODEC_H265 && ((nal[0] >> 1) & 0x3f) == 32;
  guint8 *out = NULL;
  gsize out_size = 0, diff_nal = G_MAXSIZE, diff_rbsp = G_MAXSIZE;
  GByteArray *rb_in, *rb_out;
  RdsTestSps s;

  g_assert_cmpint (rds_enc_set_level (nal, size, codec, level, &out, &out_size), ==, RDS_ENC_SPS_FIXED);
  g_assert_cmpuint (out_size, ==, size);
  g_assert_cmpuint (rds_test_nal_epb_count (out, out_size), ==, rds_test_nal_epb_count (nal, size));
  if (codec == RDS_ENC_CODEC_H264) {
    rds_test_h264_sps (out, out_size, &s);
    g_assert_cmpuint (s.level_idc, ==, idc);
    if (constraint_flags >= 0)
      g_assert_cmphex (s.constraint_flags, ==, (guint) constraint_flags);
    g_assert_cmpstr (rds_enc_h264_level_from_sps (s.profile_idc, s.constraint_flags, s.level_idc), ==, level);
  } else if (vps) {
    g_assert_cmpuint (rds_test_h265_vps_level (out, out_size), ==, idc);
  } else {
    RdsTestSps before;
    gchar *t_in = rds_test_h265_sps_tail (nal, size), *t_out = rds_test_h265_sps_tail (out, out_size);

    rds_test_h265_sps (nal, size, &before);
    rds_test_h265_sps (out, out_size, &s);
    g_assert_cmpuint (s.level_idc, ==, idc);
    g_assert_cmpuint (s.width, ==, before.width);
    g_assert_cmpuint (s.height, ==, before.height);
    g_assert_cmpstr (t_out, ==, t_in);
    g_free (t_in);
    g_free (t_out);
  }
  /* one byte differs (two for an H.264 "1b" change in Baseline: the constraint flags too) */
  for (gsize i = 0; i < size; i++) {
    if (out[i] == nal[i])
      continue;
    if (codec == RDS_ENC_CODEC_H264 && i == 2)
      continue;                 /* the constraint flags */
    g_assert_cmpuint (diff_nal, ==, G_MAXSIZE);
    diff_nal = i;
  }
  rb_in = rds_test_nal_rbsp (nal, size);
  rb_out = rds_test_nal_rbsp (out, out_size);
  g_assert_cmpuint (rb_out->len, ==, rb_in->len);
  for (guint i = 0; i < rb_in->len; i++) {
    if (rb_out->data[i] == rb_in->data[i] || (codec == RDS_ENC_CODEC_H264 && i == 2))
      continue;
    g_assert_cmpuint (diff_rbsp, ==, G_MAXSIZE);
    diff_rbsp = i;
  }
  g_byte_array_unref (rb_in);
  g_byte_array_unref (rb_out);
  *at = diff_nal;
  *rbsp_at = diff_rbsp;
  g_free (out);
}

static void
test_level_h264 (void)
{
  guint8 *out = (guint8 *) "untouched";
  gsize out_size = 99, at, rbsp_at;
  guint8 one_b[sizeof (h264_sps_cb_720p)];

  /* High: level_idc is the RBSP's (and the NAL unit's) byte 3, after the header, profile_idc and the flags */
  check_level_set (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, "4.1", 41, 0x00, &at,
      &rbsp_at);
  g_assert_cmpuint (at, ==, 3);
  g_assert_cmpuint (rbsp_at, ==, 3);
  check_level_set (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, "5.1", 51, 0x00, &at,
      &rbsp_at);
  check_level_set (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, "6.2", 62, 0x00, &at,
      &rbsp_at);
  /* "1b" in High: level_idc 9, the flags stay */
  check_level_set (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, "1b", 9, 0x00, &at,
      &rbsp_at);
  /* Constrained Baseline: 4.1 keeps set0 and set1 */
  check_level_set (h264_sps_cb_720p, sizeof (h264_sps_cb_720p), RDS_ENC_CODEC_H264, "4.1", 41, 0xc0, &at, &rbsp_at);
  g_assert_cmpuint (at, ==, 3);
  /* "1b" in Baseline: level_idc 11 and constraint_set3_flag; from "1b" to "1.1" or "3.1" the flag goes again */
  check_level_set (h264_sps_cb_720p, sizeof (h264_sps_cb_720p), RDS_ENC_CODEC_H264, "1b", 11, 0xd0, &at, &rbsp_at);
  memcpy (one_b, h264_sps_cb_720p, sizeof (one_b));
  one_b[2] = 0xd0;
  one_b[3] = 11;
  check_level_set (one_b, sizeof (one_b), RDS_ENC_CODEC_H264, "1.1", 11, 0xc0, &at, &rbsp_at);
  g_assert_cmpuint (at, ==, G_MAXSIZE);   /* only the flags changed */
  check_level_set (one_b, sizeof (one_b), RDS_ENC_CODEC_H264, "3.1", 31, 0xc0, &at, &rbsp_at);
  g_assert_cmpuint (at, ==, 3);

  /* already that level: unchanged, outputs untouched */
  g_assert_cmpint (rds_enc_set_level (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, "3.1",
          &out, &out_size), ==, RDS_ENC_SPS_UNCHANGED);
  g_assert_cmpint (rds_enc_set_level (one_b, sizeof (one_b), RDS_ENC_CODEC_H264, "1b", &out, &out_size), ==,
      RDS_ENC_SPS_UNCHANGED);
  /* not an SPS, cut short, an unknown level, the forbidden bit */
  g_assert_cmpint (rds_enc_set_level (h264_pps, sizeof (h264_pps), RDS_ENC_CODEC_H264, "4.1", &out, &out_size), ==,
      RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_set_level (h264_sps_high_720p, 3, RDS_ENC_CODEC_H264, "4.1", &out, &out_size), ==,
      RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_set_level (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, "4.4",
          &out, &out_size), ==, RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_set_level (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, NULL,
          &out, &out_size), ==, RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_set_level (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H264, "4.1", &out,
          &out_size), ==, RDS_ENC_SPS_INVALID);
  memcpy (one_b, h264_sps_cb_720p, sizeof (one_b));
  one_b[0] |= 0x80;
  g_assert_cmpint (rds_enc_set_level (one_b, sizeof (one_b), RDS_ENC_CODEC_H264, "4.1", &out, &out_size), ==,
      RDS_ENC_SPS_INVALID);
  g_assert_cmpstr ((const gchar *) out, ==, "untouched");
  g_assert_cmpuint (out_size, ==, 99);
}

static void
test_level_h265 (void)
{
  guint8 *out = (guint8 *) "untouched";
  gsize out_size = 99, at, rbsp_at;
  guint8 bad[sizeof (sps_1280x720)];

  /* general_level_idc is RBSP byte 2 + 1 + 11 of the SPS and 2 + 4 + 11 of the VPS (header included) */
  check_level_set (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H265, "4.1", 123, -1, &at, &rbsp_at);
  g_assert_cmpuint (rbsp_at, ==, 14);
  check_level_set (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H265, "5.1", 153, -1, &at, &rbsp_at);
  check_level_set (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H265, "6.2", 186, -1, &at, &rbsp_at);
  check_level_set (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H265, "1", 30, -1, &at, &rbsp_at);
  check_level_set (vps_1280x720, sizeof (vps_1280x720), RDS_ENC_CODEC_H265, "4.1", 123, -1, &at, &rbsp_at);
  g_assert_cmpuint (rbsp_at, ==, 17);
  check_level_set (vps_1280x720, sizeof (vps_1280x720), RDS_ENC_CODEC_H265, "5.1", 153, -1, &at, &rbsp_at);
  /* other sizes' SPSs too */
  check_level_set (sps_720x480, sizeof (sps_720x480), RDS_ENC_CODEC_H265, "5.1", 153, -1, &at, &rbsp_at);
  check_level_set (sps_1366x768, sizeof (sps_1366x768), RDS_ENC_CODEC_H265, "5.1", 153, -1, &at, &rbsp_at);

  g_assert_cmpint (rds_enc_set_level (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H265, "3.1", &out,
          &out_size), ==, RDS_ENC_SPS_UNCHANGED);
  g_assert_cmpint (rds_enc_set_level (vps_1280x720, sizeof (vps_1280x720), RDS_ENC_CODEC_H265, "3.1", &out,
          &out_size), ==, RDS_ENC_SPS_UNCHANGED);
  /* a PPS, cut short before the level, an H.264 level string, another layer */
  g_assert_cmpint (rds_enc_set_level (h265_slice, sizeof (h265_slice), RDS_ENC_CODEC_H265, "4.1", &out,
          &out_size), ==, RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_set_level (sps_1280x720, 17, RDS_ENC_CODEC_H265, "4.1", &out, &out_size), ==,
      RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_set_level (vps_1280x720, 20, RDS_ENC_CODEC_H265, "4.1", &out, &out_size), ==,
      RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_set_level (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H265, "1b", &out,
          &out_size), ==, RDS_ENC_SPS_INVALID);
  g_assert_cmpint (rds_enc_set_level (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H265, "4.1",
          &out, &out_size), ==, RDS_ENC_SPS_INVALID);
  memcpy (bad, sps_1280x720, sizeof (bad));
  bad[1] = (1 << 3) | 1;        /* nuh_layer_id 1 */
  g_assert_cmpint (rds_enc_set_level (bad, sizeof (bad), RDS_ENC_CODEC_H265, "4.1", &out, &out_size), ==,
      RDS_ENC_SPS_INVALID);
  g_assert_cmpstr ((const gchar *) out, ==, "untouched");
  g_assert_cmpuint (out_size, ==, 99);
}

/* VA's PTL has zero runs before general_level_idc (the reserved constraint bits), escaped by emulation prevention
 * bytes: the level byte is found on the RBSP, so its NAL unit offset is past them. A level byte (9..186) is never a
 * zero byte nor <= 3, so it can neither need nor end an escape: every level leaves the emulation prevention as it
 * was, and nothing unescaped. */
static void
test_level_emulation (void)
{
  static const gchar *h265[] = { "1", "2", "2.1", "3", "3.1", "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2" };
  static const gchar *h264[] = { "1", "1b", "1.1", "1.2", "1.3", "2", "2.1", "2.2", "3", "3.1", "3.2", "4", "4.1",
    "4.2", "5", "5.1", "5.2", "6", "6.1", "6.2" };
  gsize at, rbsp_at;

  g_assert_cmpuint (rds_test_nal_epb_count (sps_1280x720, 18), ==, 3);
  g_assert_cmpuint (rds_test_nal_epb_count (vps_1280x720, 21), ==, 3);
  check_level_set (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H265, "4.1", 123, -1, &at, &rbsp_at);
  g_assert_cmpuint (at, ==, 17);
  g_assert_cmpuint (rbsp_at, ==, 14);
  check_level_set (vps_1280x720, sizeof (vps_1280x720), RDS_ENC_CODEC_H265, "4.1", 123, -1, &at, &rbsp_at);
  g_assert_cmpuint (at, ==, 20);
  g_assert_cmpuint (rbsp_at, ==, 17);

  for (guint i = 0; i < G_N_ELEMENTS (h265); i++) {
    const guint8 *nals[] = { sps_1280x720, vps_1280x720, sps_720x480 };
    const gsize sizes[] = { sizeof (sps_1280x720), sizeof (vps_1280x720), sizeof (sps_720x480) };

    for (guint k = 0; k < G_N_ELEMENTS (nals); k++) {
      guint8 *out = NULL;
      gsize size = 0;
      RdsEncSpsResult res = rds_enc_set_level (nals[k], sizes[k], RDS_ENC_CODEC_H265, h265[i], &out, &size);

      if (res == RDS_ENC_SPS_UNCHANGED)
        continue;
      g_assert_cmpint (res, ==, RDS_ENC_SPS_FIXED);
      g_assert_cmpuint (size, ==, sizes[k]);
      g_assert_cmpuint (rds_test_nal_epb_count (out, size), ==, rds_test_nal_epb_count (nals[k], sizes[k]));
      for (gsize j = 0; j + 2 < size; j++)
        g_assert_false (out[j] == 0 && out[j + 1] == 0 && out[j + 2] <= 2);
      g_free (out);
    }
  }
  for (guint i = 0; i < G_N_ELEMENTS (h264); i++) {
    if (g_str_equal (h264[i], "3.1"))
      continue;
    check_level_set (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, h264[i],
        (guint) rds_enc_level_to_idc (RDS_ENC_CODEC_H264, h264[i]), -1, &at, &rbsp_at);
    check_level_set (h264_sps_cb_720p, sizeof (h264_sps_cb_720p), RDS_ENC_CODEC_H264, h264[i],
        g_str_equal (h264[i], "1b") ? 11 : (guint) rds_enc_level_to_idc (RDS_ENC_CODEC_H264, h264[i]), -1, &at,
        &rbsp_at);
  }
}

/* An HEVC access unit: VPS, SPS, PPS, slice. */
static GByteArray *
make_h265_au (const guint8 * vps, gsize vps_size, const guint8 * sps, gsize sps_size)
{
  static const guint8 pps[] = { 0x44, 0x01, 0xc1, 0x73, 0xd1, 0x89 };
  const Part au[] = { {4, vps, vps_size, FALSE}, {4, sps, sps_size, FALSE}, P (4, pps, FALSE),
    P (3, h265_slice, FALSE) };

  return join (au, G_N_ELEMENTS (au), TRUE);
}

/* @nal with @level, which must change it. */
static guint8 *
with_level (const guint8 * nal, gsize size, RdsEncCodec codec, const gchar * level, gsize * out_size)
{
  guint8 *out = NULL;

  g_assert_cmpint (rds_enc_set_level (nal, size, codec, level, &out, out_size), ==, RDS_ENC_SPS_FIXED);
  return out;
}

static void
test_level_buffer (void)
{
  GByteArray *au, *want;
  GstBuffer *b, *out;
  guint8 *vps, *sps;
  gsize vps_size, sps_size;

  /* H.265: the VPS and the SPS */
  au = make_h265_au (vps_1280x720, sizeof (vps_1280x720), sps_1280x720, sizeof (sps_1280x720));
  b = make_buffer (au->data, au->len);
  out = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H265, "5.1");
  g_assert_true (out != b);
  assert_like_input (out);
  vps = with_level (vps_1280x720, sizeof (vps_1280x720), RDS_ENC_CODEC_H265, "5.1", &vps_size);
  sps = with_level (sps_1280x720, sizeof (sps_1280x720), RDS_ENC_CODEC_H265, "5.1", &sps_size);
  want = make_h265_au (vps, vps_size, sps, sps_size);
  assert_buffer_bytes (out, want->data, want->len);
  gst_buffer_unref (out);
  /* the same level: the same buffer */
  out = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H265, "3.1");
  g_assert_true (out == b);
  gst_buffer_unref (out);
  gst_buffer_unref (b);
  b = make_buffer (want->data, want->len);
  out = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H265, "5.1");
  g_assert_true (out == b);
  gst_buffer_unref (out);
  /* an unknown level: the same buffer */
  out = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H265, "9");
  g_assert_true (out == b);
  gst_buffer_unref (out);
  gst_buffer_unref (b);
  g_byte_array_unref (want);
  g_byte_array_unref (au);
  g_free (vps);
  g_free (sps);

  /* no parameter set: the same buffer */
  {
    const Part slice[] = { P (4, h265_slice, FALSE) };

    au = join (slice, 1, TRUE);
    b = make_buffer (au->data, au->len);
    out = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H265, "5.1");
    g_assert_true (out == b);
    gst_buffer_unref (out);
    gst_buffer_unref (b);
    g_byte_array_unref (au);
  }

  /* H.264: SPS, PPS, IDR; 3- and 4-byte start codes */
  {
    const Part parts[] = { {4, h264_sps_high_720p, sizeof (h264_sps_high_720p), FALSE}, P (3, h264_pps, FALSE),
      P (4, h264_idr, FALSE) };
    Part fixed[G_N_ELEMENTS (parts)];

    au = join (parts, G_N_ELEMENTS (parts), TRUE);
    b = make_buffer (au->data, au->len);
    out = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H264, "4.1");
    g_assert_true (out != b);
    assert_like_input (out);
    sps = with_level (h264_sps_high_720p, sizeof (h264_sps_high_720p), RDS_ENC_CODEC_H264, "4.1", &sps_size);
    memcpy (fixed, parts, sizeof (parts));
    fixed[0].nal = sps;
    fixed[0].size = sps_size;
    want = join (fixed, G_N_ELEMENTS (fixed), TRUE);
    assert_buffer_bytes (out, want->data, want->len);
    gst_buffer_unref (out);
    out = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H264, "3.1");
    g_assert_true (out == b);
    gst_buffer_unref (out);
    gst_buffer_unref (b);
    g_byte_array_unref (want);
    g_byte_array_unref (au);
    g_free (sps);
  }
}

/* The H.265 window fix and the level rewrite on one SPS, in either order and in the one-pass helper: the same bytes,
 * the exact size and the new level; the one-pass helper with only one of them is that one alone. */
static void
test_level_with_window (void)
{
  GByteArray *au = make_h265_au (vps_1280x720, sizeof (vps_1280x720), sps_1366x768, sizeof (sps_1366x768));
  GstBuffer *b = make_buffer (au->data, au->len), *t, *a1, *a2;
  GstMapInfo m1, m2;
  GArray *nals;
  RdsTestSps s;

  t = rds_enc_h265_fix_sps_buffer (b, 1366, 768);
  a1 = rds_enc_set_level_buffer (t, RDS_ENC_CODEC_H265, "5.1");
  gst_buffer_unref (t);
  t = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H265, "5.1");
  a2 = rds_enc_h265_fix_sps_buffer (t, 1366, 768);
  gst_buffer_unref (t);
  assert_like_input (a1);
  g_assert_true (gst_buffer_map (a1, &m1, GST_MAP_READ));
  g_assert_true (gst_buffer_map (a2, &m2, GST_MAP_READ));
  g_assert_cmpmem (m1.data, m1.size, m2.data, m2.size);
  gst_buffer_unmap (a2, &m2);
  gst_buffer_unref (a2);
  a2 = rds_enc_h265_fix_parameter_sets (b, 1366, 768, "5.1");
  assert_like_input (a2);
  assert_buffer_bytes (a2, m1.data, m1.size);
  gst_buffer_unref (a2);
  /* one of the two only */
  t = rds_enc_h265_fix_sps_buffer (b, 1366, 768);
  a2 = rds_enc_h265_fix_parameter_sets (b, 1366, 768, NULL);
  g_assert_true (gst_buffer_map (t, &m2, GST_MAP_READ));
  assert_buffer_bytes (a2, m2.data, m2.size);
  gst_buffer_unmap (t, &m2);
  gst_buffer_unref (a2);
  gst_buffer_unref (t);
  t = rds_enc_set_level_buffer (b, RDS_ENC_CODEC_H265, "5.1");
  a2 = rds_enc_h265_fix_parameter_sets (b, 0, 0, "5.1");
  g_assert_true (gst_buffer_map (t, &m2, GST_MAP_READ));
  assert_buffer_bytes (a2, m2.data, m2.size);
  gst_buffer_unmap (t, &m2);
  gst_buffer_unref (a2);
  gst_buffer_unref (t);
  /* nothing to do: the same buffer */
  a2 = rds_enc_h265_fix_parameter_sets (b, 0, 0, NULL);
  g_assert_true (a2 == b);
  gst_buffer_unref (a2);
  {
    GByteArray *right = make_h265_au (vps_1280x720, sizeof (vps_1280x720), sps_1280x720, sizeof (sps_1280x720));
    GstBuffer *rb = make_buffer (right->data, right->len);

    a2 = rds_enc_h265_fix_parameter_sets (rb, 1280, 720, "3.1");
    g_assert_true (a2 == rb);
    gst_buffer_unref (a2);
    gst_buffer_unref (rb);
    g_byte_array_unref (right);
  }
  a2 = rds_enc_h265_fix_parameter_sets (b, 1366, 768, "5.1");
  g_assert_true (gst_buffer_map (a2, &m2, GST_MAP_READ));
  nals = rds_test_nal_split (m1.data, m1.size, TRUE);
  g_assert_cmpuint (nals->len, ==, 4);
  g_assert_cmpuint (rds_test_h265_vps_level (g_array_index (nals, RdsTestNal, 0).data,
          g_array_index (nals, RdsTestNal, 0).size), ==, 153);
  rds_test_h265_sps (g_array_index (nals, RdsTestNal, 1).data, g_array_index (nals, RdsTestNal, 1).size, &s);
  g_assert_cmpuint (s.width, ==, 1366);
  g_assert_cmpuint (s.height, ==, 768);
  g_assert_cmpuint (s.level_idc, ==, 153);
  g_array_unref (nals);
  gst_buffer_unmap (a1, &m1);
  gst_buffer_unmap (a2, &m2);
  gst_buffer_unref (a1);
  gst_buffer_unref (a2);
  gst_buffer_unref (b);
  g_byte_array_unref (au);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* GPU: real streams */

#define REQUIRE_ELEMENT(name) G_STMT_START {                     \
    if (!rds_test_have_element (name)) {                          \
      g_test_skip (name " not available (no VA plugin or encode)"); \
      return;                                                     \
    }                                                             \
  } G_STMT_END

static void
test_gpu_walker_h264 (void)
{
  const gchar *caps = "video/x-raw,format=NV12,width=640,height=360,framerate=30/1";
  GPtrArray *aus;
  guint b_slices = 0;

  REQUIRE_ELEMENT ("vah264enc");
  REQUIRE_ELEMENT ("h264parse");
  for (guint bf = 0; bf <= 2; bf += 2) {
    aus = rds_test_encode_aus ("h264", caps, 12, bf, "ball", NULL);
    g_assert_cmpuint (aus->len, ==, 12);
    for (guint i = 0; i < aus->len; i++) {
      gsize size;
      const guint8 *d = g_bytes_get_data (aus->pdata[i], &size);
      GArray *w = rds_test_nal_aus (d, size, FALSE);
      const RdsTestAu *au;

      g_assert_cmpuint (w->len, ==, 1);       /* alignment=au: one access unit per buffer */
      au = &g_array_index (w, RdsTestAu, 0);
      if (i == 0) {
        GArray *nals = rds_test_nal_split (d, size, FALSE);
        RdsTestSps sps;

        g_assert_true (rds_test_au_has (au, 7));
        g_assert_true (rds_test_au_has (au, 8));
        g_assert_true (rds_test_au_has (au, 5));
        g_assert_cmpuint (au->first_slice_type, ==, RDS_TEST_H264_SLICE_I);
        for (guint k = 0; k < nals->len; k++) {
          const RdsTestNal *n = &g_array_index (nals, RdsTestNal, k);

          if (n->type != 7)
            continue;
          rds_test_h264_sps (n->data, n->size, &sps);
          g_assert_true (sps.profile_idc == 66 || sps.profile_idc == 77 || sps.profile_idc == 100);
          g_assert_cmpuint (sps.level_idc, >=, 10);
          g_assert_cmpuint (sps.level_idc, <=, 62);
          g_assert_cmpuint (sps.width, ==, 640);
          g_assert_cmpuint (sps.height, ==, 360);
        }
        g_array_unref (nals);
      } else {
        g_assert_false (rds_test_au_has (au, 5));
        g_assert_true (rds_test_au_has (au, 1));
        g_assert_cmpuint (au->first_slice_type, !=, RDS_TEST_H264_SLICE_I);
        if (bf == 0)
          g_assert_cmpuint (au->first_slice_type, ==, RDS_TEST_H264_SLICE_P);
        b_slices += au->first_slice_type == RDS_TEST_H264_SLICE_B;
      }
      g_array_unref (w);
    }
    {
      GBytes *all = rds_test_concat ((GBytes * const *) aus->pdata, aus->len);
      gsize size;
      const guint8 *d = g_bytes_get_data (all, &size);
      GArray *w = rds_test_nal_aus (d, size, FALSE);

      g_assert_cmpuint (w->len, ==, 12);      /* the walker finds the AU boundaries in the whole stream */
      g_array_unref (w);
      g_bytes_unref (all);
    }
    g_ptr_array_unref (aus);
  }
  g_assert_cmpuint (b_slices, >, 0);
}

static void
test_gpu_walker_h265 (void)
{
  GBytes *stream;
  gsize size;
  const guint8 *d;
  GArray *w, *nals;

  REQUIRE_ELEMENT ("vah265enc");
  REQUIRE_ELEMENT ("h265parse");
  stream = rds_test_encode ("h265", 640, 360, 10, 0, "ball");
  d = g_bytes_get_data (stream, &size);
  w = rds_test_nal_aus (d, size, TRUE);
  g_assert_cmpuint (w->len, ==, 10);
  for (guint i = 0; i < w->len; i++) {
    const RdsTestAu *au = &g_array_index (w, RdsTestAu, i);

    if (i == 0) {
      g_assert_true (rds_test_au_has (au, 32));
      g_assert_true (rds_test_au_has (au, 33));
      g_assert_true (rds_test_au_has (au, 34));
      g_assert_true (rds_test_au_has (au, 19) || rds_test_au_has (au, 20));     /* IDR_W_RADL / IDR_N_LP */
    } else {
      g_assert_false (rds_test_au_has (au, 33));
      g_assert_false (rds_test_au_has (au, 19) || rds_test_au_has (au, 20));
      g_assert_true (rds_test_au_has (au, 0) || rds_test_au_has (au, 1));       /* TRAIL_N / TRAIL_R */
    }
  }
  nals = rds_test_nal_split (d, size, TRUE);
  for (guint k = 0; k < nals->len; k++) {
    const RdsTestNal *n = &g_array_index (nals, RdsTestNal, k);
    RdsTestSps sps;

    if (n->type != 33)
      continue;
    rds_test_h265_sps (n->data, n->size, &sps);
    g_assert_cmpuint (sps.profile_idc, ==, 1);
    g_assert_cmpuint (sps.level_idc, >=, 30);
    g_assert_cmpuint (sps.level_idc, <=, 186);
    g_assert_cmpuint (sps.chroma_format_idc, ==, 1);
    g_assert_cmpuint (sps.width, ==, 640);    /* a multiple of 64: VA's window is right */
    g_assert_cmpuint (sps.height, ==, 360);
  }
  g_array_unref (nals);
  g_array_unref (w);
  g_bytes_unref (stream);
}

/* CBR on easy content pads with filler once the CPB has filled (a few seconds in): strip every AU and check that
 * only the filler went. */
static void
test_gpu_filler (gconstpointer data)
{
  RdsEncCodec codec = GPOINTER_TO_INT (data);
  gboolean hevc = codec == RDS_ENC_CODEC_H265;
  const gchar *name = hevc ? "h265" : "h264";
  guint filler_type = hevc ? RDS_ENC_H265_NAL_FILLER : RDS_ENC_H264_NAL_FILLER;
  guint fillers = 0;
  GPtrArray *aus;

  if (!rds_test_have_element (hevc ? "vah265enc" : "vah264enc")) {
    g_test_skip ("VA encoder not available");
    return;
  }
  aus = rds_test_encode_aus (name, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1", 240, 0, "ball",
      "rate-control=cbr bitrate=4000");
  for (guint i = 0; i < aus->len; i++) {
    gsize size;
    const guint8 *d = g_bytes_get_data (aus->pdata[i], &size);
    GstBuffer *b = gst_buffer_new_memdup (d, size), *out = rds_enc_strip_filler (b, codec);
    guint n = rds_test_nal_count (d, size, hevc, filler_type);
    GstMapInfo map;
    GArray *nin, *nout;
    guint k = 0;

    fillers += n;
    g_assert_true ((out == b) == (n == 0));
    g_assert_true (gst_buffer_map (out, &map, GST_MAP_READ));
    g_assert_cmpuint (rds_test_nal_count (map.data, map.size, hevc, filler_type), ==, 0);
    nin = rds_test_nal_split (d, size, hevc);
    nout = rds_test_nal_split (map.data, map.size, hevc);
    for (guint j = 0; j < nin->len; j++) {
      const RdsTestNal *a = &g_array_index (nin, RdsTestNal, j);

      if (a->type == filler_type)
        continue;
      g_assert_cmpuint (k, <, nout->len);
      g_assert_cmpmem (a->data, a->size, g_array_index (nout, RdsTestNal, k).data,
          g_array_index (nout, RdsTestNal, k).size);
      k++;
    }
    g_assert_cmpuint (k, ==, nout->len);
    g_array_unref (nin);
    g_array_unref (nout);
    gst_buffer_unmap (out, &map);
    gst_buffer_unref (out);
    gst_buffer_unref (b);
  }
  g_assert_cmpuint (fillers, >, 0);
  g_ptr_array_unref (aus);
}

/* The VA encoder's shift, measured from its input and output segments, removed from the first output PTS gives
 * the input's first PTS. */
static void
test_gpu_va_shift (void)
{
  const GstClockTime offsets[] = { 0, 10 * GST_SECOND };

  REQUIRE_ELEMENT ("vah264enc");
  for (guint i = 0; i < G_N_ELEMENTS (offsets); i++) {
    gchar *desc = g_strdup_printf ("videotestsrc num-buffers=3 timestamp-offset=%" G_GUINT64_FORMAT
        " ! video/x-raw,format=NV12,width=320,height=240,framerate=30/1 ! vah264enc ! appsink name=sink sync=false",
        offsets[i]);
    GError *err = NULL;
    GstElement *pipe = gst_parse_launch (desc, &err), *sink;
    GstSample *s;
    GstSegment in;
    GstClockTime shift;

    g_assert_no_error (err);
    sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
    g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    s = rds_test_pull (pipe, sink);
    g_assert_nonnull (s);
    gst_segment_init (&in, GST_FORMAT_TIME);    /* videotestsrc's segment */
    shift = rds_enc_segment_shift (&in, gst_sample_get_segment (s));
    g_assert_cmpuint (shift, ==, HOURS_1000 - offsets[i]);
    g_assert_cmpuint (rds_enc_ts_unshift (GST_BUFFER_PTS (gst_sample_get_buffer (s)), shift), ==, offsets[i]);
    g_assert_cmpuint (rds_enc_ts_unshift (GST_BUFFER_DTS (gst_sample_get_buffer (s)), shift), ==, offsets[i]);
    {
      GstSegment un = *gst_sample_get_segment (s);

      rds_enc_segment_unshift (&un, shift);
      g_assert_cmpuint (un.start, ==, in.start);
      g_assert_cmpuint (gst_segment_to_running_time (&un, GST_FORMAT_TIME, offsets[i]), ==,
          gst_segment_to_running_time (gst_sample_get_segment (s), GST_FORMAT_TIME,
              GST_BUFFER_PTS (gst_sample_get_buffer (s))));
    }
    gst_sample_unref (s);
    gst_element_set_state (pipe, GST_STATE_NULL);
    gst_object_unref (sink);
    gst_object_unref (pipe);
    g_free (desc);
  }
}

/* Decodes @stream with vah265dec and returns the output caps' size. */
static void
decode_size (GBytes * stream, gint * w, gint * h)
{
  GstElement *pipe, *src, *sink;
  GError *err = NULL;
  GstSample *s;
  GstStructure *st;

  pipe = gst_parse_launch ("appsrc name=src caps=video/x-h265,stream-format=byte-stream format=bytes ! h265parse "
      "! vah265dec ! video/x-raw ! appsink name=sink sync=false", &err);
  g_assert_no_error (err);
  src = gst_bin_get_by_name (GST_BIN (pipe), "src");
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src), gst_buffer_new_wrapped_bytes (stream)), ==,
      GST_FLOW_OK);
  g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src)), ==, GST_FLOW_OK);
  s = rds_test_pull (pipe, sink);
  g_assert_nonnull (s);
  st = gst_caps_get_structure (gst_sample_get_caps (s), 0);
  g_assert_true (gst_structure_get_int (st, "width", w));
  g_assert_true (gst_structure_get_int (st, "height", h));
  gst_sample_unref (s);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (src);
  gst_object_unref (pipe);
}

static void
test_gpu_sps_round_trip (void)
{
  GPtrArray *aus, *fixed;
  GBytes *stream;
  gint w = 0, h = 0;

  REQUIRE_ELEMENT ("vah265enc");
  REQUIRE_ELEMENT ("vah265dec");
  aus = rds_test_encode_aus ("h265", "video/x-raw,format=NV12,width=720,height=480,framerate=30/1", 5, 0, "smpte",
      NULL);
  /* unfixed, VA's window decodes 768 wide; if this ever gives 720, the driver has fixed the window itself and the
   * rewrite no longer does anything */
  stream = rds_test_concat ((GBytes * const *) aus->pdata, aus->len);
  decode_size (stream, &w, &h);
  g_assert_cmpint (w, ==, 768);
  g_assert_cmpint (h, ==, 480);
  g_bytes_unref (stream);
  fixed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  for (guint i = 0; i < aus->len; i++) {
    GstBuffer *b = gst_buffer_new_wrapped_bytes (aus->pdata[i]), *out = rds_enc_h265_fix_sps_buffer (b, 720, 480);
    GstMapInfo map;

    g_assert_true ((out != b) == (i == 0));    /* only the first AU has an SPS */
    g_assert_true (gst_buffer_map (out, &map, GST_MAP_READ));
    g_ptr_array_add (fixed, g_bytes_new (map.data, map.size));
    gst_buffer_unmap (out, &map);
    gst_buffer_unref (out);
    gst_buffer_unref (b);
  }
  stream = rds_test_concat ((GBytes * const *) fixed->pdata, fixed->len);
  decode_size (stream, &w, &h);
  g_assert_cmpint (w, ==, 720);
  g_assert_cmpint (h, ==, 480);
  g_bytes_unref (stream);
  g_ptr_array_unref (fixed);
  g_ptr_array_unref (aus);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Intra access units */

/* Hand-coded slice headers (bit strings in the comments): H.264 first_mb_in_slice ue(0) "1", then slice_type; HEVC
 * first_slice_segment_in_pic_flag "1", slice_pic_parameter_set_id ue(0) "1", num_extra_slice_header_bits reserved
 * bits, then slice_type. Each AU is an AUD-less run of NAL units with 4-byte start codes. */
static gboolean
intra (RdsEncCodec codec, guint * extra, const guint8 * au, gsize size)
{
  return rds_enc_au_is_intra (au, size, codec, extra);
}

static void
test_intra_h264 (void)
{
  /* SPS, PPS, IDR (type 5): I by the NAL type */
  static const guint8 idr[] = { 0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xce, 0, 0, 0, 1, 0x65, 0x88, 0x84 };
  /* non-IDR (type 1), slice_type 7 (I, all slices) "0001000": 1 0001000 */
  static const guint8 i7[] = { 0, 0, 0, 1, 0x41, 0x88, 0x84 };
  /* slice_type 2 (I) "011": 1 011 0000 */
  static const guint8 i2[] = { 0, 0, 1, 0x41, 0xb0, 0x80 };
  /* slice_type 4 (SI) "00101": 1 00101 00 */
  static const guint8 si[] = { 0, 0, 1, 0x41, 0x94, 0x80 };
  /* slice_type 5 (P) "00110": 1 00110 10 */
  static const guint8 p5[] = { 0, 0, 0, 1, 0x41, 0x9a, 0x80 };
  /* slice_type 0 (P) "1": 1 1 */
  static const guint8 p0[] = { 0, 0, 0, 1, 0x41, 0xc0 };
  /* slice_type 1 (B) "010", non-reference: 1 010 */
  static const guint8 b1[] = { 0, 0, 0, 1, 0x01, 0xa0 };
  /* SEI (type 6) before an I slice */
  static const guint8 sei_i[] = { 0, 0, 0, 1, 0x06, 0x05, 0x80, 0, 0, 0, 1, 0x41, 0x88, 0x84 };
  /* only parameter sets, and a slice header cut short */
  static const guint8 none[] = { 0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xce };
  static const guint8 cut[] = { 0, 0, 0, 1, 0x41, 0x00 };
  /* a data partition A (type 2) whose header would read as I: not a slice here */
  static const guint8 part[] = { 0, 0, 0, 1, 0x42, 0x88, 0x84 };
  /* an I slice header followed by a long picture: only the header is read */
  guint8 *big = g_malloc0 (1 << 20);
  guint extra = 0;

  g_assert_true (intra (RDS_ENC_CODEC_H264, &extra, idr, sizeof (idr)));
  g_assert_true (intra (RDS_ENC_CODEC_H264, &extra, i7, sizeof (i7)));
  g_assert_true (intra (RDS_ENC_CODEC_H264, &extra, i2, sizeof (i2)));
  g_assert_true (intra (RDS_ENC_CODEC_H264, &extra, si, sizeof (si)));
  g_assert_true (intra (RDS_ENC_CODEC_H264, &extra, sei_i, sizeof (sei_i)));
  g_assert_false (intra (RDS_ENC_CODEC_H264, &extra, p5, sizeof (p5)));
  g_assert_false (intra (RDS_ENC_CODEC_H264, &extra, p0, sizeof (p0)));
  g_assert_false (intra (RDS_ENC_CODEC_H264, &extra, b1, sizeof (b1)));
  g_assert_false (intra (RDS_ENC_CODEC_H264, &extra, none, sizeof (none)));
  g_assert_false (intra (RDS_ENC_CODEC_H264, &extra, cut, sizeof (cut)));
  g_assert_false (intra (RDS_ENC_CODEC_H264, &extra, NULL, 0));
  g_assert_false (intra (RDS_ENC_CODEC_H264, &extra, part, sizeof (part)));
  memcpy (big, i7, sizeof (i7));
  memset (big + sizeof (i7), 0x55, (1 << 20) - sizeof (i7));
  g_assert_true (intra (RDS_ENC_CODEC_H264, &extra, big, 1 << 20));
  g_free (big);
  g_assert_cmpuint (extra, ==, 0);
}

static void
test_intra_h265 (void)
{
  /* IDR_W_RADL (19) and CRA (21): intra by the NAL type */
  static const guint8 idr[] = { 0, 0, 0, 1, 0x26, 0x01, 0xa0 };
  static const guint8 cra[] = { 0, 0, 0, 1, 0x2a, 0x01, 0xa0 };
  /* TRAIL_R (1), no extra bits: slice_type 2 (I) "011": 1 1 011 000; 1 (P) "010": 1 1 010 000 */
  static const guint8 i0[] = { 0, 0, 0, 1, 0x02, 0x01, 0xd8 };
  static const guint8 p0[] = { 0, 0, 0, 1, 0x02, 0x01, 0xd0 };
  /* with two extra bits "00": I 1 1 00 011 0 and P 1 1 00 010 0 */
  static const guint8 i2[] = { 0, 0, 0, 1, 0x02, 0x01, 0xc6 };
  static const guint8 p2[] = { 0, 0, 0, 1, 0x02, 0x01, 0xc4 };
  /* PPS (34): pps id ue(0) "1", sps id ue(0) "1", dependent_slice_segments_enabled 0, output_flag_present 0,
   * num_extra_slice_header_bits "010": 1 1 0 0 010 1; then the TRAIL_R I slice with two extra bits */
  static const guint8 pps_i2[] = { 0, 0, 0, 1, 0x44, 0x01, 0xc5, 0x80, 0, 0, 0, 1, 0x02, 0x01, 0xc6 };
  /* not the first slice segment of the picture: can't be read without the PPS's other fields */
  static const guint8 later[] = { 0, 0, 0, 1, 0x02, 0x01, 0x58 };
  guint extra = 0;

  g_assert_true (intra (RDS_ENC_CODEC_H265, &extra, idr, sizeof (idr)));
  g_assert_true (intra (RDS_ENC_CODEC_H265, &extra, cra, sizeof (cra)));
  g_assert_true (intra (RDS_ENC_CODEC_H265, &extra, i0, sizeof (i0)));
  g_assert_false (intra (RDS_ENC_CODEC_H265, &extra, p0, sizeof (p0)));
  g_assert_false (intra (RDS_ENC_CODEC_H265, &extra, later, sizeof (later)));
  g_assert_cmpuint (extra, ==, 0);
  /* the PPS updates the extra bits before its slice is read, and they stay for the next AUs */
  g_assert_true (intra (RDS_ENC_CODEC_H265, &extra, pps_i2, sizeof (pps_i2)));
  g_assert_cmpuint (extra, ==, 2);
  g_assert_true (intra (RDS_ENC_CODEC_H265, &extra, i2, sizeof (i2)));
  g_assert_false (intra (RDS_ENC_CODEC_H265, &extra, p2, sizeof (p2)));
  /* read with 0 extra bits, the same bytes give other slice types */
  extra = 0;
  g_assert_false (intra (RDS_ENC_CODEC_H265, &extra, i2, sizeof (i2)));
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Truncated and random input */

/* Every helper that reads an Annex-B buffer or a NAL unit, on @size bytes copied into an allocation of exactly that
 * size (so a read past the end is a heap overflow under ASan). Results aren't checked, only that nothing crashes or
 * reads out of bounds; a FIXED result's output goes through the same helper again. */
static void
feed_annex_b (const guint8 * data, gsize size)
{
  static const RdsEncCodec codecs[] = { RDS_ENC_CODEC_H264, RDS_ENC_CODEC_H265 };
  guint8 *copy = g_memdup2 (data, size);

  for (guint c = 0; c < G_N_ELEMENTS (codecs); c++) {
    /* an empty buffer has no memory at all */
    GstBuffer *in = size > 0 ? gst_buffer_new_memdup (data, size) : gst_buffer_new (), *out;
    guint extra = 0;

    out = rds_enc_strip_filler (in, codecs[c]);
    gst_buffer_unref (out);
    out = rds_enc_set_level_buffer (in, codecs[c], "5.1");
    gst_buffer_unref (out);
    out = rds_enc_set_level_buffer (in, codecs[c], "1b");
    gst_buffer_unref (out);
    if (codecs[c] == RDS_ENC_CODEC_H265) {
      out = rds_enc_h265_fix_parameter_sets (in, 720, 480, "5.1");
      gst_buffer_unref (out);
      out = rds_enc_h265_fix_parameter_sets (in, 1, 1, NULL);
      gst_buffer_unref (out);
      out = rds_enc_h265_fix_sps_buffer (in, 8192, 4352);
      gst_buffer_unref (out);
    }
    (void) rds_enc_au_is_intra (copy, size, codecs[c], &extra);
    extra = 7;
    (void) rds_enc_au_is_intra (copy, size, codecs[c], &extra);
    gst_buffer_unref (in);
  }
  g_free (copy);
}

static guint fuzz_fixed;         /* FIXED results of feed_nal, to show that the input reached the rewrites */

static void
feed_nal (const guint8 * data, gsize size)
{
  static const RdsEncCodec codecs[] = { RDS_ENC_CODEC_H264, RDS_ENC_CODEC_H265 };
  guint8 *copy = g_memdup2 (data, size), *out = NULL, *again = NULL;
  gsize out_size = 0, again_size = 0;

  if (rds_enc_h265_fix_sps (copy, size, 720, 480, &out, &out_size) == RDS_ENC_SPS_FIXED) {
    fuzz_fixed++;
    (void) rds_enc_h265_fix_sps (out, out_size, 720, 480, &again, &again_size);
    g_clear_pointer (&again, g_free);
    g_clear_pointer (&out, g_free);
  }
  if (rds_enc_h265_fix_sps (copy, size, 2, 2, &out, &out_size) == RDS_ENC_SPS_FIXED)
    g_clear_pointer (&out, g_free);
  for (guint c = 0; c < G_N_ELEMENTS (codecs); c++) {
    if (rds_enc_set_level (copy, size, codecs[c], "6.2", &out, &out_size) == RDS_ENC_SPS_FIXED) {
      fuzz_fixed++;
      if (rds_enc_set_level (out, out_size, codecs[c], "1b", &again, &again_size) == RDS_ENC_SPS_FIXED)
        g_clear_pointer (&again, g_free);
      g_clear_pointer (&out, g_free);
    }
  }
  g_free (copy);
}

/* @nals (NULL-terminated pairs of data and size) as an Annex-B access unit, 4-byte start codes. */
static GByteArray *
annex_b (const guint8 * first, gsize first_size, ...)
{
  static const guint8 sc[] = { 0, 0, 0, 1 };
  GByteArray *a = g_byte_array_new ();
  const guint8 *nal = first;
  gsize size = first_size;
  va_list ap;

  va_start (ap, first_size);
  while (nal != NULL) {
    g_byte_array_append (a, sc, sizeof (sc));
    g_byte_array_append (a, nal, size);
    nal = va_arg (ap, const guint8 *);
    if (nal != NULL)
      size = va_arg (ap, gsize);
  }
  va_end (ap);
  return a;
}

/* Every prefix of every captured parameter set (as a NAL unit and as an Annex-B buffer) and of access units built
 * from them, from 0 bytes to the whole. */
static void
test_fuzz_prefixes (void)
{
  static const guint8 h265_pps[] = { 0x44, 0x01, 0xc1, 0x73, 0xd1, 0x89 };
  static const guint8 h265_idr[] = { 0x26, 0x01, 0xa0, 0x00, 0x05 };
  static const struct
  {
    const guint8 *data;
    gsize size;
  } nals[] = {
    {sps_720x480, sizeof (sps_720x480)}, {sps_1366x768, sizeof (sps_1366x768)},
    {sps_1000x600, sizeof (sps_1000x600)}, {sps_1280x720, sizeof (sps_1280x720)},
    {h264_sps_1920x1080, sizeof (h264_sps_1920x1080)}, {h264_sps_1281x721, sizeof (h264_sps_1281x721)},
    {h264_sps_high_720p, sizeof (h264_sps_high_720p)}, {h264_sps_cb_720p, sizeof (h264_sps_cb_720p)},
    {vps_1280x720, sizeof (vps_1280x720)},
  };
  GByteArray *aus[4];
  guint fed = 0;

  fuzz_fixed = 0;

  aus[0] = annex_b (h264_sps_high_720p, sizeof (h264_sps_high_720p), h264_pps, sizeof (h264_pps), h264_fill,
      sizeof (h264_fill), h264_idr, sizeof (h264_idr), NULL);
  aus[1] = annex_b (h264_sps_cb_720p, sizeof (h264_sps_cb_720p), h264_pps, sizeof (h264_pps), h264_idr,
      sizeof (h264_idr), NULL);
  aus[2] = annex_b (vps_1280x720, sizeof (vps_1280x720), sps_1366x768, sizeof (sps_1366x768), h265_pps,
      sizeof (h265_pps), h265_fill, sizeof (h265_fill), h265_idr, sizeof (h265_idr), NULL);
  aus[3] = annex_b (vps_1280x720, sizeof (vps_1280x720), sps_720x480, sizeof (sps_720x480), h265_pps,
      sizeof (h265_pps), h265_idr, sizeof (h265_idr), NULL);
  for (guint i = 0; i < G_N_ELEMENTS (nals); i++) {
    GByteArray *a = annex_b (nals[i].data, nals[i].size, NULL);

    for (gsize n = 0; n <= nals[i].size; n++, fed++)
      feed_nal (nals[i].data, n);
    for (gsize n = 0; n <= a->len; n++, fed++)
      feed_annex_b (a->data, n);
    g_byte_array_unref (a);
  }
  for (guint i = 0; i < G_N_ELEMENTS (aus); i++) {
    for (gsize n = 0; n <= aus[i]->len; n++, fed++)
      feed_annex_b (aus[i]->data, n);
    g_byte_array_unref (aus[i]);
  }
  g_test_message ("%u prefixes, %u parameter sets rewritten", fed, fuzz_fixed);
  g_assert_cmpuint (fuzz_fixed, >, 0);
}

/* Random buffers from a fixed seed: plain random bytes; random bytes with start codes and NAL headers of the types
 * the helpers look for (SPS, PPS, VPS, slices, filler); and the captured parameter sets with a few bytes changed. */
static void
test_fuzz_random (void)
{
  static const guint8 h264_types[] = { 1, 5, 6, 7, 8, 9, 12 };
  static const guint8 h265_types[] = { 0, 1, 2, 19, 20, 21, 32, 33, 34, 38, 39 };
  const guint8 *sets[] = { sps_720x480, sps_1366x768, h264_sps_1920x1080, h264_sps_cb_720p, vps_1280x720 };
  const gsize set_sizes[] = { sizeof (sps_720x480), sizeof (sps_1366x768), sizeof (h264_sps_1920x1080),
    sizeof (h264_sps_cb_720p), sizeof (vps_1280x720)
  };
  GRand *rand = g_rand_new_with_seed (20261112);
  const guint rounds = 3000;

  fuzz_fixed = 0;

  for (guint r = 0; r < rounds; r++) {
    gsize size = (gsize) g_rand_int_range (rand, 0, 160);
    guint8 *buf = g_malloc (MAX (size, 1));
    guint kind = r % 3;

    for (gsize i = 0; i < size; i++)
      buf[i] = (guint8) g_rand_int_range (rand, 0, 256);
    if (kind == 1) {
      /* start codes and NAL headers every few bytes; zero runs too */
      for (gsize i = 0; i + 5 < size; i += (gsize) g_rand_int_range (rand, 4, 24)) {
        gboolean hevc = g_rand_boolean (rand);

        buf[i] = buf[i + 1] = 0;
        buf[i + 2] = 1;
        if (hevc) {
          buf[i + 3] = (guint8) (h265_types[g_rand_int_range (rand, 0, G_N_ELEMENTS (h265_types))] << 1);
          buf[i + 4] = 1;
        } else {
          buf[i + 3] = 0x60 | h264_types[g_rand_int_range (rand, 0, G_N_ELEMENTS (h264_types))];
        }
        if (g_rand_int_range (rand, 0, 4) == 0 && i + 8 < size)
          buf[i + 5] = buf[i + 6] = buf[i + 7] = 0;
      }
    } else if (kind == 2) {
      /* a captured parameter set, a few bytes changed, cut at a random length */
      guint s = (guint) g_rand_int_range (rand, 0, G_N_ELEMENTS (sets));
      gsize n = MIN (size, set_sizes[s]);

      memcpy (buf, sets[s], n);
      size = n;
      for (guint k = g_rand_int_range (rand, 1, 4); k > 0 && n > 0; k--)
        buf[g_rand_int_range (rand, 0, (gint32) n)] = (guint8) g_rand_int_range (rand, 0, 256);
      {
        GByteArray *a = annex_b (buf, size, NULL);

        feed_annex_b (a->data, a->len);
        g_byte_array_unref (a);
      }
    }
    feed_nal (buf, size);
    feed_annex_b (buf, size);
    g_free (buf);
  }
  g_rand_free (rand);
  g_test_message ("%u random buffers, %u parameter sets rewritten", rounds, fuzz_fixed);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  ref_caps = gst_caps_new_empty_simple ("timestamp/x-test");

  g_test_add_data_func ("/cpu/enc-nal/filler-h264-positions", GINT_TO_POINTER (RDS_ENC_CODEC_H264),
      test_filler_positions);
  g_test_add_data_func ("/cpu/enc-nal/filler-h265-positions", GINT_TO_POINTER (RDS_ENC_CODEC_H265),
      test_filler_positions);
  g_test_add_func ("/cpu/enc-nal/filler-zero-bytes", test_filler_zero_bytes);
  g_test_add_func ("/cpu/enc-nal/filler-codec", test_filler_codec);
  g_test_add_func ("/cpu/enc-nal/filler-all", test_filler_all);
  g_test_add_func ("/cpu/enc-nal/ts-unshift", test_ts_unshift);
  g_test_add_func ("/cpu/enc-nal/ts-segment-shift", test_segment_shift);
  g_test_add_func ("/cpu/enc-nal/ts-segment-unshift", test_segment_unshift);
  g_test_add_func ("/cpu/enc-nal/walker-sps", test_walker_sps);
  g_test_add_func ("/cpu/enc-nal/sps-captured", test_sps_fix_captured);
  g_test_add_func ("/cpu/enc-nal/sps-twice", test_sps_fix_twice);
  g_test_add_func ("/cpu/enc-nal/sps-invalid", test_sps_fix_invalid);
  g_test_add_func ("/cpu/enc-nal/sps-emulation", test_sps_fix_emulation);
  g_test_add_func ("/cpu/enc-nal/sps-emulation-captured", test_sps_fix_emulation_captured);
  g_test_add_func ("/cpu/enc-nal/sps-buffer", test_sps_fix_buffer);
  g_test_add_func ("/cpu/enc-nal/level-map", test_level_map);
  g_test_add_func ("/cpu/enc-nal/level-h264", test_level_h264);
  g_test_add_func ("/cpu/enc-nal/level-h265", test_level_h265);
  g_test_add_func ("/cpu/enc-nal/level-emulation", test_level_emulation);
  g_test_add_func ("/cpu/enc-nal/level-buffer", test_level_buffer);
  g_test_add_func ("/cpu/enc-nal/level-with-window", test_level_with_window);
  g_test_add_func ("/cpu/enc-nal/intra-h264", test_intra_h264);
  g_test_add_func ("/cpu/enc-nal/intra-h265", test_intra_h265);
  g_test_add_func ("/cpu/enc-nal/fuzz-prefixes", test_fuzz_prefixes);
  g_test_add_func ("/cpu/enc-nal/fuzz-random", test_fuzz_random);

  g_test_add_func ("/gpu/enc-nal/walker-h264", test_gpu_walker_h264);
  g_test_add_func ("/gpu/enc-nal/walker-h265", test_gpu_walker_h265);
  g_test_add_data_func ("/gpu/enc-nal/filler-h264", GINT_TO_POINTER (RDS_ENC_CODEC_H264), test_gpu_filler);
  g_test_add_data_func ("/gpu/enc-nal/filler-h265", GINT_TO_POINTER (RDS_ENC_CODEC_H265), test_gpu_filler);
  g_test_add_func ("/gpu/enc-nal/ts-va-shift", test_gpu_va_shift);
  g_test_add_func ("/gpu/enc-nal/sps-round-trip", test_gpu_sps_round_trip);

  ret = g_test_run ();
  gst_caps_unref (ref_caps);
  gst_deinit ();
  return ret;
}
