/* Redstream: the Annex-B helpers of rdsh264enc and rdsh265enc: filler NAL stripping, the H.265 SPS
 * conformance-window fix (written from the H.265 specification, sections 7.3.2.2 and 7.3.3) and the removal of the
 * VA encoders' timestamp shift; intra access units by their slice headers (H.264 section 7.3.3, H.265 sections
 * 7.3.2.3 and 7.3.6.1).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-enc-nal.h"

#include <string.h>

GST_DEBUG_CATEGORY_STATIC (rds_enc_nal_debug);
#define GST_CAT_DEFAULT rds_enc_nal_debug

static void
debug_init (void)
{
  static gsize done = 0;

  if (g_once_init_enter (&done)) {
    GST_DEBUG_CATEGORY_INIT (rds_enc_nal_debug, "rdsenc-nal", 0, "rdsh264enc / rdsh265enc Annex-B helpers");
    g_once_init_leave (&done, 1);
  }
}

/* @in can't be mapped: passed through unchanged. */
static GstBuffer *
pass_unmapped (GstBuffer * in, const gchar * what)
{
  debug_init ();
  GST_WARNING ("can't map buffer %" GST_PTR_FORMAT " for reading: %s skipped", in, what);
  return gst_buffer_ref (in);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Annex-B scanning */

/* One NAL unit of a byte stream. Bytes [lead, sc) are zero bytes before its start code (a 4-byte start code's
 * zero_byte, or trailing zero bytes of whatever came before), [sc, payload) the 3-byte start code prefix,
 * [payload, end) the NAL unit itself; zero bytes from end up to the next unit's lead belong to no NAL unit. */
typedef struct
{
  gsize lead, sc, payload, end;
} NalSpan;

/* Splits @data into NAL units. Bytes before the first start code (other than zero bytes) are left out of every
 * span; they are kept as they are by the callers. */
static GArray *
nal_scan (const guint8 * data, gsize size)
{
  GArray *spans = g_array_new (FALSE, FALSE, sizeof (NalSpan));
  gsize i = 0, floor = 0;       /* zero runs never reach back past the previous payload's start */

  while (i + 3 <= size) {
    NalSpan s;

    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
      i++;
      continue;
    }
    s.sc = i;
    s.lead = i;
    while (s.lead > floor && data[s.lead - 1] == 0)
      s.lead--;
    s.payload = i + 3;
    if (spans->len > 0) {
      NalSpan *prev = &g_array_index (spans, NalSpan, spans->len - 1);

      prev->end = s.lead;
    }
    s.end = size;
    g_array_append_val (spans, s);
    floor = s.payload;
    i = s.payload;
  }
  if (spans->len > 0) {
    NalSpan *last = &g_array_index (spans, NalSpan, spans->len - 1);

    while (last->end > last->payload && data[last->end - 1] == 0)
      last->end--;
  }
  return spans;
}

static guint
nal_type (const guint8 * data, const NalSpan * s, RdsEncCodec codec)
{
  if (s->end <= s->payload)
    return G_MAXUINT;           /* a start code with nothing after it */
  return codec == RDS_ENC_CODEC_H264 ? data[s->payload] & 0x1f : (data[s->payload] >> 1) & 0x3f;
}

/* A new buffer of @size bytes with @in's timestamps, offsets, flags and metas. */
static GstBuffer *
buffer_like (GstBuffer * in, const guint8 * bytes, gsize size)
{
  GstBuffer *out = size > 0 ? gst_buffer_new_memdup (bytes, size) : gst_buffer_new ();

  gst_buffer_copy_into (out, in, GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META, 0,
      (gsize) - 1);
  return out;
}

GstBuffer *
rds_enc_strip_filler (GstBuffer * in, RdsEncCodec codec)
{
  guint filler = codec == RDS_ENC_CODEC_H264 ? RDS_ENC_H264_NAL_FILLER : RDS_ENC_H265_NAL_FILLER;
  GstBuffer *out = NULL;
  GByteArray *kept = NULL;
  GstMapInfo map;
  GArray *spans;
  gsize copied = 0;             /* input bytes before this offset are dealt with */

  g_return_val_if_fail (GST_IS_BUFFER (in), NULL);
  if (!gst_buffer_map (in, &map, GST_MAP_READ))
    return pass_unmapped (in, "filler stripping");
  spans = nal_scan (map.data, map.size);
  for (guint i = 0; i < spans->len; i++) {
    const NalSpan *s = &g_array_index (spans, NalSpan, i);
    gsize cut_end;

    if (nal_type (map.data, s, codec) != filler)
      continue;
    if (kept == NULL)
      kept = g_byte_array_sized_new ((guint) map.size);
    /* Drop from the zero run before its start code to the next unit's start code, keeping the one zero byte that
     * made that a 4-byte start code. */
    if (i + 1 < spans->len) {
      const NalSpan *next = &g_array_index (spans, NalSpan, i + 1);

      cut_end = next->sc > next->lead ? next->sc - 1 : next->sc;
    } else {
      cut_end = map.size;
    }
    /* After a removed filler, @copied sits on the zero byte kept for this unit's start code; when this unit is a
     * filler too, that byte is part of its zero run and goes with it. */
    if (s->lead > copied)
      g_byte_array_append (kept, map.data + copied, (guint) (s->lead - copied));
    copied = cut_end;
  }
  if (kept != NULL) {
    g_byte_array_append (kept, map.data + copied, (guint) (map.size - copied));
    out = buffer_like (in, kept->data, kept->len);
    g_byte_array_unref (kept);
  }
  g_array_unref (spans);
  gst_buffer_unmap (in, &map);
  return out != NULL ? out : gst_buffer_ref (in);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Timestamp shift */

GstClockTime
rds_enc_segment_shift (const GstSegment * in, const GstSegment * out)
{
  g_return_val_if_fail (in != NULL && out != NULL, 0);
  if (in->format != GST_FORMAT_TIME || out->format != GST_FORMAT_TIME || out->start < in->start)
    return 0;
  return out->start - in->start;
}

GstClockTime
rds_enc_ts_unshift (GstClockTime t, GstClockTime shift)
{
  if (!GST_CLOCK_TIME_IS_VALID (t) || !GST_CLOCK_TIME_IS_VALID (shift))
    return t;
  return t >= shift ? t - shift : 0;    /* ours: clamp rather than wrap */
}

void
rds_enc_segment_unshift (GstSegment * segment, GstClockTime shift)
{
  g_return_if_fail (segment != NULL);
  if (segment->format != GST_FORMAT_TIME || shift == 0)
    return;
  segment->start = rds_enc_ts_unshift (segment->start, shift);
  segment->stop = rds_enc_ts_unshift (segment->stop, shift);
  segment->position = rds_enc_ts_unshift (segment->position, shift);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* H.265 SPS conformance window */

typedef struct
{
  const guint8 *d;
  gsize bits;
  gsize pos;
  gboolean bad;                 /* read past the end or an over-long exp-Golomb code */
} BitReader;

static guint32
br_bits (BitReader * r, guint n)
{
  guint32 v = 0;

  for (guint k = 0; k < n; k++) {
    if (r->pos >= r->bits) {
      r->bad = TRUE;
      return 0;
    }
    v = (v << 1) | ((r->d[r->pos / 8] >> (7 - r->pos % 8)) & 1);
    r->pos++;
  }
  return v;
}

static guint32
br_ue (BitReader * r)
{
  guint lz = 0;

  while (!r->bad && br_bits (r, 1) == 0)
    if (++lz > 31) {
      r->bad = TRUE;
      return 0;
    }
  if (r->bad)
    return 0;
  return (guint32) (((guint64) 1 << lz) - 1 + br_bits (r, lz));
}

typedef struct
{
  GByteArray *a;
  guint8 cur;
  guint n;                      /* bits in cur */
} BitWriter;

static void
bw_bits (BitWriter * w, guint32 v, guint n)
{
  while (n--) {
    w->cur = (guint8) ((w->cur << 1) | ((v >> n) & 1));
    if (++w->n == 8) {
      g_byte_array_append (w->a, &w->cur, 1);
      w->cur = 0;
      w->n = 0;
    }
  }
}

static void
bw_ue (BitWriter * w, guint32 v)
{
  guint64 code = (guint64) v + 1;
  guint len = 0;

  while ((code >> len) > 1)
    len++;
  bw_bits (w, 0, len);
  /* the 1 and the len low bits; len <= 32 */
  bw_bits (w, 1, 1);
  if (len > 0)
    bw_bits (w, (guint32) (code & (((guint64) 1 << len) - 1)), len);
}

/* RBSP of a NAL unit payload (after the 2-byte header): every 0x03 after two zero bytes is emulation prevention. */
static GByteArray *
nal_to_rbsp (const guint8 * p, gsize size)
{
  GByteArray *rbsp = g_byte_array_sized_new ((guint) size);
  guint zeros = 0;

  for (gsize i = 0; i < size; i++) {
    if (zeros >= 2 && p[i] == 0x03) {
      zeros = 0;
      continue;
    }
    zeros = p[i] == 0 ? zeros + 1 : 0;
    g_byte_array_append (rbsp, p + i, 1);
  }
  return rbsp;
}

/* Appends @rbsp to @out with emulation prevention: 0x03 before any byte <= 0x03 that follows two zero bytes. */
static void
rbsp_to_nal (const GByteArray * rbsp, GByteArray * out)
{
  static const guint8 epb = 0x03;
  guint zeros = 0;

  for (guint i = 0; i < rbsp->len; i++) {
    if (zeros >= 2 && rbsp->data[i] <= 0x03) {
      g_byte_array_append (out, &epb, 1);
      zeros = 0;
    }
    zeros = rbsp->data[i] == 0 ? zeros + 1 : 0;
    g_byte_array_append (out, rbsp->data + i, 1);
  }
}

/* Reads profile_tier_level (profilePresentFlag 1, @max_sub_layers_minus1), H.265 7.3.3. */
static void
skip_profile_tier_level (BitReader * r, guint max_sub_layers_minus1)
{
  gboolean profile_present[8] = { FALSE }, level_present[8] = { FALSE };

  /* general_profile_space .. general_level_idc: 2+1+5+32+4+43+1+8 bits */
  (void) br_bits (r, 8);
  (void) br_bits (r, 32);
  (void) br_bits (r, 32);
  (void) br_bits (r, 16);
  (void) br_bits (r, 8);
  for (guint i = 0; i < max_sub_layers_minus1; i++) {
    profile_present[i] = br_bits (r, 1);
    level_present[i] = br_bits (r, 1);
  }
  if (max_sub_layers_minus1 > 0)
    for (guint i = max_sub_layers_minus1; i < 8; i++)
      (void) br_bits (r, 2);    /* reserved_zero_2bits */
  for (guint i = 0; i < max_sub_layers_minus1; i++) {
    if (profile_present[i]) {   /* sub_layer_profile_space .. sub_layer_inbld_flag: 88 bits */
      (void) br_bits (r, 32);
      (void) br_bits (r, 32);
      (void) br_bits (r, 24);
    }
    if (level_present[i])
      (void) br_bits (r, 8);
  }
}

RdsEncSpsResult
rds_enc_h265_fix_sps (const guint8 * nal, gsize size, guint width, guint height, guint8 ** fixed,
    gsize * fixed_size)
{
  RdsEncSpsResult res = RDS_ENC_SPS_INVALID;
  GByteArray *rbsp, *out_rbsp = NULL, *out;
  BitReader r;
  BitWriter w;
  guint32 max_sub, chroma, sep = 0, pic_w, pic_h, win[4] = { 0 }, unit_x, unit_y, want_w, want_h, new_r, new_b;
  gsize win_start, win_end, stop;
  gboolean flag;

  g_return_val_if_fail (fixed != NULL && fixed_size != NULL, RDS_ENC_SPS_INVALID);
  /* forbidden_zero_bit 0, nal_unit_type SPS_NUT, nuh_layer_id 0 (the base layer) */
  if (nal == NULL || size < 3 || (nal[0] & 0x80) != 0 || ((nal[0] >> 1) & 0x3f) != RDS_ENC_H265_NAL_SPS
      || (((nal[0] & 1) << 5) | (nal[1] >> 3)) != 0 || width == 0 || height == 0)
    return RDS_ENC_SPS_INVALID;
  rbsp = nal_to_rbsp (nal + 2, size - 2);
  r = (BitReader) { rbsp->data, (gsize) rbsp->len * 8, 0, FALSE };

  /* seq_parameter_set_rbsp (), H.265 7.3.2.2, up to the conformance window */
  (void) br_bits (&r, 4);       /* sps_video_parameter_set_id */
  max_sub = br_bits (&r, 3);    /* sps_max_sub_layers_minus1 */
  (void) br_bits (&r, 1);       /* sps_temporal_id_nesting_flag */
  skip_profile_tier_level (&r, max_sub);
  if (r.bad)
    goto done;
  (void) br_ue (&r);            /* sps_seq_parameter_set_id */
  chroma = br_ue (&r);
  if (chroma == 3)
    sep = br_bits (&r, 1);
  pic_w = br_ue (&r);
  pic_h = br_ue (&r);
  win_start = r.pos;
  flag = br_bits (&r, 1);
  if (flag)
    for (guint i = 0; i < 4; i++)
      win[i] = br_ue (&r);
  win_end = r.pos;
  if (r.bad || chroma > 3)
    goto done;

  /* SubWidthC, SubHeightC (H.265 table 6-1) */
  unit_x = (chroma == 1 || chroma == 2) && !sep ? 2 : 1;
  unit_y = chroma == 1 && !sep ? 2 : 1;
  want_w = (width + unit_x - 1) / unit_x * unit_x;
  want_h = (height + unit_y - 1) / unit_y * unit_y;
  if ((guint64) unit_x * (win[0] + (guint64) win[1]) > pic_w
      || (guint64) unit_y * (win[2] + (guint64) win[3]) > pic_h)
    goto done;
  if (pic_w - unit_x * (win[0] + win[1]) == want_w && pic_h - unit_y * (win[2] + win[3]) == want_h) {
    res = RDS_ENC_SPS_UNCHANGED;
    goto done;
  }
  /* the left and top offsets stay; the right and bottom ones take the rest */
  if ((guint64) want_w + (guint64) unit_x * win[0] > pic_w || (guint64) want_h + (guint64) unit_y * win[2] > pic_h)
    goto done;
  new_r = (pic_w - unit_x * win[0] - want_w) / unit_x;
  new_b = (pic_h - unit_y * win[2] - want_h) / unit_y;
  if (unit_x * (win[0] + new_r) + want_w != pic_w || unit_y * (win[2] + new_b) + want_h != pic_h)
    goto done;                  /* the coded size isn't a whole number of crop units past the wanted size */

  /* the rbsp_stop_one_bit: the last 1 bit of the RBSP */
  for (stop = r.bits; stop > win_end; stop--)
    if ((rbsp->data[(stop - 1) / 8] >> (7 - (stop - 1) % 8)) & 1)
      break;
  if (stop <= win_end)
    goto done;
  stop--;

  out_rbsp = g_byte_array_sized_new (rbsp->len + 16);
  w = (BitWriter) { out_rbsp, 0, 0 };
  for (gsize i = 0; i < win_start; i++)
    bw_bits (&w, (rbsp->data[i / 8] >> (7 - i % 8)) & 1, 1);
  if (win[0] == 0 && new_r == 0 && win[2] == 0 && new_b == 0) {
    bw_bits (&w, 0, 1);
  } else {
    bw_bits (&w, 1, 1);
    bw_ue (&w, win[0]);
    bw_ue (&w, new_r);
    bw_ue (&w, win[2]);
    bw_ue (&w, new_b);
  }
  for (gsize i = win_end; i < stop; i++)
    bw_bits (&w, (rbsp->data[i / 8] >> (7 - i % 8)) & 1, 1);
  bw_bits (&w, 1, 1);           /* rbsp_stop_one_bit */
  while (w.n != 0)
    bw_bits (&w, 0, 1);         /* rbsp_alignment_zero_bit */

  out = g_byte_array_sized_new (out_rbsp->len + out_rbsp->len / 16 + 4);
  g_byte_array_append (out, nal, 2);
  rbsp_to_nal (out_rbsp, out);
  *fixed_size = out->len;
  *fixed = g_byte_array_free (out, FALSE);
  res = RDS_ENC_SPS_FIXED;

done:
  if (out_rbsp != NULL)
    g_byte_array_unref (out_rbsp);
  g_byte_array_unref (rbsp);
  return res;
}

GstBuffer *
rds_enc_h265_fix_sps_buffer (GstBuffer * in, guint width, guint height)
{
  GstBuffer *out = NULL;
  GByteArray *bytes = NULL;
  GstMapInfo map;
  GArray *spans;
  gsize copied = 0;

  g_return_val_if_fail (GST_IS_BUFFER (in), NULL);
  if (!gst_buffer_map (in, &map, GST_MAP_READ))
    return pass_unmapped (in, "SPS conformance-window fix");
  spans = nal_scan (map.data, map.size);
  for (guint i = 0; i < spans->len; i++) {
    const NalSpan *s = &g_array_index (spans, NalSpan, i);
    guint8 *sps = NULL;
    gsize sps_size = 0;

    if (nal_type (map.data, s, RDS_ENC_CODEC_H265) != RDS_ENC_H265_NAL_SPS
        || rds_enc_h265_fix_sps (map.data + s->payload, s->end - s->payload, width, height, &sps,
            &sps_size) != RDS_ENC_SPS_FIXED)
      continue;
    if (bytes == NULL)
      bytes = g_byte_array_sized_new ((guint) map.size + 16);
    g_byte_array_append (bytes, map.data + copied, (guint) (s->payload - copied));
    g_byte_array_append (bytes, sps, (guint) sps_size);
    copied = s->end;
    g_free (sps);
  }
  if (bytes != NULL) {
    g_byte_array_append (bytes, map.data + copied, (guint) (map.size - copied));
    out = buffer_like (in, bytes->data, bytes->len);
    g_byte_array_unref (bytes);
  }
  g_array_unref (spans);
  gst_buffer_unmap (in, &map);
  return out != NULL ? out : gst_buffer_ref (in);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Intra access units */

/* Enough of a NAL unit's start for the PPS and slice header fields read here, without emulation prevention. */
#define HEADER_PREFIX 32

/* @r over the RBSP of the first bytes of NAL unit @nal (header included); @rbsp holds the bytes. */
static void
br_init_prefix (BitReader * r, GByteArray ** rbsp, const guint8 * nal, gsize size)
{
  *rbsp = nal_to_rbsp (nal, MIN (size, HEADER_PREFIX));
  r->d = (*rbsp)->data;
  r->bits = (gsize) (*rbsp)->len * 8;
  r->pos = 0;
  r->bad = FALSE;
}

/* HEVC PPS: num_extra_slice_header_bits, or -1 when cut short. */
static gint
h265_pps_extra_bits (const guint8 * nal, gsize size)
{
  GByteArray *rbsp;
  BitReader r;
  guint32 v;

  br_init_prefix (&r, &rbsp, nal, size);
  r.pos = 16;                   /* the NAL unit header */
  (void) br_ue (&r);            /* pps_pic_parameter_set_id */
  (void) br_ue (&r);            /* pps_seq_parameter_set_id */
  (void) br_bits (&r, 2);       /* dependent_slice_segments_enabled_flag, output_flag_present_flag */
  v = br_bits (&r, 3);
  g_byte_array_unref (rbsp);
  return r.bad ? -1 : (gint) v;
}

/* The slice_type of a first slice (H.264: modulo 5, so 0 P, 1 B, 2 I, 3 SP, 4 SI; HEVC: 0 B, 1 P, 2 I), or -1. */
static gint
slice_type (const guint8 * nal, gsize size, RdsEncCodec codec, guint type, guint extra_bits)
{
  GByteArray *rbsp;
  BitReader r;
  guint32 t;

  br_init_prefix (&r, &rbsp, nal, size);
  if (codec == RDS_ENC_CODEC_H264) {
    r.pos = 8;
    (void) br_ue (&r);          /* first_mb_in_slice */
    t = br_ue (&r) % 5;
  } else {
    r.pos = 16;
    if (br_bits (&r, 1) == 0)   /* first_slice_segment_in_pic_flag */
      r.bad = TRUE;
    if (type >= 16 && type <= 23)
      (void) br_bits (&r, 1);   /* no_output_of_prior_pics_flag */
    (void) br_ue (&r);          /* slice_pic_parameter_set_id */
    (void) br_bits (&r, extra_bits);    /* slice_reserved_flag[] */
    t = br_ue (&r);
  }
  g_byte_array_unref (rbsp);
  return r.bad ? -1 : (gint) t;
}

/* The end of the NAL unit starting at @from: the next start code (or zero_byte before it), or @limit. */
static gsize
nal_end (const guint8 * data, gsize from, gsize limit)
{
  for (gsize end = from; end + 3 <= limit; end++) {
    if (data[end] == 0 && data[end + 1] == 0 && data[end + 2] <= 1)
      return end;
  }
  return limit;
}

gboolean
rds_enc_au_is_intra (const guint8 * data, gsize size, RdsEncCodec codec, guint * hevc_extra_bits)
{
  gsize i = 0;

  g_return_val_if_fail (data != NULL || size == 0, FALSE);
  g_return_val_if_fail (hevc_extra_bits != NULL, FALSE);
  /* the NAL units up to the first slice, whose header only is read: the picture's data is never scanned */
  while (i + 3 <= size) {
    const guint8 *nal;
    gsize start;
    guint type;
    gint st;

    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
      i++;
      continue;
    }
    start = i + 3;
    if (start >= size)
      break;
    nal = data + start;
    if (codec == RDS_ENC_CODEC_H264) {
      type = nal[0] & 0x1f;
      if (type == 5)
        return TRUE;
      if (type >= 2 && type <= 4)       /* data partitions: VA writes none */
        return FALSE;
      if (type == 1) {
        st = slice_type (nal, nal_end (data, start, MIN (size, start + HEADER_PREFIX)) - start, codec, type, 0);
        return st == 2 || st == 4;
      }
    } else {
      type = (nal[0] >> 1) & 0x3f;
      if (type >= 16 && type <= 23)
        return TRUE;
      if (type <= 31) {
        st = slice_type (nal, nal_end (data, start, MIN (size, start + HEADER_PREFIX)) - start, codec, type,
            *hevc_extra_bits);
        return st == 2;
      }
      if (type == 34) {
        gint extra = h265_pps_extra_bits (nal, nal_end (data, start, MIN (size, start + HEADER_PREFIX)) - start);

        if (extra >= 0)
          *hevc_extra_bits = (guint) extra;
      }
    }
    /* a parameter set, SEI or delimiter: on to the next start code */
    i = nal_end (data, start, size);
  }
  return FALSE;
}
