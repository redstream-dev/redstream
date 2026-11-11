/* A small Annex-B walker for the encoder tests, written separately from the plugin's own NAL code so it checks that
 * code independently: NAL units and access units of a byte stream, NAL types, the first slice's slice_type
 * (H.264), the SPS profile_idc / level_idc and the SPS cropping (H.264 frame cropping, HEVC conformance window) with
 * the displayed size. Header-only (static inline functions); fails the test on a stream it can't read.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_NAL_H
#define RDS_TEST_NAL_H

#include <glib.h>
#include <string.h>

/* One NAL unit of a byte stream: payload from the NAL header to its last non-zero byte (no start code). */
typedef struct
{
  const guint8 *data;
  gsize size;
  guint type;                   /* nal_unit_type */
} RdsTestNal;

/* Splits @size bytes of Annex-B @data into NAL units (GArray of RdsTestNal pointing into @data). */
static inline GArray *
rds_test_nal_split (const guint8 * data, gsize size, gboolean hevc)
{
  GArray *nals = g_array_new (FALSE, TRUE, sizeof (RdsTestNal));
  gssize start = -1;            /* payload start of the NAL being collected */
  gsize i = 0;

  while (i <= size) {
    gboolean at_sc = i + 3 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1;

    if (at_sc || i == size) {
      if (start >= 0) {
        gsize end = i;
        RdsTestNal n;

        while (end > (gsize) start && data[end - 1] == 0)
          end--;
        if (end > (gsize) start) {
          n.data = data + start;
          n.size = end - start;
          n.type = hevc ? (n.data[0] >> 1) & 0x3f : n.data[0] & 0x1f;
          g_array_append_val (nals, n);
        }
      }
      if (i == size)
        break;
      start = (gssize) (i + 3);
      i += 3;
      continue;
    }
    i++;
  }
  return nals;
}

/* Number of NAL units of @type in @data. */
static inline guint
rds_test_nal_count (const guint8 * data, gsize size, gboolean hevc, guint type)
{
  GArray *nals = rds_test_nal_split (data, size, hevc);
  guint n = 0;

  for (guint i = 0; i < nals->len; i++)
    n += g_array_index (nals, RdsTestNal, i).type == type;
  g_array_unref (nals);
  return n;
}

/* The RBSP of a NAL unit (emulation prevention bytes 0x03 after 0x00 0x00 removed), header included. */
static inline GByteArray *
rds_test_nal_rbsp (const guint8 * nal, gsize size)
{
  GByteArray *out = g_byte_array_sized_new ((guint) size);
  guint zeros = 0;

  for (gsize i = 0; i < size; i++) {
    if (zeros >= 2 && nal[i] == 3) {
      zeros = 0;
      continue;
    }
    zeros = nal[i] == 0 ? zeros + 1 : 0;
    g_byte_array_append (out, nal + i, 1);
  }
  return out;
}

/* Number of emulation prevention bytes in a NAL unit. */
static inline guint
rds_test_nal_epb_count (const guint8 * nal, gsize size)
{
  GByteArray *rbsp = rds_test_nal_rbsp (nal, size);
  guint n = (guint) (size - rbsp->len);

  g_byte_array_unref (rbsp);
  return n;
}

/* MSB-first bit reader over an RBSP; reading past the end fails the test. */
typedef struct
{
  const guint8 *d;
  gsize bits;
  gsize at;
} RdsTestBits;

static inline guint
rds_test_bit (RdsTestBits * b)
{
  guint v;

  g_assert_cmpuint (b->at, <, b->bits);
  v = (b->d[b->at / 8] >> (7 - b->at % 8)) & 1;
  b->at++;
  return v;
}

static inline guint
rds_test_bits (RdsTestBits * b, guint n)
{
  guint v = 0;

  while (n--)
    v = (v << 1) | rds_test_bit (b);
  return v;
}

static inline guint
rds_test_ue (RdsTestBits * b)
{
  guint zeros = 0;

  while (rds_test_bit (b) == 0) {
    zeros++;
    g_assert_cmpuint (zeros, <, 32);
  }
  return (1u << zeros) - 1 + rds_test_bits (b, zeros);
}

static inline gint
rds_test_se (RdsTestBits * b)
{
  guint k = rds_test_ue (b);

  return (k & 1) ? (gint) ((k + 1) / 2) : -(gint) (k / 2);
}

/* What the tests read from an SPS. */
typedef struct
{
  guint profile_idc;
  guint level_idc;
  guint chroma_format_idc;
  guint coded_width, coded_height;      /* luma samples */
  gboolean cropping;            /* frame_cropping_flag / conformance_window_flag */
  guint crop_left, crop_right, crop_top, crop_bottom;   /* as coded (crop units) */
  guint width, height;          /* displayed size */
  gsize window_bit;             /* HEVC: RBSP bit offset (header included) of conformance_window_flag */
  gsize tail_bit;               /* HEVC: RBSP bit offset just after the conformance window */
} RdsTestSps;

static inline void
rds_test_h264_skip_scaling_list (RdsTestBits * b, guint n)
{
  gint last = 8, next = 8;

  for (guint j = 0; j < n; j++) {
    if (next != 0)
      next = (last + rds_test_se (b) + 256) % 256;
    last = next == 0 ? last : next;
  }
}

/* Parses an H.264 SPS NAL unit (header byte included). */
static inline void
rds_test_h264_sps (const guint8 * nal, gsize size, RdsTestSps * sps)
{
  GByteArray *rbsp = rds_test_nal_rbsp (nal, size);
  RdsTestBits b = { rbsp->data, rbsp->len * 8, 8 };
  guint frame_mbs_only, sub_w = 2, sub_h = 2, unit_x, unit_y;

  memset (sps, 0, sizeof (*sps));
  g_assert_cmpuint (nal[0] & 0x1f, ==, 7);
  sps->profile_idc = rds_test_bits (&b, 8);
  (void) rds_test_bits (&b, 8); /* constraint flags */
  sps->level_idc = rds_test_bits (&b, 8);
  (void) rds_test_ue (&b);      /* seq_parameter_set_id */
  sps->chroma_format_idc = 1;
  switch (sps->profile_idc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139:
    case 134: case 135:
      sps->chroma_format_idc = rds_test_ue (&b);
      if (sps->chroma_format_idc == 3)
        (void) rds_test_bit (&b);       /* separate_colour_plane_flag */
      (void) rds_test_ue (&b);  /* bit_depth_luma_minus8 */
      (void) rds_test_ue (&b);  /* bit_depth_chroma_minus8 */
      (void) rds_test_bit (&b); /* qpprime_y_zero_transform_bypass_flag */
      if (rds_test_bit (&b)) {  /* seq_scaling_matrix_present_flag */
        for (guint i = 0; i < (sps->chroma_format_idc != 3 ? 8u : 12u); i++)
          if (rds_test_bit (&b))
            rds_test_h264_skip_scaling_list (&b, i < 6 ? 16 : 64);
      }
      break;
    default:
      break;
  }
  (void) rds_test_ue (&b);      /* log2_max_frame_num_minus4 */
  switch (rds_test_ue (&b)) {   /* pic_order_cnt_type */
    case 0:
      (void) rds_test_ue (&b);
      break;
    case 1:{
      guint cycle;

      (void) rds_test_bit (&b);
      (void) rds_test_se (&b);
      (void) rds_test_se (&b);
      cycle = rds_test_ue (&b);
      for (guint i = 0; i < cycle; i++)
        (void) rds_test_se (&b);
      break;
    }
    default:
      break;
  }
  (void) rds_test_ue (&b);      /* max_num_ref_frames */
  (void) rds_test_bit (&b);     /* gaps_in_frame_num_value_allowed_flag */
  sps->coded_width = (rds_test_ue (&b) + 1) * 16;
  sps->coded_height = rds_test_ue (&b) + 1;     /* map units */
  frame_mbs_only = rds_test_bit (&b);
  sps->coded_height *= (2 - frame_mbs_only) * 16;
  if (!frame_mbs_only)
    (void) rds_test_bit (&b);   /* mb_adaptive_frame_field_flag */
  (void) rds_test_bit (&b);     /* direct_8x8_inference_flag */
  sps->cropping = rds_test_bit (&b);
  if (sps->cropping) {
    sps->crop_left = rds_test_ue (&b);
    sps->crop_right = rds_test_ue (&b);
    sps->crop_top = rds_test_ue (&b);
    sps->crop_bottom = rds_test_ue (&b);
  }
  if (sps->chroma_format_idc == 0 || sps->chroma_format_idc == 3)
    sub_w = sub_h = 1;
  else if (sps->chroma_format_idc == 2)
    sub_h = 1;
  unit_x = sps->chroma_format_idc == 0 ? 1 : sub_w;
  unit_y = (sps->chroma_format_idc == 0 ? 1 : sub_h) * (2 - frame_mbs_only);
  sps->width = sps->coded_width - unit_x * (sps->crop_left + sps->crop_right);
  sps->height = sps->coded_height - unit_y * (sps->crop_top + sps->crop_bottom);
  g_byte_array_unref (rbsp);
}

/* Parses an HEVC SPS NAL unit (2-byte header included) up to the conformance window. */
static inline void
rds_test_h265_sps (const guint8 * nal, gsize size, RdsTestSps * sps)
{
  GByteArray *rbsp = rds_test_nal_rbsp (nal, size);
  RdsTestBits b = { rbsp->data, rbsp->len * 8, 16 };
  guint max_sub, sub_profile[8] = { 0 }, sub_level[8] = { 0 }, sub_w = 2, sub_h = 2;

  memset (sps, 0, sizeof (*sps));
  g_assert_cmpuint ((nal[0] >> 1) & 0x3f, ==, 33);
  (void) rds_test_bits (&b, 4); /* sps_video_parameter_set_id */
  max_sub = rds_test_bits (&b, 3);
  (void) rds_test_bit (&b);     /* sps_temporal_id_nesting_flag */
  /* profile_tier_level (1, max_sub) */
  (void) rds_test_bits (&b, 3); /* general_profile_space, general_tier_flag */
  sps->profile_idc = rds_test_bits (&b, 5);
  for (guint i = 0; i < 32 + 4 + 43 + 1; i++)   /* compatibility flags, source flags, constraints, inbld */
    (void) rds_test_bit (&b);
  sps->level_idc = rds_test_bits (&b, 8);
  for (guint i = 0; i < max_sub; i++) {
    sub_profile[i] = rds_test_bit (&b);
    sub_level[i] = rds_test_bit (&b);
  }
  if (max_sub > 0)
    for (guint i = max_sub; i < 8; i++)
      (void) rds_test_bits (&b, 2);
  for (guint i = 0; i < max_sub; i++) {
    if (sub_profile[i])
      for (guint k = 0; k < 88; k++)
        (void) rds_test_bit (&b);
    if (sub_level[i])
      (void) rds_test_bits (&b, 8);
  }
  (void) rds_test_ue (&b);      /* sps_seq_parameter_set_id */
  sps->chroma_format_idc = rds_test_ue (&b);
  if (sps->chroma_format_idc == 3 && rds_test_bit (&b))
    sps->chroma_format_idc = 0; /* separate colour planes: crop units of 1, as monochrome */
  sps->coded_width = rds_test_ue (&b);
  sps->coded_height = rds_test_ue (&b);
  sps->window_bit = b.at;
  sps->cropping = rds_test_bit (&b);
  if (sps->cropping) {
    sps->crop_left = rds_test_ue (&b);
    sps->crop_right = rds_test_ue (&b);
    sps->crop_top = rds_test_ue (&b);
    sps->crop_bottom = rds_test_ue (&b);
  }
  sps->tail_bit = b.at;
  if (sps->chroma_format_idc == 0 || sps->chroma_format_idc == 3)
    sub_w = sub_h = 1;
  else if (sps->chroma_format_idc == 2)
    sub_h = 1;
  sps->width = sps->coded_width - sub_w * (sps->crop_left + sps->crop_right);
  sps->height = sps->coded_height - sub_h * (sps->crop_top + sps->crop_bottom);
  g_byte_array_unref (rbsp);
}

/* The RBSP bits of an HEVC SPS from just after its conformance window up to (not including) the
 * rbsp_stop_one_bit, as a '0'/'1' string (g_free), to compare two SPSs field for field after the window. */
static inline gchar *
rds_test_h265_sps_tail (const guint8 * nal, gsize size)
{
  RdsTestSps sps;
  GByteArray *rbsp;
  GString *s = g_string_new (NULL);
  gssize stop;

  rds_test_h265_sps (nal, size, &sps);
  rbsp = rds_test_nal_rbsp (nal, size);
  for (stop = (gssize) rbsp->len * 8 - 1; stop >= 0; stop--)
    if ((rbsp->data[stop / 8] >> (7 - stop % 8)) & 1)
      break;
  g_assert_cmpint (stop, >=, (gssize) sps.tail_bit);
  for (gsize i = sps.tail_bit; i < (gsize) stop; i++)
    g_string_append_c (s, '0' + ((rbsp->data[i / 8] >> (7 - i % 8)) & 1));
  g_byte_array_unref (rbsp);
  return g_string_free (s, FALSE);
}

/* H.264 slice_type of a slice NAL unit (1 or 5) modulo 5: 0 P, 1 B, 2 I, 3 SP, 4 SI. */
static inline guint
rds_test_h264_slice_type (const guint8 * nal, gsize size)
{
  GByteArray *rbsp = rds_test_nal_rbsp (nal, size);
  RdsTestBits b = { rbsp->data, rbsp->len * 8, 8 };
  guint t;

  (void) rds_test_ue (&b);      /* first_mb_in_slice */
  t = rds_test_ue (&b) % 5;
  g_byte_array_unref (rbsp);
  return t;
}

#define RDS_TEST_H264_SLICE_P 0
#define RDS_TEST_H264_SLICE_B 1
#define RDS_TEST_H264_SLICE_I 2

/* One access unit of a byte stream: its NAL types in order, and for H.264 the first slice's slice_type (G_MAXUINT
 * when the AU has no slice). */
typedef struct
{
  GArray *types;                /* guint */
  guint first_slice_type;
} RdsTestAu;

static inline void
rds_test_au_clear (gpointer p)
{
  g_array_unref (((RdsTestAu *) p)->types);
}

static inline gboolean
rds_test_nal_is_vcl (const RdsTestNal * n, gboolean hevc)
{
  return hevc ? n->type < 32 : (n->type >= 1 && n->type <= 5);
}

/* Whether a VCL NAL unit starts a new picture: H.264 first_mb_in_slice == 0, HEVC first_slice_segment_in_pic_flag. */
static inline gboolean
rds_test_nal_first_slice (const RdsTestNal * n, gboolean hevc)
{
  if (hevc)
    return n->size > 2 && (n->data[2] & 0x80) != 0;
  /* first_mb_in_slice is ue(v): 0 is the single bit 1 */
  return n->size > 1 && (n->data[1] & 0x80) != 0;
}

/* Splits a whole byte stream into access units (GArray of RdsTestAu): a new AU starts at the first VCL NAL unit of
 * a picture or at a non-VCL NAL unit that precedes one (AUD, parameter sets, SEI) once the current AU has a slice. */
static inline GArray *
rds_test_nal_aus (const guint8 * data, gsize size, gboolean hevc)
{
  GArray *nals = rds_test_nal_split (data, size, hevc);
  GArray *aus = g_array_new (FALSE, TRUE, sizeof (RdsTestAu));
  RdsTestAu *cur = NULL;
  gboolean have_vcl = FALSE;

  g_array_set_clear_func (aus, rds_test_au_clear);
  for (guint i = 0; i < nals->len; i++) {
    const RdsTestNal *n = &g_array_index (nals, RdsTestNal, i);
    gboolean vcl = rds_test_nal_is_vcl (n, hevc);
    gboolean prefix = hevc ? (n->type >= 32 && n->type <= 35) || n->type == 39
        : n->type == 6 || n->type == 7 || n->type == 8 || n->type == 9;

    if (cur == NULL || (have_vcl && ((vcl && rds_test_nal_first_slice (n, hevc)) || prefix))) {
      RdsTestAu au = { g_array_new (FALSE, FALSE, sizeof (guint)), G_MAXUINT };

      g_array_append_val (aus, au);
      cur = &g_array_index (aus, RdsTestAu, aus->len - 1);
      have_vcl = FALSE;
    }
    g_array_append_val (cur->types, n->type);
    if (vcl && !have_vcl && !hevc)
      cur->first_slice_type = rds_test_h264_slice_type (n->data, n->size);
    have_vcl |= vcl;
  }
  g_array_unref (nals);
  return aus;
}

/* Whether @au has a NAL unit of @type. */
static inline gboolean
rds_test_au_has (const RdsTestAu * au, guint type)
{
  for (guint i = 0; i < au->types->len; i++)
    if (g_array_index (au->types, guint, i) == type)
      return TRUE;
  return FALSE;
}

#endif /* RDS_TEST_NAL_H */
