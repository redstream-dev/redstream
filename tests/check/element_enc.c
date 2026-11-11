/* rdsh264enc and rdsh265enc: registration, pads, every property's name, type, range, default and flags as
 * gst-inspect shows them for DeepStream 8.0's nvv4l2h264enc / nvv4l2h265enc, the enum types, the accepted no-ops'
 * warning, malformed QP strings, the inner chain, a missing VA encoder, one that doesn't link and the unsupported
 * profiles (cpu; a stand-in element takes the VA encoder's factory name where the bin has to reach READY); a short
 * encode and properties set while PLAYING, per codec (gpu, each skipped without its VA encoder).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"

#include <string.h>

typedef struct
{
  const gchar *element;
  const gchar *va_factory;
  const gchar *caps_name;
} Codec;

static const Codec codecs[] = {
  {"rdsh264enc", "vah264enc", "video/x-h264"},
  {"rdsh265enc", "vah265enc", "video/x-h265"},
};

/* ---- the VA encoder's factory: the real one, a stand-in or none ---- */

typedef struct
{
  GstElement parent;
} RdsTestVaStandIn;
typedef struct
{
  GstElementClass parent_class;
} RdsTestVaStandInClass;

static GType rds_test_va_stand_in_get_type (void);
G_DEFINE_TYPE (RdsTestVaStandIn, rds_test_va_stand_in, GST_TYPE_ELEMENT);

static GstStaticPadTemplate stand_in_sink = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate stand_in_src = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static void
rds_test_va_stand_in_init (RdsTestVaStandIn * self)
{
  gst_element_add_pad (GST_ELEMENT (self), gst_pad_new_from_static_template (&stand_in_sink, "sink"));
  gst_element_add_pad (GST_ELEMENT (self), gst_pad_new_from_static_template (&stand_in_src, "src"));
}

static void
rds_test_va_stand_in_class_init (RdsTestVaStandInClass * klass)
{
  GstElementClass *ec = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (ec, &stand_in_sink);
  gst_element_class_add_static_pad_template (ec, &stand_in_src);
  gst_element_class_set_static_metadata (ec, "VA encoder stand-in", "Codec/Encoder/Video", "Does nothing",
      "Redstream contributors");
}

/* A stand-in that can't link to the bin's src capsfilter (audio out). */
typedef struct
{
  GstElement parent;
} RdsTestVaBadSrc;
typedef struct
{
  GstElementClass parent_class;
} RdsTestVaBadSrcClass;

static GType rds_test_va_bad_src_get_type (void);
G_DEFINE_TYPE (RdsTestVaBadSrc, rds_test_va_bad_src, GST_TYPE_ELEMENT);

static GstStaticPadTemplate bad_src = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw"));

static void
rds_test_va_bad_src_init (RdsTestVaBadSrc * self)
{
  gst_element_add_pad (GST_ELEMENT (self), gst_pad_new_from_static_template (&stand_in_sink, "sink"));
  gst_element_add_pad (GST_ELEMENT (self), gst_pad_new_from_static_template (&bad_src, "src"));
}

static void
rds_test_va_bad_src_class_init (RdsTestVaBadSrcClass * klass)
{
  GstElementClass *ec = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (ec, &stand_in_sink);
  gst_element_class_add_static_pad_template (ec, &bad_src);
  gst_element_class_set_static_metadata (ec, "VA encoder stand-in, audio out", "Codec/Encoder/Video",
      "Does nothing", "Redstream contributors");
}

typedef enum
{
  VA_REAL,
  VA_STAND_IN,
  VA_BAD_SRC,
  VA_NONE,
} VaMode;

static gboolean
is_stand_in (GstPluginFeature * f)
{
  GType t;

  if (G_TYPE_FROM_INSTANCE (f) != GST_TYPE_ELEMENT_FACTORY)
    return FALSE;
  t = gst_element_factory_get_element_type (GST_ELEMENT_FACTORY (f));
  return t == rds_test_va_stand_in_get_type () || t == rds_test_va_bad_src_get_type ();
}

static GstPluginFeature *real_va[G_N_ELEMENTS (codecs)];        /* the real factories, while removed */

/* Puts the factory named @c->va_factory in the registry as @mode says. */
static void
set_va (const Codec * c, VaMode mode)
{
  GstRegistry *reg = gst_registry_get ();
  guint i = (guint) (c - codecs);
  GstPluginFeature *f = gst_registry_lookup_feature (reg, c->va_factory);

  if (f != NULL) {
    if (is_stand_in (f)) {
      gst_registry_remove_feature (reg, f);
    } else if (mode != VA_REAL) {
      if (real_va[i] == NULL)
        real_va[i] = gst_object_ref (f);
      gst_registry_remove_feature (reg, f);
    }
    gst_object_unref (f);
  }
  if (mode == VA_STAND_IN)
    g_assert_true (gst_element_register (NULL, c->va_factory, GST_RANK_NONE, rds_test_va_stand_in_get_type ()));
  else if (mode == VA_BAD_SRC)
    g_assert_true (gst_element_register (NULL, c->va_factory, GST_RANK_NONE, rds_test_va_bad_src_get_type ()));
  else if (mode == VA_REAL && real_va[i] != NULL) {
    g_assert_true (gst_registry_add_feature (reg, real_va[i]));
    real_va[i] = NULL;
  }
}

/* ---- helpers ---- */

static GstMessage *
pop_error (GstBus * bus)
{
  return gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
}

/* The error message's text and debug string, joined. */
static gchar *
error_text (GstMessage * m)
{
  GError *err = NULL;
  gchar *dbg = NULL, *s;

  gst_message_parse_error (m, &err, &dbg);
  s = g_strdup_printf ("%s (%s)", err->message, dbg != NULL ? dbg : "");
  g_test_message ("error from %s: %s", GST_MESSAGE_SRC_NAME (m), s);
  g_clear_error (&err);
  g_free (dbg);
  return s;
}

/* ---- registration and pads ---- */

static void
test_registration (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    const Codec *c = &codecs[i];
    GstElementFactory *f = gst_element_factory_find (c->element);
    GstElement *e;
    GstPad *pad;
    GstCaps *caps;
    GstStructure *s;

    g_assert_nonnull (f);
    g_assert_cmpstr (gst_element_factory_get_metadata (f, GST_ELEMENT_METADATA_KLASS), ==,
        "Codec/Encoder/Video/Hardware");
    g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (f)), ==, GST_RANK_NONE);
    e = gst_element_factory_create (f, NULL);
    g_assert_nonnull (e);
    g_assert_true (GST_IS_BIN (e));

    /* ghost pads from the start, before any inner element exists */
    pad = gst_element_get_static_pad (e, "sink");
    g_assert_true (GST_IS_GHOST_PAD (pad));
    caps = gst_pad_get_pad_template_caps (pad);
    g_assert_cmpuint (gst_caps_get_size (caps), ==, 2);
    g_assert_true (gst_caps_features_contains (gst_caps_get_features (caps, 0), "memory:RDS"));
    g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (caps, 1),
            GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
    for (guint k = 0; k < 2; k++)
      g_assert_cmpstr (gst_structure_get_name (gst_caps_get_structure (caps, k)), ==, "video/x-raw");
    gst_caps_unref (caps);
    gst_object_unref (pad);

    pad = gst_element_get_static_pad (e, "src");
    g_assert_true (GST_IS_GHOST_PAD (pad));
    caps = gst_pad_get_pad_template_caps (pad);
    g_assert_cmpuint (gst_caps_get_size (caps), ==, 1);
    s = gst_caps_get_structure (caps, 0);
    g_assert_cmpstr (gst_structure_get_name (s), ==, c->caps_name);
    g_assert_cmpstr (gst_structure_get_string (s, "stream-format"), ==, "byte-stream");
    g_assert_cmpstr (gst_structure_get_string (s, "alignment"), ==, "au");
    gst_caps_unref (caps);
    gst_object_unref (pad);

    gst_object_unref (e);
    gst_object_unref (f);
  }
}

/* ---- properties ---- */

enum
{
  F_LIVE = 1,                   /* mutable in PLAYING */
  F_READY = 2,                  /* changeable only in NULL or READY */
  F_RO = 4,                     /* read-only */
  F_NO264 = 8,                  /* H.265 only */
  F_NO265 = 16,                 /* H.264 only */
};

typedef struct
{
  const gchar *name;
  const gchar *type;            /* the value type's name */
  const gchar *min, *max;       /* numbers: the range, as strings; NULL otherwise */
  const gchar *def;             /* g_strdup_value_contents of the default; enums: the value as a number */
  guint flags;
} Prop;

#define UMAX "4294967295"
#define U64MAX "18446744073709551615"

/* gst-inspect-nvv4l2h264enc.txt and gst-inspect-nvv4l2h265enc.txt (DeepStream 8.0), but for the flags: bitrate,
 * force-idr and force-intra are mutable in PLAYING, every other writable property only in NULL or READY (ours). */
static const Prop props[] = {
  {"SliceIntraRefreshInterval", "guint", "0", UMAX, "0", F_READY},
  {"aq", "guint", "0", "15", "0", F_READY},
  {"bit-packetization", "gboolean", NULL, NULL, "FALSE", F_READY},
  {"bitrate", "guint", "0", UMAX, "4000000", F_LIVE},
  {"capture-io-mode", "GstRdsEncIOMode", NULL, NULL, "0", F_READY},
  {"constqp", "gchararray", NULL, NULL, "NULL", F_READY},
  {"control-rate", "GstRdsEncRateControl", NULL, NULL, "1", F_READY},
  {"copy-meta", "gboolean", NULL, NULL, "FALSE", F_READY},
  {"cq", "guint", "0", "51", "0", F_READY},
  {"cudaenc-memtype", "GstRdsEncCudaMemType", NULL, NULL, "0", F_READY},
  {"device", "gchararray", NULL, NULL, "\"/dev/v4l2-nvenc\"", F_RO},
  {"device-fd", "gint", "-1", "2147483647", "-1", F_RO},
  {"device-name", "gchararray", NULL, NULL, "\"\"", F_RO},
  {"disable-cabac", "gboolean", NULL, NULL, "FALSE", F_READY | F_NO265},
  {"enable-lossless", "gboolean", NULL, NULL, "FALSE", F_READY},
  {"extended-colorformat", "gboolean", NULL, NULL, "FALSE", F_READY},
  {"extra-controls", "GstStructure", NULL, NULL, "NULL", F_READY},
  {"force-idr", "gboolean", NULL, NULL, "FALSE", F_LIVE},
  {"force-intra", "gboolean", NULL, NULL, "FALSE", F_LIVE},
  {"gpu-id", "guint", "0", UMAX, "0", F_READY},
  {"idrinterval", "guint", "0", UMAX, "256", F_READY},
  {"iframeinterval", "guint", "0", UMAX, UMAX, F_READY},
  {"initqp", "gchararray", NULL, NULL, "NULL", F_READY},
  {"insert-aud", "gboolean", NULL, NULL, "FALSE", F_READY},
  {"insert-sps-pps", "gboolean", NULL, NULL, "FALSE", F_READY},
  {"insert-vui", "gboolean", NULL, NULL, "TRUE", F_READY},
  {"intra-refresh", "gchararray", NULL, NULL, "\"1,30,1\"", F_READY},
  {"maxbitrate", "guint", "0", UMAX, "0", F_READY},
  {"min-force-key-unit-interval", "guint64", "0", U64MAX, "0", F_READY},
  {"num-B-Frames", "guint", "0", "2", "0", F_READY},
  {"num-Ref-Frames", "guint", "0", "8", "0", F_READY},
  {"output-io-mode", "GstRdsEncIOMode", NULL, NULL, "0", F_READY},
  {"preset-id", "guint", "1", "7", "1", F_READY},
  {"profile", "GstRdsH264EncProfile", NULL, NULL, "0", F_READY | F_NO265},
  {"profile", "GstRdsH265EncProfile", NULL, NULL, "0", F_READY | F_NO264},
  {"qos", "gboolean", NULL, NULL, "FALSE", F_READY},
  {"qp-range", "gchararray", NULL, NULL, "NULL", F_READY},
  {"sei-uuid", "gchararray", NULL, NULL, "\"NVDS_CUSTOMMETA\"", F_READY},
  {"slice-header-spacing", "guint64", "0", U64MAX, "0", F_READY},
  {"temporalaq", "gboolean", NULL, NULL, "FALSE", F_READY},
  {"tuning-info-id", "GstRdsEncTuningInfo", NULL, NULL, "2", F_READY},
  {"vbvbufsize", "guint", "0", UMAX, "0", F_READY},
  {"vbvinit", "guint", "0", UMAX, "0", F_READY},
};

static gboolean
prop_in (const Prop * p, guint codec)
{
  return !(p->flags & (codec == 0 ? F_NO264 : F_NO265));
}

static void
check_range (GParamSpec * ps, const Prop * p)
{
  gchar *min = NULL, *max = NULL;

  if (G_IS_PARAM_SPEC_UINT (ps)) {
    min = g_strdup_printf ("%u", G_PARAM_SPEC_UINT (ps)->minimum);
    max = g_strdup_printf ("%u", G_PARAM_SPEC_UINT (ps)->maximum);
  } else if (G_IS_PARAM_SPEC_INT (ps)) {
    min = g_strdup_printf ("%d", G_PARAM_SPEC_INT (ps)->minimum);
    max = g_strdup_printf ("%d", G_PARAM_SPEC_INT (ps)->maximum);
  } else if (G_IS_PARAM_SPEC_UINT64 (ps)) {
    min = g_strdup_printf ("%" G_GUINT64_FORMAT, G_PARAM_SPEC_UINT64 (ps)->minimum);
    max = g_strdup_printf ("%" G_GUINT64_FORMAT, G_PARAM_SPEC_UINT64 (ps)->maximum);
  }
  g_assert_cmpstr (min, ==, p->min);
  g_assert_cmpstr (max, ==, p->max);
  g_free (min);
  g_free (max);
}

static void
check_properties (guint codec)
{
  GstElement *e = gst_element_factory_make (codecs[codec].element, NULL);
  GObjectClass *klass = G_OBJECT_GET_CLASS (e);
  GParamSpec **all;
  guint n_all, n_ours = 0, n_expected = 0;

  g_assert_nonnull (e);
  for (guint i = 0; i < G_N_ELEMENTS (props); i++) {
    const Prop *p = &props[i];
    GParamSpec *ps;
    GValue v = G_VALUE_INIT;
    gchar *def;

    if (!prop_in (p, codec)) {
      if (strcmp (p->name, "profile") != 0)
        g_assert_null (g_object_class_find_property (klass, p->name));
      continue;
    }
    n_expected++;
    ps = g_object_class_find_property (klass, p->name);
    g_test_message ("%s: %s", codecs[codec].element, p->name);
    g_assert_nonnull (ps);
    g_assert_cmpstr (g_type_name (ps->value_type), ==, p->type);
    check_range (ps, p);

    /* the default, both from the spec and read back from a new element */
    g_value_init (&v, ps->value_type);
    g_object_get_property (G_OBJECT (e), p->name, &v);
    g_assert_true (g_param_value_defaults (ps, &v));
    if (G_IS_PARAM_SPEC_ENUM (ps))
      def = g_strdup_printf ("%d", g_value_get_enum (&v));
    else if (G_IS_PARAM_SPEC_BOXED (ps))
      def = g_strdup (g_value_get_boxed (&v) == NULL ? "NULL" : "set");
    else
      def = g_strdup_value_contents (&v);
    g_assert_cmpstr (def, ==, p->def);
    g_free (def);
    g_value_unset (&v);

    g_assert_true (ps->flags & G_PARAM_READABLE);
    if (p->flags & F_RO) {
      g_assert_false (ps->flags & G_PARAM_WRITABLE);
    } else {
      g_assert_true (ps->flags & G_PARAM_WRITABLE);
      g_assert_cmpint (! !(ps->flags & GST_PARAM_MUTABLE_PLAYING), ==, ! !(p->flags & F_LIVE));
      g_assert_cmpint (! !(ps->flags & GST_PARAM_MUTABLE_READY), ==, ! !(p->flags & F_READY));
    }
    g_assert_cmpstr (g_param_spec_get_blurb (ps), !=, NULL);
  }
  g_assert_cmpuint (n_expected, ==, codec == 0 ? 42 : 41);

  /* and nothing else, but GstObject's and GstBin's own */
  all = g_object_class_list_properties (klass, &n_all);
  for (guint i = 0; i < n_all; i++) {
    if (all[i]->owner_type != GST_TYPE_OBJECT && all[i]->owner_type != GST_TYPE_ELEMENT
        && all[i]->owner_type != GST_TYPE_BIN)
      n_ours++;
  }
  g_assert_cmpuint (n_ours, ==, n_expected);
  g_free (all);
  gst_object_unref (e);
}

static void
test_properties_h264 (void)
{
  check_properties (0);
}

static void
test_properties_h265 (void)
{
  check_properties (1);
}

/* ---- enums ---- */

typedef struct
{
  gint value;
  const gchar *nick;
} EnumValue;

static void
check_enum (const gchar * element, const gchar * property, const gchar * type, const EnumValue * values, guint n)
{
  GstElement *e = gst_element_factory_make (element, NULL);
  GParamSpec *ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), property);
  GEnumClass *ec;

  g_assert_nonnull (ps);
  g_assert_true (G_IS_PARAM_SPEC_ENUM (ps));
  g_assert_cmpstr (g_type_name (ps->value_type), ==, type);
  ec = G_PARAM_SPEC_ENUM (ps)->enum_class;
  g_assert_cmpuint (ec->n_values, ==, n);
  for (guint i = 0; i < n; i++) {
    GEnumValue *ev = g_enum_get_value (ec, values[i].value);

    g_assert_nonnull (ev);
    g_assert_cmpstr (ev->value_nick, ==, values[i].nick);
  }
  gst_object_unref (e);
}

static void
test_enums (void)
{
  static const EnumValue rate[] = { {0, "variable_bitrate"}, {1, "constant_bitrate"}, {2, "constantQP"} };
  static const EnumValue io[] = { {0, "auto"}, {2, "mmap"}, {5, "dmabuf-import"} };
  static const EnumValue mem[] = { {0, "memtype_device"}, {1, "memtype_pinned"}, {2, "memtype_unified"} };
  static const EnumValue tuning[] = { {1, "HighQualityPreset"}, {2, "LowLatencyPreset"},
  {3, "UltraLowLatencyPreset"}, {4, "LosslessPreset"}
  };
  static const EnumValue p264[] = { {0, "Baseline"}, {1, "Constrained-Baseline"}, {2, "Main"}, {4, "High"},
  {17, "Constrained-High"}, {7, "High444"}
  };
  static const EnumValue p265[] = { {0, "Main"}, {1, "Main10"}, {3, "FREXT"} };

  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    const gchar *el = codecs[i].element;

    check_enum (el, "control-rate", "GstRdsEncRateControl", rate, G_N_ELEMENTS (rate));
    check_enum (el, "capture-io-mode", "GstRdsEncIOMode", io, G_N_ELEMENTS (io));
    check_enum (el, "output-io-mode", "GstRdsEncIOMode", io, G_N_ELEMENTS (io));
    check_enum (el, "cudaenc-memtype", "GstRdsEncCudaMemType", mem, G_N_ELEMENTS (mem));
    check_enum (el, "tuning-info-id", "GstRdsEncTuningInfo", tuning, G_N_ELEMENTS (tuning));
  }
  check_enum ("rdsh264enc", "profile", "GstRdsH264EncProfile", p264, G_N_ELEMENTS (p264));
  check_enum ("rdsh265enc", "profile", "GstRdsH265EncProfile", p265, G_N_ELEMENTS (p265));

  /* nicks work from a launch line */
  {
    GstElement *e = gst_parse_launch ("rdsh264enc profile=High control-rate=constantQP", NULL);
    gint profile, rc;

    g_assert_nonnull (e);
    g_object_get (e, "profile", &profile, "control-rate", &rc, NULL);
    g_assert_cmpint (profile, ==, 4);
    g_assert_cmpint (rc, ==, 2);
    gst_object_unref (e);
  }
}

/* ---- stored values, no-ops, QP strings ---- */

/* The mapped properties: stored and read back, no warning. */
static void
test_stored (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    GstElement *e = gst_element_factory_make (codecs[i].element, NULL);
    guint bitrate, maxbitrate, vbv, idr, iframe, b, refs, gpu;
    guint64 fku;
    gint rc, profile;
    gboolean aud, qos, copy, force_idr, force_intra;

    g_object_set (e, "bitrate", 1000000u, "control-rate", 0, "maxbitrate", 8000000u, "vbvbufsize", 500000u,
        "idrinterval", 60u, "iframeinterval", 30u, "num-B-Frames", 2u, "num-Ref-Frames", 4u, "gpu-id", 1u,
        "min-force-key-unit-interval", (guint64) GST_SECOND, "insert-aud", TRUE, "qos", TRUE, "copy-meta", TRUE,
        "profile", i == 0 ? 2 : 0, NULL);
    if (i == 0)
      g_object_set (e, "disable-cabac", TRUE, NULL);
    g_object_get (e, "bitrate", &bitrate, "control-rate", &rc, "maxbitrate", &maxbitrate, "vbvbufsize", &vbv,
        "idrinterval", &idr, "iframeinterval", &iframe, "num-B-Frames", &b, "num-Ref-Frames", &refs, "gpu-id", &gpu,
        "min-force-key-unit-interval", &fku, "insert-aud", &aud, "qos", &qos, "copy-meta", &copy, "profile",
        &profile, NULL);
    g_assert_cmpuint (bitrate, ==, 1000000);
    g_assert_cmpint (rc, ==, 0);
    g_assert_cmpuint (maxbitrate, ==, 8000000);
    g_assert_cmpuint (vbv, ==, 500000);
    g_assert_cmpuint (idr, ==, 60);
    g_assert_cmpuint (iframe, ==, 30);
    g_assert_cmpuint (b, ==, 2);
    g_assert_cmpuint (refs, ==, 4);
    g_assert_cmpuint (gpu, ==, 1);
    g_assert_cmpuint (fku, ==, GST_SECOND);
    g_assert_true (aud);
    g_assert_true (qos);
    g_assert_true (copy);
    g_assert_cmpint (profile, ==, i == 0 ? 2 : 0);
    if (i == 0) {
      gboolean cabac_off;

      g_object_get (e, "disable-cabac", &cabac_off, NULL);
      g_assert_true (cabac_off);
    }

    /* force-idr and force-intra are requests: they read back FALSE */
    g_object_set (e, "force-idr", TRUE, "force-intra", TRUE, NULL);
    g_object_get (e, "force-idr", &force_idr, "force-intra", &force_intra, NULL);
    g_assert_false (force_idr);
    g_assert_false (force_intra);
    gst_object_unref (e);
  }
}

typedef struct
{
  const gchar *name;
  const gchar *value;           /* a non-default value, parsed with gst_value_deserialize */
} Noop;

static const Noop noops[] = {
  {"SliceIntraRefreshInterval", "5"},
  {"aq", "3"},
  {"bit-packetization", "true"},
  {"capture-io-mode", "mmap"},
  {"cq", "20"},
  {"cudaenc-memtype", "memtype_unified"},
  {"enable-lossless", "true"},
  {"extended-colorformat", "true"},
  {"extra-controls", "controls, a=(int)1"},
  {"initqp", "20:22:24"},
  {"insert-sps-pps", "true"},
  {"insert-vui", "false"},
  {"intra-refresh", "0,0,0"},
  {"output-io-mode", "dmabuf-import"},
  {"preset-id", "4"},
  {"sei-uuid", "0123456789abcdef"},
  {"slice-header-spacing", "8"},
  {"temporalaq", "true"},
  {"tuning-info-id", "HighQualityPreset"},
  {"vbvinit", "1000"},
};

static void
set_from_string (GstElement * e, const gchar * name, const gchar * str)
{
  GParamSpec *ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), name);
  GValue v = G_VALUE_INIT;

  g_assert_nonnull (ps);
  g_value_init (&v, ps->value_type);
  g_assert_true (gst_value_deserialize (&v, str));
  g_object_set_property (G_OBJECT (e), name, &v);
  g_value_unset (&v);
}

static void
test_noops_warn_once (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    GstElement *e = gst_element_factory_make (codecs[i].element, NULL);

    /* defaults: silent */
    for (guint k = 0; k < G_N_ELEMENTS (noops); k++) {
      GParamSpec *ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), noops[k].name);
      GValue v = G_VALUE_INIT;

      g_value_init (&v, ps->value_type);
      g_param_value_set_default (ps, &v);
      g_object_set_property (G_OBJECT (e), noops[k].name, &v);
      g_value_unset (&v);
    }

    /* one warning per property, the first time it gets a value that is not the default */
    for (guint k = 0; k < G_N_ELEMENTS (noops); k++) {
      gchar *pattern = g_strdup_printf ("*%s*no effect*", noops[k].name);

      g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, pattern);
      set_from_string (e, noops[k].name, noops[k].value);
      g_test_assert_expected_messages ();
      g_free (pattern);
    }
    for (guint k = 0; k < G_N_ELEMENTS (noops); k++)
      set_from_string (e, noops[k].name, noops[k].value);

    /* stored */
    for (guint k = 0; k < G_N_ELEMENTS (noops); k++) {
      GParamSpec *ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), noops[k].name);
      GValue v = G_VALUE_INIT;

      g_value_init (&v, ps->value_type);
      g_object_get_property (G_OBJECT (e), noops[k].name, &v);
      g_assert_false (g_param_value_defaults (ps, &v));
      g_value_unset (&v);
    }
    gst_object_unref (e);
  }
}

static void
check_qp_string (GstElement * e, const gchar * name, const gchar * good, const gchar * const *bad)
{
  gchar *s;

  g_object_set (e, name, good, NULL);
  g_object_get (e, name, &s, NULL);
  g_assert_cmpstr (s, ==, good);
  g_free (s);
  for (guint k = 0; bad[k] != NULL; k++) {
    gchar *pattern = g_strdup_printf ("*%s*%s*", name, bad[k]);

    g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, pattern);
    g_object_set (e, name, bad[k], NULL);
    g_test_assert_expected_messages ();
    g_object_get (e, name, &s, NULL);
    g_assert_cmpstr (s, ==, good);
    g_free (s);
    g_free (pattern);
  }
  /* NULL unsets it */
  g_object_set (e, name, NULL, NULL);
  g_object_get (e, name, &s, NULL);
  g_assert_null (s);
}

static void
test_qp_strings (void)
{
  static const gchar *const bad_triplet[] = { "", "20:22", "20:22:24:26", "a:b:c", "52:22:24", "-1:22:24",
    " 20:22:24", "20,22,24", NULL
  };
  static const gchar *const bad_range[] = { "", "10,40:10,40", "10,40:10,40:10,40:1,2", "40,10:10,40:10,40",
    "10,52:10,40:10,40", "10,40;10,40;10,40", "x", NULL
  };

  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    GstElement *e = gst_element_factory_make (codecs[i].element, NULL);

    check_qp_string (e, "constqp", "20:22:24", bad_triplet);
    check_qp_string (e, "qp-range", "10,40:12,38:14,42", bad_range);
    /* initqp is a no-op: its first value warns once as such, then malformed strings as for constqp */
    g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*initqp*no effect*");
    check_qp_string (e, "initqp", "20:22:24", bad_triplet);
    gst_object_unref (e);
  }
}

/* ---- start: inner chain, missing VA encoder, profiles ---- */

static void
test_inner_chain (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    const Codec *c = &codecs[i];
    GstElement *e = gst_element_factory_make (c->element, NULL);
    GstElement *conv, *raw, *enc, *out;
    GstPad *ghost, *target;
    GstCaps *caps;
    gchar *s;

    set_va (c, VA_STAND_IN);
    g_assert_cmpuint (GST_BIN_NUMCHILDREN (e), ==, 0);
    g_assert_cmpint (gst_element_set_state (e, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
    g_assert_cmpuint (GST_BIN_NUMCHILDREN (e), ==, 4);
    conv = gst_bin_get_by_name (GST_BIN (e), "convert");
    raw = gst_bin_get_by_name (GST_BIN (e), "rawcaps");
    enc = gst_bin_get_by_name (GST_BIN (e), "encoder");
    out = gst_bin_get_by_name (GST_BIN (e), "srccaps");
    g_assert_cmpstr (GST_OBJECT_NAME (gst_element_get_factory (conv)), ==, "rdsvideoconvert");
    g_assert_cmpstr (GST_OBJECT_NAME (gst_element_get_factory (raw)), ==, "capsfilter");
    g_assert_cmpstr (GST_OBJECT_NAME (gst_element_get_factory (enc)), ==, c->va_factory);
    g_assert_cmpstr (GST_OBJECT_NAME (gst_element_get_factory (out)), ==, "capsfilter");

    g_object_get (raw, "caps", &caps, NULL);
    s = gst_caps_to_string (caps);
    g_assert_cmpstr (s, ==, "video/x-raw, format=(string)NV12");
    g_free (s);
    gst_caps_unref (caps);
    g_object_get (out, "caps", &caps, NULL);
    g_assert_true (gst_caps_is_fixed (caps));
    g_assert_cmpstr (gst_structure_get_name (gst_caps_get_structure (caps, 0)), ==, c->caps_name);
    g_assert_cmpstr (gst_structure_get_string (gst_caps_get_structure (caps, 0), "stream-format"), ==,
        "byte-stream");
    g_assert_cmpstr (gst_structure_get_string (gst_caps_get_structure (caps, 0), "alignment"), ==, "au");
    gst_caps_unref (caps);

    /* the ghost pads point at the chain's ends */
    ghost = gst_element_get_static_pad (e, "sink");
    target = gst_ghost_pad_get_target (GST_GHOST_PAD (ghost));
    g_assert_true (GST_PAD_PARENT (target) == conv);
    gst_object_unref (target);
    gst_object_unref (ghost);
    ghost = gst_element_get_static_pad (e, "src");
    target = gst_ghost_pad_get_target (GST_GHOST_PAD (ghost));
    g_assert_true (GST_PAD_PARENT (target) == out);
    gst_object_unref (target);
    gst_object_unref (ghost);

    /* in READY a stored property is just stored: no warning */
    g_object_set (e, "idrinterval", 30u, "bitrate", 1000000u, NULL);

    /* kept across a restart */
    g_assert_cmpint (gst_element_set_state (e, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
    g_assert_cmpint (gst_element_set_state (e, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
    g_assert_cmpuint (GST_BIN_NUMCHILDREN (e), ==, 4);
    gst_element_set_state (e, GST_STATE_NULL);

    gst_object_unref (conv);
    gst_object_unref (raw);
    gst_object_unref (enc);
    gst_object_unref (out);
    gst_object_unref (e);
    set_va (c, VA_REAL);
  }
}

static void
test_missing_va (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    const Codec *c = &codecs[i];
    GstElement *p = gst_pipeline_new (NULL), *e = gst_element_factory_make (c->element, NULL);
    GstBus *bus = gst_element_get_bus (p);
    GstMessage *m;
    gchar *text;

    set_va (c, VA_NONE);
    gst_bin_add (GST_BIN (p), e);
    g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_FAILURE);
    m = pop_error (bus);
    g_assert_nonnull (m);
    g_assert_true (GST_MESSAGE_SRC (m) == GST_OBJECT (e));
    text = error_text (m);
    g_assert_nonnull (strstr (text, c->va_factory));
    g_free (text);
    gst_message_unref (m);
    g_assert_null (pop_error (bus));
    g_assert_cmpuint (GST_BIN_NUMCHILDREN (e), ==, 0);

    /* with the factory back, the same element starts */
    set_va (c, VA_STAND_IN);
    g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
    gst_element_set_state (p, GST_STATE_NULL);
    gst_object_unref (bus);
    gst_object_unref (p);
    set_va (c, VA_REAL);
  }
}

/* An encoder that doesn't link: an error, and nothing left in the bin. */
static void
test_link_failure (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    const Codec *c = &codecs[i];
    GstElement *p = gst_pipeline_new (NULL), *e = gst_element_factory_make (c->element, NULL);
    GstBus *bus = gst_element_get_bus (p);
    GstMessage *m;
    GstPad *ghost, *target;
    gchar *text;

    set_va (c, VA_BAD_SRC);
    gst_bin_add (GST_BIN (p), e);
    g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_FAILURE);
    m = pop_error (bus);
    g_assert_nonnull (m);
    g_assert_true (GST_MESSAGE_SRC (m) == GST_OBJECT (e));
    text = error_text (m);
    g_assert_nonnull (strstr (text, "link"));
    g_free (text);
    gst_message_unref (m);
    g_assert_cmpuint (GST_BIN_NUMCHILDREN (e), ==, 0);
    for (guint k = 0; k < 2; k++) {
      ghost = gst_element_get_static_pad (e, k == 0 ? "sink" : "src");
      target = gst_ghost_pad_get_target (GST_GHOST_PAD (ghost));
      g_assert_null (target);
      gst_object_unref (ghost);
    }
    gst_element_set_state (p, GST_STATE_NULL);
    gst_object_unref (bus);
    gst_object_unref (p);
    set_va (c, VA_REAL);
  }
}

static void
check_profile_fails (guint codec, gint profile, const gchar * name)
{
  const Codec *c = &codecs[codec];
  GstElement *p = gst_pipeline_new (NULL), *e = gst_element_factory_make (c->element, NULL);
  GstBus *bus = gst_element_get_bus (p);
  GstMessage *m;
  gchar *text;

  set_va (c, VA_STAND_IN);
  g_object_set (e, "profile", profile, NULL);
  gst_bin_add (GST_BIN (p), e);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_FAILURE);
  m = pop_error (bus);
  g_assert_nonnull (m);
  g_assert_true (GST_MESSAGE_SRC (m) == GST_OBJECT (e));
  text = error_text (m);
  g_assert_nonnull (strstr (text, name));
  g_free (text);
  gst_message_unref (m);
  g_assert_null (pop_error (bus));

  /* with a supported profile the same element starts (its rdsvideoconvert needs a GPU for that) */
  g_object_set (e, "profile", 0, NULL);
  if (rds_test_have_gpu ())
    g_assert_cmpint (gst_element_set_state (p, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_SUCCESS);
  else
    g_test_message ("no ROCm device: the start with a supported profile is not checked");
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (p);
  set_va (c, VA_REAL);
}

static void
test_unsupported_profile (void)
{
  check_profile_fails (0, 7, "High444");
  check_profile_fails (1, 1, "Main10");
  check_profile_fails (1, 3, "FREXT");
}

/* ---- gpu ---- */

#define SMOKE_FRAMES 30

static void
on_handoff (G_GNUC_UNUSED GstElement * sink, G_GNUC_UNUSED GstBuffer * buf, G_GNUC_UNUSED GstPad * pad,
    gpointer user_data)
{
  g_atomic_int_inc ((gint *) user_data);
}

/* Runs @p to EOS; fails the test on an error or a timeout. */
static void
run_to_eos (GstElement * p)
{
  GstBus *bus = gst_element_get_bus (p);
  GstMessage *m;

  g_assert_cmpint (gst_element_set_state (p, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  m = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (m);
  if (GST_MESSAGE_TYPE (m) == GST_MESSAGE_ERROR)
    g_error ("%s", error_text (m));
  gst_message_unref (m);
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (bus);
}

static gboolean
require_va (const Codec * c)
{
  if (!rds_test_have_gpu () || !rds_test_have_element (c->va_factory)) {
    g_test_skip ("no ROCm device or no VA encoder");
    return FALSE;
  }
  return TRUE;
}

static void
test_smoke (gconstpointer data)
{
  const Codec *c = data;
  gchar *desc;
  GstElement *p, *sink;
  gint n = 0;

  if (!require_va (c))
    return;
  desc = g_strdup_printf ("videotestsrc num-buffers=%d ! video/x-raw,width=320,height=240,framerate=30/1 ! %s ! "
      "%s ! fakesink name=sink signal-handoffs=true", SMOKE_FRAMES, c->element, c->caps_name);
  p = gst_parse_launch (desc, NULL);
  g_assert_nonnull (p);
  sink = gst_bin_get_by_name (GST_BIN (p), "sink");
  g_signal_connect (sink, "handoff", G_CALLBACK (on_handoff), &n);
  run_to_eos (p);
  g_assert_cmpint (g_atomic_int_get (&n), ==, SMOKE_FRAMES);
  gst_object_unref (sink);
  gst_object_unref (p);
  g_free (desc);
}

/* A stored property set in PLAYING warns (applied at the next start); the live ones and no-ops don't add that. */
static void
test_set_while_playing (gconstpointer data)
{
  const Codec *c = data;
  gchar *desc;
  GstElement *p, *e;
  guint idr;

  if (!require_va (c))
    return;
  desc = g_strdup_printf ("videotestsrc is-live=true ! video/x-raw,width=320,height=240,framerate=30/1 ! "
      "%s name=enc ! fakesink", c->element);
  p = gst_parse_launch (desc, NULL);
  g_assert_nonnull (p);
  e = gst_bin_get_by_name (GST_BIN (p), "enc");
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_element_get_state (p, NULL, NULL, 10 * GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);

  g_object_set (e, "bitrate", 2000000u, "force-idr", TRUE, "force-intra", TRUE, NULL);
  g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*idrinterval*next start*");
  g_object_set (e, "idrinterval", 30u, NULL);
  g_test_assert_expected_messages ();
  g_object_get (e, "idrinterval", &idr, NULL);
  g_assert_cmpuint (idr, ==, 30);
  g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*aq*no effect*");
  g_object_set (e, "aq", 2u, NULL);
  g_test_assert_expected_messages ();

  gst_element_set_state (p, GST_STATE_NULL);
  /* stopped: no warning */
  g_object_set (e, "idrinterval", 60u, NULL);
  gst_object_unref (e);
  gst_object_unref (p);
  g_free (desc);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/enc/registration", test_registration);
  g_test_add_func ("/cpu/enc/properties-h264", test_properties_h264);
  g_test_add_func ("/cpu/enc/properties-h265", test_properties_h265);
  g_test_add_func ("/cpu/enc/enums", test_enums);
  g_test_add_func ("/cpu/enc/stored", test_stored);
  g_test_add_func ("/cpu/enc/noops-warn-once", test_noops_warn_once);
  g_test_add_func ("/cpu/enc/qp-strings", test_qp_strings);
  g_test_add_func ("/cpu/enc/inner-chain", test_inner_chain);
  g_test_add_func ("/cpu/enc/missing-va", test_missing_va);
  g_test_add_func ("/cpu/enc/link-failure", test_link_failure);
  g_test_add_func ("/cpu/enc/unsupported-profile", test_unsupported_profile);
  g_test_add_data_func ("/gpu/enc/smoke-h264", &codecs[0], test_smoke);
  g_test_add_data_func ("/gpu/enc/smoke-h265", &codecs[1], test_smoke);
  g_test_add_data_func ("/gpu/enc/set-while-playing-h264", &codecs[0], test_set_while_playing);
  g_test_add_data_func ("/gpu/enc/set-while-playing-h265", &codecs[1], test_set_while_playing);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
