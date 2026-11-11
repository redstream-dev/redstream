/* Redstream: the Annex-B helpers of rdsh264enc and rdsh265enc (plugin-internal, not installed): filler NAL
 * stripping, the H.265 SPS conformance-window fix, the removal of the VA encoders' timestamp shift and telling intra
 * access units. Buffers and segments only, no element code, so the cpu tests link it without any plugin.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ENC_NAL_H
#define RDS_ENC_NAL_H

#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum
{
  RDS_ENC_CODEC_H264,
  RDS_ENC_CODEC_H265,
} RdsEncCodec;

/* Pad-probe contract of rds_enc_strip_filler and rds_enc_h265_fix_sps_buffer: the result is transfer full and the
 * caller keeps its own reference on @in. When the result differs from @in, the probe unrefs @in and puts the result
 * in GST_PAD_PROBE_INFO_DATA; when the result has size 0 (only filler), the probe returns GST_PAD_PROBE_DROP. A
 * buffer that can't be mapped for reading is passed through unchanged (@in with an added reference) after a
 * GST_WARNING in the "rdsenc-nal" debug category. */

/* Filler data NAL unit types: H.264 FILLER (12) and HEVC FD_NUT (38). */
#define RDS_ENC_H264_NAL_FILLER 12
#define RDS_ENC_H265_NAL_FILLER 38
/* HEVC SPS_NUT. */
#define RDS_ENC_H265_NAL_SPS 33

/* Annex-B byte stream in @in (3- and 4-byte start codes) without its filler NAL units. Returns @in with an added
 * reference when it has none; otherwise a new buffer with @in's timestamps, offsets, flags and metas (copied as
 * gst_buffer_copy_into with GST_BUFFER_COPY_META does) and every other byte unchanged. Zero bytes between NAL
 * units belong to no NAL unit: a removed filler takes its own start code and the zero bytes around it, except the
 * one zero byte that makes the next NAL unit's start code a 4-byte one, so neighbours keep their start codes. A
 * buffer of filler only gives an empty buffer (the caller drops it). */
G_GNUC_INTERNAL GstBuffer *rds_enc_strip_filler (GstBuffer * in, RdsEncCodec codec);

/* The VA encoders' timestamp shift: they move PTS, DTS and the segment start forward by the same amount (1000 h
 * minus the first input PTS when that is below 1000 h, else 0), so running time is unchanged. The shift is measured,
 * never assumed: rds_enc_segment_shift gives @out->start - @in->start for the encoder's input segment @in and output
 * segment @out, both in TIME format; 0 when either is not TIME or @out starts before @in. */
G_GNUC_INTERNAL GstClockTime rds_enc_segment_shift (const GstSegment * in, const GstSegment * out);

/* @t - @shift. GST_CLOCK_TIME_NONE stays NONE; a @t below @shift clamps to 0 rather than wrapping (ours). The bin
 * removes the shift only without B frames, where VA's output PTS = DTS is never below it; with B frames it keeps
 * VA's shift (the first DTS precedes the first PTS), so the clamp never hits a DTS. */
G_GNUC_INTERNAL GstClockTime rds_enc_ts_unshift (GstClockTime t, GstClockTime shift);

/* Removes @shift from a TIME segment (start, stop and position, as rds_enc_ts_unshift; time, base, offset and
 * duration stay), so buffers unshifted by rds_enc_ts_unshift keep the input's running and stream times. Other
 * formats are left alone. */
G_GNUC_INTERNAL void rds_enc_segment_unshift (GstSegment * segment, GstClockTime shift);

typedef enum
{
  RDS_ENC_SPS_UNCHANGED,        /* the window already gives the wanted size */
  RDS_ENC_SPS_FIXED,            /* a rewritten SPS was returned */
  RDS_ENC_SPS_INVALID,          /* not a parseable base-layer HEVC SPS, or the coded size is smaller than the
                                 * wanted one */
} RdsEncSpsResult;

/* H.265 conformance-window fix (ours: VA on this Mesa pads the SPS width to a multiple of 64 but computes the
 * window from a multiple of 16, so most widths decode too wide). @nal is one HEVC SPS NAL unit, 2-byte header
 * included, no start code (trailing zero bytes allowed); a set forbidden_zero_bit or a nuh_layer_id other than 0 is
 * RDS_ENC_SPS_INVALID. The wanted displayed size is @width x @height rounded up
 * to the crop unit (SubWidthC, SubHeightC: 2 x 2 for 4:2:0, so the next even size). When pic_width/height minus
 * the window differs from it, the window is rewritten: the left and top offsets stay, the right and bottom ones
 * take the rest, conformance_window_flag is 0 when all four are 0. Every syntax element after the window is copied
 * bit for bit, the RBSP trailing bits are redone and emulation prevention is re-inserted; *@fixed (g_free) and
 * *@fixed_size are set only for RDS_ENC_SPS_FIXED. */
G_GNUC_INTERNAL RdsEncSpsResult rds_enc_h265_fix_sps (const guint8 * nal, gsize size, guint width, guint height,
    guint8 ** fixed, gsize * fixed_size);

/* rds_enc_h265_fix_sps on every SPS NAL unit of the Annex-B access unit @in. Returns @in with an added reference
 * when no SPS changes (none, already right or unparseable), else a new buffer with @in's timestamps, offsets, flags
 * and metas and every other byte unchanged. VA writes parameter sets only before IDRs, so the probe calls it only on
 * buffers without GST_BUFFER_FLAG_DELTA_UNIT. */
G_GNUC_INTERNAL GstBuffer *rds_enc_h265_fix_sps_buffer (GstBuffer * in, guint width, guint height);

/* Whether the Annex-B access unit @data (one picture) is intra coded, from its NAL unit types and its first slice
 * header: an IDR picture (H.264 nal_unit_type 5), an IRAP picture (HEVC types 16..23), or a first slice of type I or
 * SI (H.264 slice_type 2, 4, 7, 9; HEVC 2). An HEVC slice header has num_extra_slice_header_bits reserved bits, set
 * by the PPS: *@hevc_extra_bits is the value of the last PPS seen, updated by a PPS in @data before its slices are
 * read; keep it across access units (VA writes its one PPS only before IDRs). H.264 slices are nal_unit_type 1 and 5
 * only. An AU without a slice, or whose first slice can't be read (cut short, an H.264 data partition, an HEVC first
 * slice that is not the picture's first segment), is not intra. Only the first slice's header bytes are read, not the
 * picture data. */
G_GNUC_INTERNAL gboolean rds_enc_au_is_intra (const guint8 * data, gsize size, RdsEncCodec codec,
    guint * hevc_extra_bits);

G_END_DECLS

#endif /* RDS_ENC_NAL_H */
