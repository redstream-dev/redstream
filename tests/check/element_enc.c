/* rdsh264enc and rdsh265enc: registration, pads, every property's name, type, range, default and flags as
 * gst-inspect shows them for DeepStream 8.0's nvv4l2h264enc / nvv4l2h265enc, the enum types, the accepted no-ops'
 * warning, malformed QP strings, the inner chain, a missing VA encoder, one that doesn't link, the unsupported
 * profiles, what a start sets on the VA encoder, and the output probes on a stand-in's pushes: a buffer of filler
 * only dropped, a segment sent again (cpu; a stand-in element takes the VA encoder's factory name where the bin has
 * to reach READY); the preferred-profile caps answer of a started bin, with the stand-in (gpu: the start needs
 * rdsvideoconvert's GPU); per codec on the real VA encoders: a short encode,
 * properties set while PLAYING, GOP layouts and parameter sets, profiles and levels in the SPS and the caps, a
 * downstream level, CBR rates, a live bitrate change, B frames and Baseline, preset-id and gpu-id, and what the bin's
 * pad probes do: no filler, the H.265 size, input timestamps (also after a seek and with B frames), copy-meta,
 * memory:RDS and system-memory input, batch refusal, DELTA_UNIT, a frame-rate change (gpu, each skipped without its
 * VA encoder).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"
#include "rds-test-nal.h"

#include <rds/rds.h>
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

/* A stand-in with the VA encoders' properties (names, ranges and defaults as gst-inspect shows them for vah264enc),
 * to check what the bin sets at start. */
typedef struct
{
  GstElement parent;
  GValue values[32];
} RdsTestVaProps;
typedef struct
{
  GstElementClass parent_class;
} RdsTestVaPropsClass;

static GType rds_test_va_props_get_type (void);
G_DEFINE_TYPE (RdsTestVaProps, rds_test_va_props, GST_TYPE_ELEMENT);

static GType
rds_test_va_rc_get_type (void)
{
  static gsize id = 0;
  static const GEnumValue values[] = {
    {2, "Constant Bitrate", "cbr"},
    {4, "Variable Bitrate", "vbr"},
    {16, "Constant Quantizer", "cqp"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&id))
    g_once_init_leave (&id, g_enum_register_static ("RdsTestVaRateControl", values));
  return (GType) id;
}

typedef struct
{
  const gchar *name;
  guint min, max, def;
} VaUintProp;

static const VaUintProp va_uint_props[] = {
  {"bitrate", 0, 2048000, 0},
  {"target-percentage", 50, 100, 66},
  {"qpi", 0, 51, 26},
  {"qpp", 0, 51, 26},
  {"qpb", 0, 51, 26},
  {"min-qp", 0, 51, 1},
  {"max-qp", 0, 51, 51},
  {"cpb-size", 0, 2048000, 0},
  {"key-int-max", 0, 1024, 0},
  {"i-frames", 0, 1023, 0},
  {"target-usage", 1, 7, 4},
  {"b-frames", 0, 31, 0},
  {"ref-frames", 0, 16, 3},
};

static void
rds_test_va_props_set (GObject * o, guint id, const GValue * v, G_GNUC_UNUSED GParamSpec * ps)
{
  g_value_copy (v, &((RdsTestVaProps *) o)->values[id]);
}

static void
rds_test_va_props_get (GObject * o, guint id, GValue * v, G_GNUC_UNUSED GParamSpec * ps)
{
  g_value_copy (&((RdsTestVaProps *) o)->values[id], v);
}

static void
rds_test_va_props_finalize (GObject * o)
{
  RdsTestVaProps *self = (RdsTestVaProps *) o;

  for (guint i = 0; i < G_N_ELEMENTS (self->values); i++)
    if (G_IS_VALUE (&self->values[i]))
      g_value_unset (&self->values[i]);
  G_OBJECT_CLASS (rds_test_va_props_parent_class)->finalize (o);
}

static void
rds_test_va_props_init (RdsTestVaProps * self)
{
  GParamSpec **specs;
  guint n;

  gst_element_add_pad (GST_ELEMENT (self), gst_pad_new_from_static_template (&stand_in_sink, "sink"));
  gst_element_add_pad (GST_ELEMENT (self), gst_pad_new_from_static_template (&stand_in_src, "src"));
  specs = g_object_class_list_properties (G_OBJECT_GET_CLASS (self), &n);
  for (guint i = 0; i < n; i++) {
    if (specs[i]->owner_type != rds_test_va_props_get_type ())
      continue;
    g_value_init (&self->values[specs[i]->param_id], specs[i]->value_type);
    g_param_value_set_default (specs[i], &self->values[specs[i]->param_id]);
  }
  g_free (specs);
}

static void
rds_test_va_props_class_init (RdsTestVaPropsClass * klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  GstElementClass *ec = GST_ELEMENT_CLASS (klass);
  const GParamFlags f = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;
  guint id = 1;

  oc->set_property = rds_test_va_props_set;
  oc->get_property = rds_test_va_props_get;
  oc->finalize = rds_test_va_props_finalize;
  for (guint i = 0; i < G_N_ELEMENTS (va_uint_props); i++) {
    const VaUintProp *p = &va_uint_props[i];

    g_object_class_install_property (oc, id++, g_param_spec_uint (p->name, p->name, p->name, p->min, p->max,
            p->def, f));
  }
  g_object_class_install_property (oc, id++, g_param_spec_enum ("rate-control", "rc", "rc",
          rds_test_va_rc_get_type (), 2, f));
  g_object_class_install_property (oc, id++, g_param_spec_boolean ("cabac", "cabac", "cabac", TRUE, f));
  g_object_class_install_property (oc, id++, g_param_spec_boolean ("aud", "aud", "aud", FALSE, f));
  g_object_class_install_property (oc, id++, g_param_spec_boolean ("qos", "qos", "qos", FALSE, f));
  g_object_class_install_property (oc, id++, g_param_spec_uint64 ("min-force-key-unit-interval", "mfku", "mfku",
          0, G_MAXUINT64, 0, f));
  g_assert_cmpuint (id, <=, 32);

  gst_element_class_add_static_pad_template (ec, &stand_in_sink);
  gst_element_class_add_static_pad_template (ec, &stand_in_src);
  gst_element_class_set_static_metadata (ec, "VA encoder stand-in with properties", "Codec/Encoder/Video",
      "Does nothing", "Redstream contributors");
}

typedef enum
{
  VA_REAL,
  VA_STAND_IN,
  VA_BAD_SRC,
  VA_PROPS,
  VA_NONE,
} VaMode;

static gboolean
is_stand_in (GstPluginFeature * f)
{
  GType t;

  if (G_TYPE_FROM_INSTANCE (f) != GST_TYPE_ELEMENT_FACTORY)
    return FALSE;
  t = gst_element_factory_get_element_type (GST_ELEMENT_FACTORY (f));
  return g_type_is_a (t, rds_test_va_stand_in_get_type ()) || g_type_is_a (t, rds_test_va_bad_src_get_type ())
      || g_type_is_a (t, rds_test_va_props_get_type ());
}

/* The stand-in of @mode for codec @i: a subtype per codec. An element class points at the factory it was first
 * registered with, without a reference, so one type must not serve two factory names, and a removed stand-in
 * factory is kept alive (removed_stand_ins). */
static GType
stand_in_type (VaMode mode, guint i)
{
  static GType types[3][G_N_ELEMENTS (codecs)];
  static const gchar *names[3] = { "RdsTestVaStandIn", "RdsTestVaBadSrc", "RdsTestVaProps" };
  guint k = mode == VA_STAND_IN ? 0 : mode == VA_BAD_SRC ? 1 : 2;

  g_assert_true (mode == VA_STAND_IN || mode == VA_BAD_SRC || mode == VA_PROPS);
  if (types[k][i] == 0) {
    GType parent = k == 0 ? rds_test_va_stand_in_get_type () : k == 1 ? rds_test_va_bad_src_get_type ()
        : rds_test_va_props_get_type ();
    GTypeQuery q;
    gchar *name = g_strdup_printf ("%s_%s", names[k], codecs[i].va_factory);

    g_type_query (parent, &q);
    types[k][i] = g_type_register_static_simple (parent, name, q.class_size, NULL, q.instance_size, NULL, 0);
    g_free (name);
  }
  return types[k][i];
}

static GstPluginFeature *real_va[G_N_ELEMENTS (codecs)];        /* the real factories, while removed */
static GPtrArray *removed_stand_ins;    /* never freed: element classes point at them */

/* Puts the factory named @c->va_factory in the registry as @mode says. */
static void
set_va (const Codec * c, VaMode mode)
{
  GstRegistry *reg = gst_registry_get ();
  guint i = (guint) (c - codecs);
  GstPluginFeature *f = gst_registry_lookup_feature (reg, c->va_factory);

  if (f != NULL) {
    if (is_stand_in (f)) {
      if (removed_stand_ins == NULL)
        removed_stand_ins = g_ptr_array_new ();
      g_ptr_array_add (removed_stand_ins, gst_object_ref (f));
      gst_registry_remove_feature (reg, f);
    } else if (mode != VA_REAL) {
      if (real_va[i] == NULL)
        real_va[i] = gst_object_ref (f);
      gst_registry_remove_feature (reg, f);
    }
    gst_object_unref (f);
  }
  if (mode == VA_STAND_IN || mode == VA_BAD_SRC || mode == VA_PROPS)
    g_assert_true (gst_element_register (NULL, c->va_factory, GST_RANK_NONE, stand_in_type (mode, i)));
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

/* ---- what the bin sets on the VA encoder (the stand-in with VA's properties) ---- */

static guint
get_uint (GstElement * e, const gchar * name)
{
  guint v;

  g_object_get (e, name, &v, NULL);
  return v;
}

static gboolean
get_bool (GstElement * e, const gchar * name)
{
  gboolean v;

  g_object_get (e, name, &v, NULL);
  return v;
}

static guint64
get_uint64 (GstElement * e, const gchar * name)
{
  guint64 v;

  g_object_get (e, name, &v, NULL);
  return v;
}

static gchar *
get_enum_nick (GstElement * e, const gchar * name)
{
  GValue v = G_VALUE_INIT;
  GParamSpec *ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), name);
  GEnumValue *ev;

  g_value_init (&v, ps->value_type);
  g_object_get_property (G_OBJECT (e), name, &v);
  ev = g_enum_get_value (g_type_class_peek (ps->value_type), g_value_get_enum (&v));
  g_value_unset (&v);
  return g_strdup (ev->value_nick);
}

/* Starts @p (READY to PAUSED). Without a ROCm device rdsvideoconvert can't start, so the start fails after the bin
 * has configured its encoder; either way the VA properties are set. */
static void
start_any (GstElement * p)
{
  GstStateChangeReturn r = gst_element_set_state (p, GST_STATE_PAUSED);
  GstBus *bus = gst_element_get_bus (p);

  if (r == GST_STATE_CHANGE_FAILURE)
    g_test_message ("the start failed (no ROCm device?): only the configuration is checked");
  gst_bus_set_flushing (bus, TRUE);
  gst_bus_set_flushing (bus, FALSE);
  gst_object_unref (bus);
}

static void
test_configure (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    const Codec *c = &codecs[i];
    GstElement *p = gst_pipeline_new (NULL), *e = gst_element_factory_make (c->element, NULL), *va, *conv;
    gchar *rc;

    set_va (c, VA_PROPS);
    gst_bin_add (GST_BIN (p), e);
    g_object_set (e, "bitrate", 2500400u, "control-rate", 0, "maxbitrate", 4000000u, "constqp", "20:22:24",
        "qp-range", "10,40:5,30:1,51", "vbvbufsize", 3000000u, "iframeinterval", 100u, "num-B-Frames", 2u,
        "insert-aud", TRUE, "min-force-key-unit-interval", (guint64) 5000, "qos", TRUE, "gpu-id", 3u,
        "idrinterval", 2000u, NULL);
    if (i == 0)
      g_object_set (e, "disable-cabac", TRUE, NULL);
    g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*preset-id*no effect*");
    g_object_set (e, "preset-id", 7u, NULL);
    g_test_assert_expected_messages ();
    g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
    va = gst_bin_get_by_name (GST_BIN (e), "encoder");
    conv = gst_bin_get_by_name (GST_BIN (e), "convert");
    g_assert_true (G_TYPE_CHECK_INSTANCE_TYPE (va, rds_test_va_props_get_type ()));
    /* left over from an earlier run: the bin sets VA's defaults back */
    g_object_set (va, "qpi", 40u, "ref-frames", 5u, NULL);

    g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*idrinterval 2000*1024*");
    start_any (p);
    g_test_assert_expected_messages ();
    rc = get_enum_nick (va, "rate-control");
    g_assert_cmpstr (rc, ==, "vbr");
    g_free (rc);
    g_assert_cmpuint (get_uint (va, "bitrate"), ==, 2500);
    g_assert_cmpuint (get_uint (va, "target-percentage"), ==, 62);
    g_assert_cmpuint (get_uint (va, "qpi"), ==, 26);    /* constqp only with constantQP */
    g_assert_cmpuint (get_uint (va, "qpp"), ==, 26);
    g_assert_cmpuint (get_uint (va, "qpb"), ==, 26);
    g_assert_cmpuint (get_uint (va, "min-qp"), ==, 10);
    g_assert_cmpuint (get_uint (va, "max-qp"), ==, 40);
    g_assert_cmpuint (get_uint (va, "cpb-size"), ==, 3000);
    g_assert_cmpuint (get_uint (va, "key-int-max"), ==, 1024);
    g_assert_cmpuint (get_uint (va, "i-frames"), ==, 10);
    g_assert_cmpuint (get_uint (va, "target-usage"), ==, 1);
    g_assert_cmpuint (get_uint (va, "b-frames"), ==, 2);
    g_assert_cmpuint (get_uint (va, "ref-frames"), ==, 3);
    g_assert_cmpint (get_bool (va, "cabac"), ==, i != 0);       /* H.265 has no disable-cabac: VA's default */
    g_assert_true (get_bool (va, "aud"));
    g_assert_true (get_bool (va, "qos"));
    g_assert_cmpuint (get_uint64 (va, "min-force-key-unit-interval"), ==, 5000);
    g_assert_cmpuint (get_uint (conv, "gpu-id"), ==, 3);

    /* the next start reads the properties again */
    gst_element_set_state (p, GST_STATE_READY);
    g_object_set (e, "control-rate", 2, "maxbitrate", 40000000u, "num-Ref-Frames", 4u, "bitrate", 3000000000u,
        "idrinterval", 0u, "iframeinterval", 0u, "qp-range", NULL, "gpu-id", 0u, NULL);
    g_object_set (va, "target-percentage", 80u, NULL);
    start_any (p);
    rc = get_enum_nick (va, "rate-control");
    g_assert_cmpstr (rc, ==, "cqp");
    g_free (rc);
    g_assert_cmpuint (get_uint (va, "qpi"), ==, 20);
    g_assert_cmpuint (get_uint (va, "qpp"), ==, 22);
    g_assert_cmpuint (get_uint (va, "qpb"), ==, 24);
    g_assert_cmpuint (get_uint (va, "target-percentage"), ==, 66);      /* maxbitrate: VBR only */
    g_assert_cmpuint (get_uint (va, "min-qp"), ==, 1);
    g_assert_cmpuint (get_uint (va, "max-qp"), ==, 51);
    g_assert_cmpuint (get_uint (va, "ref-frames"), ==, 4);
    g_assert_cmpuint (get_uint (va, "bitrate"), ==, 2048000);   /* VA's largest */
    g_assert_cmpuint (get_uint (va, "key-int-max"), ==, 256);   /* idrinterval 0 is 256 */
    g_assert_cmpuint (get_uint (va, "i-frames"), ==, 0);

    /* a target percentage below VA's smallest (50) is raised to it */
    gst_element_set_state (p, GST_STATE_READY);
    g_object_set (e, "control-rate", 1, NULL);
    start_any (p);
    rc = get_enum_nick (va, "rate-control");
    g_assert_cmpstr (rc, ==, "cbr");
    g_free (rc);
    gst_element_set_state (p, GST_STATE_READY);
    g_object_set (e, "control-rate", 0, "bitrate", 1000000u, "maxbitrate", 30000000u, NULL);
    start_any (p);
    g_assert_cmpuint (get_uint (va, "target-percentage"), ==, 50);

    /* bitrate is live: forwarded at once */
    g_object_set (e, "bitrate", 1500000u, NULL);
    g_assert_cmpuint (get_uint (va, "bitrate"), ==, 1500);

    gst_element_set_state (p, GST_STATE_NULL);
    gst_object_unref (va);
    gst_object_unref (conv);
    gst_object_unref (p);
    set_va (c, VA_REAL);
  }
}

/* The profiles in @caps' profile fields, sorted and comma-separated ("" when none). */
static gchar *
profile_set (const GstCaps * caps)
{
  GPtrArray *names = g_ptr_array_new ();
  gchar *s;

  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    const GValue *v = gst_structure_get_value (gst_caps_get_structure (caps, i), "profile");

    if (v == NULL)
      continue;
    if (G_VALUE_HOLDS_STRING (v)) {
      g_ptr_array_add (names, (gpointer) g_value_get_string (v));
    } else {
      g_assert_true (GST_VALUE_HOLDS_LIST (v));
      for (guint j = 0; j < gst_value_list_get_size (v); j++)
        g_ptr_array_add (names, (gpointer) g_value_get_string (gst_value_list_get_value (v, j)));
    }
  }
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
  g_ptr_array_add (names, NULL);
  s = g_strjoinv (",", (gchar **) names->pdata);
  g_ptr_array_unref (names);
  return s;
}

/* The answer the VA encoder gets to its downstream caps query, as the profiles in it, with the bin's `profile`
 * @pref, `num-B-Frames` @b and the caps @down (or none) after the bin; *@b_out: the encoder's b-frames then.
 * @queries: how many times the encoder asks (VA asks again when it renegotiates). */
static gchar *
query_profiles (guint codec, gint pref, guint b, const gchar * down, guint queries, guint * b_out)
{
  const Codec *c = &codecs[codec];
  gchar *desc, *filter_s, *s = NULL;
  GstElement *p, *e, *va;
  GstPad *pad;
  GstCaps *filter;

  set_va (c, VA_PROPS);
  desc = g_strdup_printf ("%s name=enc profile=%d num-B-Frames=%u ! %s ! fakesink", c->element, pref, b,
      down != NULL ? down : "identity");
  p = gst_parse_launch (desc, NULL);
  g_assert_nonnull (p);
  e = gst_bin_get_by_name (GST_BIN (p), "enc");
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  start_any (p);
  va = gst_bin_get_by_name (GST_BIN (e), "encoder");
  pad = gst_element_get_static_pad (va, "src");
  /* VA asks with its src template */
  filter_s = g_strdup_printf ("%s, profile=(string){ %s }, stream-format=(string)byte-stream, alignment=(string)au",
      c->caps_name, codec == 0 ? "constrained-baseline, main, high" : "main, main-10");
  filter = gst_caps_from_string (filter_s);
  for (guint i = 0; i < queries; i++) {
    GstCaps *res = gst_pad_peer_query_caps (pad, filter);
    gchar *rs = gst_caps_to_string (res);

    g_free (s);
    s = profile_set (res);
    g_test_message ("%s, profile %d, b %u: %s", down ? down : "no caps", pref, b, rs);
    g_free (rs);
    gst_caps_unref (res);
  }
  *b_out = get_uint (va, "b-frames");
  gst_caps_unref (filter);
  g_free (filter_s);
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (pad);
  gst_object_unref (va);
  gst_object_unref (e);
  gst_object_unref (p);
  g_free (desc);
  set_va (c, VA_REAL);
  return s;
}

static void
check_profiles (guint codec, gint pref, guint b, const gchar * down, const gchar * want, guint want_b,
    gboolean warn)
{
  guint b_out;
  gchar *got;

  if (warn)
    g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*num-B-Frames*Baseline*");
  /* twice: one warning per run */
  got = query_profiles (codec, pref, b, down, 2, &b_out);
  g_test_assert_expected_messages ();
  g_assert_cmpstr (got, ==, want);
  g_assert_cmpuint (b_out, ==, want_b);
  g_free (got);
}

/* The `profile` property is a preference: the bin narrows the VA encoder's downstream caps answer to it when
 * downstream allows it; downstream caps that fix the profile win. With B frames and a Baseline result: no B
 * frames and a warning; with B frames and a choice, the Baseline profiles are left out. */
static void
test_preferred_profile (void)
{
  /* The answer is narrowed only while the bin is started, and its rdsvideoconvert needs a GPU to start (a failed
   * start clears the run's profile), so this is a gpu test. The decisions themselves are checked on plain caps by
   * enc_caps. */
  if (!rds_test_have_gpu ()) {
    g_test_skip ("no ROCm device: the bin can't start");
    return;
  }
  /* H.264: Baseline 0, Main 2, High 4 */
  check_profiles (0, 2, 0, NULL, "main", 0, FALSE);
  check_profiles (0, 0, 0, NULL, "constrained-baseline", 0, FALSE);
  check_profiles (0, 4, 0, NULL, "high", 0, FALSE);
  check_profiles (0, 2, 0, "video/x-h264,profile=high", "high", 0, FALSE);
  check_profiles (0, 4, 0, "video/x-h264,profile={main,constrained-baseline}", "constrained-baseline,main", 0,
      FALSE);
  check_profiles (0, 2, 0, "video/x-h264,profile={main,high}", "main", 0, FALSE);
  /* B frames */
  check_profiles (0, 2, 2, NULL, "main", 2, FALSE);
  check_profiles (0, 0, 2, NULL, "constrained-baseline", 0, TRUE);
  check_profiles (0, 1, 1, NULL, "constrained-baseline", 0, TRUE);
  check_profiles (0, 4, 2, "video/x-h264,profile=constrained-baseline", "constrained-baseline", 0, TRUE);
  check_profiles (0, 2, 2, "video/x-h264,profile={constrained-baseline,high}", "high", 2, FALSE);
  /* the preferred Baseline is allowed, so it is chosen, without B frames */
  check_profiles (0, 0, 2, "video/x-h264,profile={constrained-baseline,main}", "constrained-baseline", 0, TRUE);
  /* H.265: Main */
  check_profiles (1, 0, 0, NULL, "main", 0, FALSE);
  check_profiles (1, 0, 2, NULL, "main", 2, FALSE);
  check_profiles (1, 0, 0, "video/x-h265,profile=main-10", "main-10", 0, FALSE);
}

/* ---- the output probes on a stand-in VA encoder ---- */

/* The bin in READY (its rdsvideoconvert needs a GPU to start) with the stand-in encoder and the src capsfilter
 * started on their own: what is pushed from the stand-in's src pad goes through the bin's output probes to a test
 * pad on the ghost src, and events sent to its sink pad go through the input probes and stop there. */
typedef struct
{
  GstElement *e, *va, *out;
  GstPad *va_sink, *va_src, *ghost, *sink;
  GPtrArray *got;               /* buffers */
  GArray *segs;                 /* GstSegment */
} Harness;

static GstFlowReturn
harness_chain (GstPad * pad, G_GNUC_UNUSED GstObject * parent, GstBuffer * buf)
{
  Harness *h = g_object_get_data (G_OBJECT (pad), "harness");

  g_ptr_array_add (h->got, buf);
  return GST_FLOW_OK;
}

static gboolean
harness_event (GstPad * pad, G_GNUC_UNUSED GstObject * parent, GstEvent * ev)
{
  Harness *h = g_object_get_data (G_OBJECT (pad), "harness");

  if (GST_EVENT_TYPE (ev) == GST_EVENT_SEGMENT) {
    GstSegment seg;

    gst_event_copy_segment (ev, &seg);
    g_array_append_val (h->segs, seg);
  }
  gst_event_unref (ev);
  return TRUE;
}

static gboolean
drop_event (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstObject * parent, GstEvent * ev)
{
  gst_event_unref (ev);
  return TRUE;
}

static void
harness_start (Harness * h, const Codec * c)
{
  GstCaps *caps;

  set_va (c, VA_STAND_IN);
  h->e = gst_element_factory_make (c->element, NULL);
  h->got = g_ptr_array_new_with_free_func ((GDestroyNotify) gst_buffer_unref);
  h->segs = g_array_new (FALSE, FALSE, sizeof (GstSegment));
  g_assert_cmpint (gst_element_set_state (h->e, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  h->va = gst_bin_get_by_name (GST_BIN (h->e), "encoder");
  h->out = gst_bin_get_by_name (GST_BIN (h->e), "srccaps");
  h->ghost = gst_element_get_static_pad (h->e, "src");
  h->sink = gst_pad_new ("sink", GST_PAD_SINK);
  g_object_set_data (G_OBJECT (h->sink), "harness", h);
  gst_pad_set_chain_function (h->sink, harness_chain);
  gst_pad_set_event_function (h->sink, harness_event);
  g_assert_cmpint (gst_pad_link (h->ghost, h->sink), ==, GST_PAD_LINK_OK);
  gst_pad_set_active (h->sink, TRUE);
  gst_pad_set_active (h->ghost, TRUE);
  g_assert_cmpint (gst_element_set_state (h->out, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpint (gst_element_set_state (h->va, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_SUCCESS);
  h->va_src = gst_element_get_static_pad (h->va, "src");
  h->va_sink = gst_element_get_static_pad (h->va, "sink");
  gst_pad_set_event_function (h->va_sink, drop_event);

  g_assert_true (gst_pad_push_event (h->va_src, gst_event_new_stream_start ("harness")));
  caps = gst_caps_new_simple (c->caps_name, "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
      G_TYPE_STRING, "au", "width", G_TYPE_INT, 320, "height", G_TYPE_INT, 240, NULL);
  g_assert_true (gst_pad_push_event (h->va_src, gst_event_new_caps (caps)));
  gst_caps_unref (caps);
}

static void
harness_stop (Harness * h, const Codec * c)
{
  gst_element_set_state (h->va, GST_STATE_READY);
  gst_element_set_state (h->out, GST_STATE_READY);
  gst_pad_set_active (h->ghost, FALSE);
  gst_pad_set_active (h->sink, FALSE);
  gst_pad_unlink (h->ghost, h->sink);
  gst_element_set_state (h->e, GST_STATE_NULL);
  gst_object_unref (h->va_sink);
  gst_object_unref (h->va_src);
  gst_object_unref (h->sink);
  gst_object_unref (h->ghost);
  gst_object_unref (h->out);
  gst_object_unref (h->va);
  gst_object_unref (h->e);
  g_ptr_array_unref (h->got);
  g_array_unref (h->segs);
  set_va (c, VA_REAL);
}

/* A TIME segment starting at @start, with @seqnum when not GST_SEQNUM_INVALID. */
static GstEvent *
segment_event (GstClockTime start, guint32 seqnum)
{
  GstSegment seg;
  GstEvent *ev;

  gst_segment_init (&seg, GST_FORMAT_TIME);
  seg.start = seg.position = start;
  seg.time = start;
  ev = gst_event_new_segment (&seg);
  if (seqnum != GST_SEQNUM_INVALID)
    gst_event_set_seqnum (ev, seqnum);
  return ev;
}

/* Pushes @n bytes on the stand-in encoder's src pad (the bin's output probe runs there). */
static GstFlowReturn
push_bytes (GstPad * va_src, const guint8 * bytes, gsize n)
{
  GstBuffer *buf = gst_buffer_new_memdup (bytes, n);

  GST_BUFFER_PTS (buf) = 0;
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
  return gst_pad_push (va_src, buf);
}

/* A buffer of filler NAL units only is dropped; filler next to a slice is cut out and the slice kept. */
static void
test_filler_only_dropped (void)
{
  /* H.264: filler (12); a non-IDR P slice (slice_type 5) */
  static const guint8 h264_filler[] = { 0, 0, 0, 1, 0x0c, 0xff, 0xff, 0x80, 0, 0, 0, 1, 0x0c, 0xff, 0x80 };
  static const guint8 h264_slice[] = { 0, 0, 0, 1, 0x41, 0x9a, 0x80 };
  static const guint8 h264_mixed[] = { 0, 0, 0, 1, 0x41, 0x9a, 0x80, 0, 0, 0, 1, 0x0c, 0xff, 0xff, 0x80 };
  /* H.265: FD_NUT (38); a TRAIL_R P slice */
  static const guint8 h265_filler[] = { 0, 0, 0, 1, 0x4c, 0x01, 0xff, 0xff, 0x80 };
  static const guint8 h265_slice[] = { 0, 0, 0, 1, 0x02, 0x01, 0xd0, 0x80 };
  static const guint8 h265_mixed[] = { 0, 0, 0, 1, 0x02, 0x01, 0xd0, 0x80, 0, 0, 0, 1, 0x4c, 0x01, 0xff, 0x80 };

  for (guint i = 0; i < G_N_ELEMENTS (codecs); i++) {
    const Codec *c = &codecs[i];
    const guint8 *filler = i == 0 ? h264_filler : h265_filler, *slice = i == 0 ? h264_slice : h265_slice;
    const guint8 *mixed = i == 0 ? h264_mixed : h265_mixed;
    gsize filler_n = i == 0 ? sizeof (h264_filler) : sizeof (h265_filler);
    gsize slice_n = i == 0 ? sizeof (h264_slice) : sizeof (h265_slice);
    gsize mixed_n = i == 0 ? sizeof (h264_mixed) : sizeof (h265_mixed);
    Harness h;

    harness_start (&h, c);
    g_assert_true (gst_pad_push_event (h.va_src, segment_event (0, GST_SEQNUM_INVALID)));
    g_assert_cmpint (push_bytes (h.va_src, filler, filler_n), ==, GST_FLOW_OK);
    g_assert_cmpuint (h.got->len, ==, 0);
    g_assert_cmpint (push_bytes (h.va_src, slice, slice_n), ==, GST_FLOW_OK);
    g_assert_cmpint (push_bytes (h.va_src, mixed, mixed_n), ==, GST_FLOW_OK);
    g_assert_cmpuint (h.got->len, ==, 2);
    for (guint k = 0; k < h.got->len; k++) {
      GstBuffer *b = g_ptr_array_index (h.got, k);

      g_assert_cmpuint (gst_buffer_get_size (b), ==, slice_n);
      g_assert_cmpint (gst_buffer_memcmp (b, 0, slice, slice_n), ==, 0);
      g_assert_true (GST_BUFFER_FLAG_IS_SET (b, GST_BUFFER_FLAG_DELTA_UNIT));
    }
    harness_stop (&h, c);
  }
}

/* Each output segment is matched with the input segment it comes from; one sent again with the same seqnum (sticky
 * events re-sent) takes none and keeps its shift. */
static void
test_segment_again (void)
{
  const Codec *c = &codecs[0];
  const GstClockTime shift = 1000 * 3600 * GST_SECOND;
  Harness h;
  GstEvent *ev;

  harness_start (&h, c);
  /* VA's input: two segments queued before VA pushes the first output segment */
  g_assert_true (gst_pad_send_event (h.va_sink, segment_event (0, GST_SEQNUM_INVALID)));
  g_assert_true (gst_pad_send_event (h.va_sink, segment_event (5 * GST_SECOND, GST_SEQNUM_INVALID)));
  /* VA's output for the first, shifted, then the same again */
  ev = segment_event (shift, 1234);
  g_assert_true (gst_pad_push_event (h.va_src, gst_event_copy (ev)));
  g_assert_true (gst_pad_push_event (h.va_src, ev));
  /* then for the second: VA's shift is the one it measured on the first frame */
  g_assert_true (gst_pad_push_event (h.va_src, segment_event (5 * GST_SECOND + shift, 1235)));
  g_assert_cmpuint (h.segs->len, ==, 3);
  g_assert_cmpuint (g_array_index (h.segs, GstSegment, 0).start, ==, 0);
  g_assert_cmpuint (g_array_index (h.segs, GstSegment, 1).start, ==, 0);
  g_assert_cmpuint (g_array_index (h.segs, GstSegment, 2).start, ==, 5 * GST_SECOND);
  harness_stop (&h, c);
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

/* ---- gpu: the mapping on the real VA encoders (1280x720 at 30 fps from videotestsrc) ---- */

#define W 1280
#define H 720
#define FPS 30

static gboolean
is_hevc (const Codec * c)
{
  return c == &codecs[1];
}

/* What an encode gave. */
typedef struct
{
  GByteArray *data;             /* the whole byte stream */
  GArray *sizes;                /* guint per output buffer: bytes without filler NAL units */
  GstCaps *caps;                /* the output caps */
  /* a live bitrate change before input frame change_at */
  GstElement *enc;
  guint change_at, new_bitrate, in_frames;
} Encoded;

static void
encoded_clear (Encoded * r)
{
  g_byte_array_unref (r->data);
  g_array_unref (r->sizes);
  gst_clear_caps (&r->caps);
}

static void
on_encoded (G_GNUC_UNUSED GstElement * sink, GstBuffer * buf, GstPad * pad, gpointer user_data)
{
  Encoded *r = user_data;
  GstMapInfo map;
  GArray *nals;
  guint bytes = 0;
  gboolean hevc;

  if (r->caps == NULL)
    r->caps = gst_pad_get_current_caps (pad);
  hevc = g_str_equal (gst_structure_get_name (gst_caps_get_structure (r->caps, 0)), "video/x-h265");
  g_assert_true (gst_buffer_map (buf, &map, GST_MAP_READ));
  g_byte_array_append (r->data, map.data, map.size);
  nals = rds_test_nal_split (map.data, map.size, hevc);
  for (guint i = 0; i < nals->len; i++) {
    const RdsTestNal *n = &g_array_index (nals, RdsTestNal, i);

    if (n->type != (hevc ? 38u : 12u))
      bytes += n->size + 4;     /* with a 4-byte start code */
  }
  g_array_append_val (r->sizes, bytes);
  g_array_unref (nals);
  gst_buffer_unmap (buf, &map);
}

static GstPadProbeReturn
count_input (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  Encoded *r = user_data;

  if (r->new_bitrate > 0 && r->in_frames == r->change_at)
    g_object_set (r->enc, "bitrate", r->new_bitrate, NULL);
  r->in_frames++;
  return GST_PAD_PROBE_OK;
}

/* Encodes @frames frames of videotestsrc @src (its properties) through @c's element with @props, then @down (caps
 * after the element; NULL: the codec's caps name) into @r. */
static void
encode (const Codec * c, const gchar * src, const gchar * props, const gchar * down, guint frames, Encoded * r)
{
  gchar *desc = g_strdup_printf ("videotestsrc num-buffers=%u %s ! video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
      "%s name=enc %s ! %s ! fakesink name=sink signal-handoffs=true", frames, src, W, H, FPS, c->element,
      props, down != NULL ? down : c->caps_name);
  GstElement *p = gst_parse_launch (desc, NULL), *sink;
  GstPad *pad;

  g_assert_nonnull (p);
  g_test_message ("%s", desc);
  r->data = g_byte_array_new ();
  r->sizes = g_array_new (FALSE, FALSE, sizeof (guint));
  r->caps = NULL;
  r->in_frames = 0;
  r->enc = gst_bin_get_by_name (GST_BIN (p), "enc");
  pad = gst_element_get_static_pad (r->enc, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_input, r, NULL);
  gst_object_unref (pad);
  sink = gst_bin_get_by_name (GST_BIN (p), "sink");
  g_signal_connect (sink, "handoff", G_CALLBACK (on_encoded), r);
  run_to_eos (p);
  g_assert_cmpuint (r->sizes->len, ==, frames);
  gst_object_unref (sink);
  gst_clear_object (&r->enc);
  gst_object_unref (p);
  g_free (desc);
}

static gboolean
au_is_idr (const RdsTestAu * au, gboolean hevc)
{
  return hevc ? rds_test_au_has (au, 19) || rds_test_au_has (au, 20) : rds_test_au_has (au, 5);
}

static gboolean
au_has_parameter_sets (const RdsTestAu * au, gboolean hevc)
{
  return hevc ? rds_test_au_has (au, 32) && rds_test_au_has (au, 33) && rds_test_au_has (au, 34)
      : rds_test_au_has (au, 7) && rds_test_au_has (au, 8);
}

/* The first SPS of @r. */
static void
first_sps (const Codec * c, const Encoded * r, RdsTestSps * sps)
{
  GArray *nals = rds_test_nal_split (r->data->data, r->data->len, is_hevc (c));

  for (guint i = 0; i < nals->len; i++) {
    const RdsTestNal *n = &g_array_index (nals, RdsTestNal, i);

    if (n->type == (is_hevc (c) ? 33u : 7u)) {
      if (is_hevc (c))
        rds_test_h265_sps (n->data, n->size, sps);
      else
        rds_test_h264_sps (n->data, n->size, sps);
      g_array_unref (nals);
      return;
    }
  }
  g_error ("no SPS");
}

/* The caps level string for a level_idc (H.264: 31 -> 3.1; HEVC: 30 x level, 93 -> 3.1). */
static gchar *
level_string (const Codec * c, guint idc)
{
  if (is_hevc (c))
    return idc % 30 == 0 ? g_strdup_printf ("%u", idc / 30) : g_strdup_printf ("%u.%u", idc / 30, idc % 30 / 3);
  return idc % 10 == 0 ? g_strdup_printf ("%u", idc / 10) : g_strdup_printf ("%u.%u", idc / 10, idc % 10);
}

/* The output caps carry the profile and level the SPS has. */
static void
check_caps_match_sps (const Codec * c, const Encoded * r, const gchar * profile, guint profile_idc)
{
  const GstStructure *st = gst_caps_get_structure (r->caps, 0);
  RdsTestSps sps;
  gchar *level;

  first_sps (c, r, &sps);
  g_assert_cmpuint (sps.profile_idc, ==, profile_idc);
  g_assert_cmpstr (gst_structure_get_string (st, "profile"), ==, profile);
  level = level_string (c, sps.level_idc);
  g_assert_cmpstr (gst_structure_get_string (st, "level"), ==, level);
  g_free (level);
}

/* GOP layout: the first frame and every @idr-th are IDR frames with parameter sets before them; between them,
 * @i_frames intra frames (at multiples of @every when @every > 0), the rest P frames. */
static void
check_gop (const Codec * c, guint idrinterval, guint iframeinterval, guint idr, guint i_frames, guint every)
{
  gboolean hevc = is_hevc (c);
  gchar *props = iframeinterval > 0 ? g_strdup_printf ("idrinterval=%u iframeinterval=%u", idrinterval,
      iframeinterval) : g_strdup_printf ("idrinterval=%u", idrinterval);
  Encoded r = { 0 };
  GArray *aus;
  guint intra_in_gop = 0;

  encode (c, "pattern=ball", props, NULL, 300, &r);
  aus = rds_test_nal_aus (r.data->data, r.data->len, hevc);
  g_assert_cmpuint (aus->len, ==, 300);
  for (guint k = 0; k < aus->len; k++) {
    const RdsTestAu *au = &g_array_index (aus, RdsTestAu, k);
    guint pos = k % idr;

    if (pos == 0) {
      g_assert_true (au_is_idr (au, hevc));
      g_assert_true (au_has_parameter_sets (au, hevc));
      if (k > 0)
        g_assert_cmpuint (intra_in_gop, ==, i_frames);
      intra_in_gop = 0;
      continue;
    }
    g_assert_false (au_is_idr (au, hevc));
    g_assert_cmpuint (au->first_slice_type, !=, RDS_TEST_H264_SLICE_B);
    if (au->first_slice_type == RDS_TEST_H264_SLICE_I) {
      intra_in_gop++;
      if (every > 0)
        g_assert_cmpuint (pos % every, ==, 0);
    } else {
      g_assert_cmpuint (au->first_slice_type, ==, RDS_TEST_H264_SLICE_P);
      if (every > 0)
        g_assert_cmpuint (pos % every, !=, 0);
    }
  }
  g_array_unref (aus);
  encoded_clear (&r);
  g_free (props);
}

static void
test_gop (gconstpointer data)
{
  const Codec *c = data;

  if (!require_va (c))
    return;
  check_gop (c, 30, 0, 30, 0, 0);       /* iframeinterval: none */
  check_gop (c, 30, 10, 30, 2, 10);
  check_gop (c, 45, 30, 45, 1, 0);      /* VA spaces its one I frame evenly, not at 30 */
  check_gop (c, 256, 0, 256, 0, 0);     /* the defaults */
}

typedef struct
{
  gint value;
  const gchar *caps_profile;
  guint profile_idc;
} ProfileCase;

static void
test_profiles (gconstpointer data)
{
  const Codec *c = data;
  static const ProfileCase h264[] = {
    {0, "constrained-baseline", 66}, {1, "constrained-baseline", 66}, {2, "main", 77}, {4, "high", 100},
    {17, "high", 100},
  };
  static const ProfileCase h265[] = { {0, "main", 1} };
  const ProfileCase *cases = is_hevc (c) ? h265 : h264;
  guint n = is_hevc (c) ? G_N_ELEMENTS (h265) : G_N_ELEMENTS (h264);
  Encoded r;

  if (!require_va (c))
    return;
  for (guint i = 0; i < n; i++) {
    gchar *props = g_strdup_printf ("profile=%d", cases[i].value);

    encode (c, "pattern=ball", props, NULL, 10, &r);
    check_caps_match_sps (c, &r, cases[i].caps_profile, cases[i].profile_idc);
    encoded_clear (&r);
    g_free (props);
  }
  /* downstream caps that fix the profile win */
  if (is_hevc (c)) {
    encode (c, "pattern=ball", "profile=0", "video/x-h265,profile=main", 10, &r);
    check_caps_match_sps (c, &r, "main", 1);
  } else {
    encode (c, "pattern=ball", "profile=4", "video/x-h264,profile=main", 10, &r);
    check_caps_match_sps (c, &r, "main", 77);
    encoded_clear (&r);
    encode (c, "pattern=ball", "profile=0", "video/x-h264,profile={main,high}", 10, &r);
    check_caps_match_sps (c, &r, "high", 100);
  }
  encoded_clear (&r);
}

/* VA sets the level itself: a downstream level equal to VA's negotiates (3.1 at 720p30 and 4 Mbit/s), another is
 * not-negotiated with a warning naming both (ours: DeepStream 8.0 writes the downstream level). */
static void
test_downstream_level (gconstpointer data)
{
  const Codec *c = data;
  gchar *down, *desc;
  Encoded r;
  GstElement *p;
  GstBus *bus;
  GstMessage *m;
  GError *err = NULL;

  if (!require_va (c))
    return;
  down = g_strdup_printf ("%s,level=(string)3.1", c->caps_name);
  encode (c, "pattern=ball", "", down, 10, &r);
  g_assert_cmpstr (gst_structure_get_string (gst_caps_get_structure (r.caps, 0), "level"), ==, "3.1");
  encoded_clear (&r);
  g_free (down);

  desc = g_strdup_printf ("videotestsrc num-buffers=10 ! video/x-raw,width=%d,height=%d,framerate=%d/1 ! %s ! "
      "%s,level=(string)5.1 ! fakesink", W, H, FPS, c->element, c->caps_name);
  p = gst_parse_launch (desc, NULL);
  bus = gst_element_get_bus (p);
  g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*level 5.1*VA encoder*3.1*");
  gst_element_set_state (p, GST_STATE_PLAYING);
  m = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (m);
  g_assert_cmpint (GST_MESSAGE_TYPE (m), ==, GST_MESSAGE_ERROR);
  gst_message_parse_error (m, &err, NULL);
  g_assert_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED);    /* not-negotiated */
  g_clear_error (&err);
  gst_message_unref (m);
  gst_element_set_state (p, GST_STATE_NULL);
  g_test_assert_expected_messages ();
  gst_object_unref (bus);
  gst_object_unref (p);
  g_free (desc);
}

/* kbit/s of output buffers [@from, @to) without filler NAL units */
static gdouble
rate_kbps (const Encoded * r, guint from, guint to)
{
  guint64 bytes = 0;

  for (guint i = from; i < to; i++)
    bytes += g_array_index (r->sizes, guint, i);
  return bytes * 8.0 / 1000.0 / ((to - from) / (gdouble) FPS);
}

/* Content hard enough that CBR needs no filler to reach its target (it may still add some). */
#define HARD_SRC "pattern=smpte horizontal-speed=8"

static void
test_cbr (gconstpointer data)
{
  const Codec *c = data;
  static const guint targets[] = { 1000000, 4000000 };

  if (!require_va (c))
    return;
  for (guint i = 0; i < G_N_ELEMENTS (targets); i++) {
    gchar *props = g_strdup_printf ("bitrate=%u", targets[i]);
    Encoded r;
    gdouble kbps;

    encode (c, HARD_SRC, props, NULL, 300, &r);
    /* VA's H.265 CBR settles about 20 % above a 1 Mbit/s target on this content (measured: ~1245 kbit/s, QP 33..48,
     * so not out of QP range); everything else holds +-10 % */
    gdouble over = is_hevc (c) && targets[i] == 1000000 ? 1.3 : 1.1;

    kbps = rate_kbps (&r, 0, 300);
    g_test_message ("%s CBR %u kbit/s: %.0f kbit/s without filler", c->element, targets[i] / 1000, kbps);
    g_assert_cmpfloat (kbps, >=, targets[i] / 1000.0 * 0.9);
    g_assert_cmpfloat (kbps, <=, targets[i] / 1000.0 * over);
    encoded_clear (&r);
    g_free (props);
  }
}

/* bitrate is live: 4 -> 1 Mbit/s halfway. VA restarts rate control there (an IDR) and settles above the new target,
 * so only a drop of at least 40 % is asserted. */
static void
test_live_bitrate (gconstpointer data)
{
  const Codec *c = data;
  Encoded r = { .change_at = 150, .new_bitrate = 1000000 };
  gdouble first, second;

  if (!require_va (c))
    return;
  encode (c, HARD_SRC, "bitrate=4000000", NULL, 300, &r);
  /* the first 30 frames of each half: VA's rate control settling */
  first = rate_kbps (&r, 30, 150);
  second = rate_kbps (&r, 180, 300);
  g_test_message ("%s: %.0f kbit/s, then %.0f kbit/s", c->element, first, second);
  g_assert_cmpfloat (second, <=, first * 0.6);
  encoded_clear (&r);
}

/* Counts the H.264 B pictures of @r by the first slice of each AU (VA writes one slice per picture). */
static guint
count_b (const Codec * c, const Encoded * r)
{
  GArray *aus = rds_test_nal_aus (r->data->data, r->data->len, is_hevc (c));
  guint b = 0;

  for (guint k = 0; k < aus->len; k++)
    b += g_array_index (aus, RdsTestAu, k).first_slice_type == RDS_TEST_H264_SLICE_B;
  g_array_unref (aus);
  return b;
}

/* H.264 B frames: with Main they are there; with Baseline, from the property or from downstream caps, a warning and
 * none (VA would write them into a Baseline stream). count_b looks at the first slice of each AU only, which is every
 * slice: VA writes one slice per picture. This also guards the order of VA's reconfiguration that the bin's
 * caps-query probe relies on. */
static void
test_b_frames (void)
{
  const Codec *c = &codecs[0];
  Encoded r;

  if (!require_va (c))
    return;
  encode (c, "pattern=ball", "profile=2 num-B-Frames=2", NULL, 60, &r);
  g_assert_cmpuint (count_b (c, &r), >, 0);
  check_caps_match_sps (c, &r, "main", 77);
  encoded_clear (&r);

  g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*num-B-Frames=2*Baseline*");
  encode (c, "pattern=ball", "profile=0 num-B-Frames=2", NULL, 60, &r);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (count_b (c, &r), ==, 0);
  check_caps_match_sps (c, &r, "constrained-baseline", 66);
  encoded_clear (&r);

  g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*num-B-Frames=1*Baseline*");
  encode (c, "pattern=ball", "profile=4 num-B-Frames=1", "video/x-h264,profile=constrained-baseline", 60, &r);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (count_b (c, &r), ==, 0);
  check_caps_match_sps (c, &r, "constrained-baseline", 66);
  encoded_clear (&r);
}

/* preset-id doesn't reach the VA encoder (target-usage is always 1); gpu-id reaches rdsvideoconvert. */
static void
test_inner_settings (gconstpointer data)
{
  const Codec *c = data;
  gchar *desc = g_strdup_printf ("videotestsrc ! video/x-raw,width=%d,height=%d ! %s name=enc ! fakesink", W, H,
      c->element);
  GstElement *p, *e, *va, *conv;

  if (!require_va (c)) {
    g_free (desc);
    return;
  }
  p = gst_parse_launch (desc, NULL);
  e = gst_bin_get_by_name (GST_BIN (p), "enc");
  g_test_expect_message ("rdsenc", G_LOG_LEVEL_WARNING, "*preset-id*no effect*");
  g_object_set (e, "preset-id", 7u, NULL);
  g_test_assert_expected_messages ();
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  va = gst_bin_get_by_name (GST_BIN (e), "encoder");
  conv = gst_bin_get_by_name (GST_BIN (e), "convert");
  g_object_set (conv, "gpu-id", 5u, NULL);      /* the start sets the bin's (0) */
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_element_get_state (p, NULL, NULL, 10 * GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpuint (get_uint (va, "target-usage"), ==, 1);
  g_assert_cmpuint (get_uint (conv, "gpu-id"), ==, 0);
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (va);
  gst_object_unref (conv);
  gst_object_unref (e);
  gst_object_unref (p);
  g_free (desc);
}

/* ---- gpu: what the bin's pad probes do to the VA encoders' output ---- */

#define SW 320
#define SH 240
#define HOUR (3600 * GST_SECOND)
#define FRAME_DUR (GST_SECOND / FPS)

typedef struct
{
  GstClockTime pts, duration;
  gint fps_n;                   /* the framerate numerator of its caps */
} InBuf;

typedef struct
{
  GstClockTime pts, dts, duration;
  gboolean delta, idr, discont;
  guint first_slice_type;
  guint filler;                 /* filler NAL units */
  gint source_id;               /* the batch meta's first frame's source_id; -1: no batch meta */
  guint meta_frames;
  gint fps_n;                   /* the framerate numerator of the caps it came with */
} OutBuf;

/* What a run through the bin gave, seen just before the bin (input) and at the sink (output). */
typedef struct
{
  gboolean hevc;
  gboolean tag;                 /* attach a batch meta (one frame, source_id = input index) to every input */
  GArray *in, *out;             /* InBuf, OutBuf */
  GByteArray *data;
  GstSegment in_seg, out_seg;
  guint in_segs, out_segs;
  gint in_fps_n;
  GstCaps *first_caps, *last_caps;
  /* a caps change before input frame change_at: cf's caps become change_caps */
  GstElement *cf;
  GstCaps *change_caps;
  guint change_at;
} Run;

static void
run_clear (Run * r)
{
  g_array_unref (r->in);
  g_array_unref (r->out);
  g_byte_array_unref (r->data);
  gst_clear_caps (&r->first_caps);
  gst_clear_caps (&r->last_caps);
  gst_clear_caps (&r->change_caps);
  gst_clear_object (&r->cf);
}

static GstPadProbeReturn
run_in_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Run *r = user_data;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
    InBuf ib = { GST_BUFFER_PTS (buf), GST_BUFFER_DURATION (buf), r->in_fps_n };

    if (r->change_caps != NULL && r->in->len == r->change_at)
      g_object_set (r->cf, "caps", r->change_caps, NULL);
    if (r->tag) {
      RdsBatchMeta *bm = rds_create_batch_meta (1);
      RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

      buf = gst_buffer_make_writable (buf);
      fm->source_id = r->in->len;
      fm->frame_num = (gint) r->in->len;
      rds_add_frame_meta_to_batch (bm, fm);
      g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
      GST_PAD_PROBE_INFO_DATA (info) = buf;
    }
    g_array_append_val (r->in, ib);
  } else {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);

    GstCaps *caps;
    gint d;

    if (GST_EVENT_TYPE (ev) == GST_EVENT_SEGMENT) {
      gst_event_copy_segment (ev, &r->in_seg);
      r->in_segs++;
    } else if (GST_EVENT_TYPE (ev) == GST_EVENT_CAPS) {
      gst_event_parse_caps (ev, &caps);
      g_assert_true (gst_structure_get_fraction (gst_caps_get_structure (caps, 0), "framerate", &r->in_fps_n, &d));
    }
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
run_out_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Run *r = user_data;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
    RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
    OutBuf ob = { GST_BUFFER_PTS (buf), GST_BUFFER_DTS (buf), GST_BUFFER_DURATION (buf),
      GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT), FALSE, GST_BUFFER_FLAG_IS_SET (buf,
          GST_BUFFER_FLAG_DISCONT), G_MAXUINT, 0, -1, 0, 0
    };
    gint d;
    GstMapInfo map;
    GArray *aus;

    g_assert_nonnull (r->last_caps);
    g_assert_true (gst_structure_get_fraction (gst_caps_get_structure (r->last_caps, 0), "framerate", &ob.fps_n, &d));
    g_assert_true (gst_buffer_map (buf, &map, GST_MAP_READ));
    g_byte_array_append (r->data, map.data, map.size);
    aus = rds_test_nal_aus (map.data, map.size, r->hevc);
    g_assert_cmpuint (aus->len, ==, 1);
    ob.idr = au_is_idr (&g_array_index (aus, RdsTestAu, 0), r->hevc);
    ob.first_slice_type = g_array_index (aus, RdsTestAu, 0).first_slice_type;
    ob.filler = rds_test_nal_count (map.data, map.size, r->hevc, r->hevc ? 38 : 12);
    g_array_unref (aus);
    gst_buffer_unmap (buf, &map);
    if (bm != NULL) {
      ob.meta_frames = bm->num_frames_in_batch;
      if (bm->frame_meta_list != NULL)
        ob.source_id = (gint) ((RdsFrameMeta *) bm->frame_meta_list->data)->source_id;
    }
    g_array_append_val (r->out, ob);
  } else {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
    GstCaps *caps;

    if (GST_EVENT_TYPE (ev) == GST_EVENT_SEGMENT) {
      gst_event_copy_segment (ev, &r->out_seg);
      r->out_segs++;
    } else if (GST_EVENT_TYPE (ev) == GST_EVENT_CAPS) {
      gst_event_parse_caps (ev, &caps);
      if (r->first_caps == NULL)
        r->first_caps = gst_caps_ref (caps);
      gst_caps_replace (&r->last_caps, caps);
    }
  }
  return GST_PAD_PROBE_OK;
}

/* Runs @desc (an element named "enc", the bin; one named "sink") to EOS into @r; @r->tag, change_* set by the
 * caller (with change_caps, the capsfilter is the element named "cf"). With @seek_to valid, the pipeline is
 * prerolled, then a flushing seek to [@seek_to, @seek_to + 1 s) is made before playing. */
static void
run_pipeline (const Codec * c, const gchar * desc, Run * r, GstClockTime seek_to)
{
  GstElement *p = gst_parse_launch (desc, NULL), *enc, *sink;
  GstPad *pad, *peer;

  g_assert_nonnull (p);
  g_test_message ("%s", desc);
  r->hevc = is_hevc (c);
  r->in = g_array_new (FALSE, FALSE, sizeof (InBuf));
  r->out = g_array_new (FALSE, FALSE, sizeof (OutBuf));
  r->data = g_byte_array_new ();
  if (r->change_caps != NULL)
    r->cf = gst_bin_get_by_name (GST_BIN (p), "cf");
  enc = gst_bin_get_by_name (GST_BIN (p), "enc");
  pad = gst_element_get_static_pad (enc, "sink");
  peer = gst_pad_get_peer (pad);
  gst_pad_add_probe (peer, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, run_in_probe, r, NULL);
  gst_object_unref (peer);
  gst_object_unref (pad);
  sink = gst_bin_get_by_name (GST_BIN (p), "sink");
  pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, run_out_probe, r, NULL);
  gst_object_unref (pad);
  if (GST_CLOCK_TIME_IS_VALID (seek_to)) {
    g_assert_cmpint (gst_element_set_state (p, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
    g_assert_cmpint (gst_element_get_state (p, NULL, NULL, RDS_TEST_MEDIA_TIMEOUT), ==, GST_STATE_CHANGE_SUCCESS);
    g_assert_true (gst_element_seek (p, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
            GST_SEEK_TYPE_SET, seek_to, GST_SEEK_TYPE_SET, seek_to + GST_SECOND));
  }
  run_to_eos (p);
  gst_object_unref (sink);
  gst_object_unref (enc);
  gst_object_unref (p);
}

/* @frames of videotestsrc @src at SWxSH, 30 fps, through @c's element with @props. */
static void
run_simple (const Codec * c, const gchar * src, const gchar * props, guint frames, Run * r)
{
  gchar *desc = g_strdup_printf ("videotestsrc num-buffers=%u %s ! video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
      "%s name=enc %s ! fakesink name=sink", frames, src, SW, SH, FPS, c->element, props);

  run_pipeline (c, desc, r, GST_CLOCK_TIME_NONE);
  g_free (desc);
}

/* CBR on easy content pads with filler NAL units once VA's buffer has filled (a few seconds in): the bare VA encoder
 * with the settings the bin gives it writes some, the bin lets none out. */
static void
test_no_filler (gconstpointer data)
{
  const Codec *c = data;
  Run r = { 0 }, va = { 0 };
  gchar *props = g_strdup_printf ("bitrate=4000000 ! %s", c->caps_name), *desc;
  guint va_filler = 0;

  if (!require_va (c)) {
    g_free (props);
    return;
  }
  desc = g_strdup_printf ("videotestsrc num-buffers=300 pattern=ball ! video/x-raw,format=NV12,width=%d,height=%d,"
      "framerate=%d/1 ! %s name=enc rate-control=cbr bitrate=4000 target-usage=1 key-int-max=256 ! %s,"
      "stream-format=byte-stream,alignment=au ! fakesink name=sink", SW, SH, FPS, c->va_factory, c->caps_name);
  run_pipeline (c, desc, &va, GST_CLOCK_TIME_NONE);
  for (guint i = 0; i < va.out->len; i++)
    va_filler += g_array_index (va.out, OutBuf, i).filler;
  g_test_message ("%s alone: %u filler NAL units in %u frames", c->va_factory, va_filler, va.out->len);
  g_assert_cmpuint (va_filler, >, 0);

  run_simple (c, "pattern=ball", props, 300, &r);
  g_assert_cmpuint (r.out->len, ==, 300);
  for (guint i = 0; i < r.out->len; i++)
    g_assert_cmpuint (g_array_index (r.out, OutBuf, i).filler, ==, 0);
  g_assert_cmpuint (rds_test_nal_count (r.data->data, r.data->len, r.hevc, r.hevc ? 38 : 12), ==, 0);
  run_clear (&va);
  run_clear (&r);
  g_free (desc);
  g_free (props);
}

/* H.265 at widths that are not multiples of 64: the SPS and the decoder give the exact size (ours: VA's window
 * alone decodes 768 and 1398 wide). */
static void
check_h265_size (gint w, gint h)
{
  const Codec *c = &codecs[1];
  gchar *desc = g_strdup_printf ("videotestsrc num-buffers=10 pattern=ball ! video/x-raw,width=%d,height=%d,"
      "framerate=30/1 ! %s name=enc ! fakesink name=sink", w, h, c->element);
  gchar *dec = g_strdup_printf ("appsrc name=src caps=video/x-h265,stream-format=byte-stream format=bytes ! "
      "h265parse ! vah265dec ! video/x-raw ! appsink name=sink sync=false");
  Run r = { 0 };
  RdsTestSps sps;
  GArray *nals;
  GstElement *p, *src, *sink;
  GstSample *s;
  const GstStructure *st;
  gint dw = 0, dh = 0;

  run_pipeline (c, desc, &r, GST_CLOCK_TIME_NONE);
  g_assert_cmpuint (r.out->len, ==, 10);
  nals = rds_test_nal_split (r.data->data, r.data->len, TRUE);
  for (guint i = 0; i < nals->len; i++) {
    const RdsTestNal *n = &g_array_index (nals, RdsTestNal, i);

    if (n->type == 33) {
      rds_test_h265_sps (n->data, n->size, &sps);
      g_assert_cmpuint (sps.width, ==, (guint) w);
      g_assert_cmpuint (sps.height, ==, (guint) h);
    }
  }
  g_array_unref (nals);

  p = gst_parse_launch (dec, NULL);
  src = gst_bin_get_by_name (GST_BIN (p), "src");
  sink = gst_bin_get_by_name (GST_BIN (p), "sink");
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src), gst_buffer_new_memdup (r.data->data, r.data->len)),
      ==, GST_FLOW_OK);
  g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src)), ==, GST_FLOW_OK);
  s = rds_test_pull (p, sink);
  g_assert_nonnull (s);
  st = gst_caps_get_structure (gst_sample_get_caps (s), 0);
  g_assert_true (gst_structure_get_int (st, "width", &dw));
  g_assert_true (gst_structure_get_int (st, "height", &dh));
  g_test_message ("%dx%d decodes %dx%d", w, h, dw, dh);
  g_assert_cmpint (dw, ==, w);
  g_assert_cmpint (dh, ==, h);
  gst_sample_unref (s);
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (src);
  gst_object_unref (p);
  run_clear (&r);
  g_free (dec);
  g_free (desc);
}

static void
test_h265_size (void)
{
  if (!require_va (&codecs[1]) || !rds_test_have_element ("vah265dec")) {
    g_test_skip ("no ROCm device, VA encoder or vah265dec");
    return;
  }
  check_h265_size (720, 480);
  check_h265_size (1366, 768);
}

/* Without B frames, output PTS = DTS = input PTS and the duration and segment are the input's (DeepStream 8.0
 * behaviour): VA's shift (up to 1000 h) is removed. Only the first buffer is DISCONT. */
static void
check_unshifted (const Run * r)
{
  g_assert_cmpuint (r->out->len, ==, r->in->len);
  g_assert_cmpuint (r->out_segs, >=, 1);
  g_assert_true (gst_segment_is_equal (&r->in_seg, &r->out_seg));
  for (guint i = 0; i < r->out->len; i++) {
    const InBuf *ib = &g_array_index (r->in, InBuf, i);
    const OutBuf *ob = &g_array_index (r->out, OutBuf, i);

    g_assert_cmpuint (ob->pts, ==, ib->pts);
    g_assert_cmpuint (ob->dts, ==, ib->pts);
    g_assert_cmpuint (ob->duration, ==, ib->duration);
    g_assert_cmpint (ob->discont, ==, i == 0);
  }
}

static void
test_timestamps (gconstpointer data)
{
  const Codec *c = data;
  static const GstClockTime starts[] = { 0, 10 * GST_SECOND, 2000 * HOUR };

  if (!require_va (c))
    return;
  for (guint i = 0; i < G_N_ELEMENTS (starts); i++) {
    gchar *src = g_strdup_printf ("pattern=ball timestamp-offset=%" G_GUINT64_FORMAT, starts[i]);
    Run r = { 0 };

    run_simple (c, src, "", 30, &r);
    g_assert_cmpuint (g_array_index (r.in, InBuf, 0).pts, ==, starts[i]);
    check_unshifted (&r);
    run_clear (&r);
    g_free (src);
  }
}

/* A flushing seek: VA measures a new shift after the flush; the output still has the input's timestamps and
 * segment. */
static void
test_timestamps_seek (gconstpointer data)
{
  const Codec *c = data;
  gchar *desc;
  Run r = { 0 };
  const InBuf *last_in;
  const OutBuf *last_out;

  if (!require_va (c))
    return;
  desc = g_strdup_printf ("videotestsrc pattern=ball ! video/x-raw,width=%d,height=%d,framerate=%d/1 ! %s name=enc "
      "! fakesink name=sink", SW, SH, FPS, c->element);
  run_pipeline (c, desc, &r, 5 * GST_SECOND);
  g_assert_cmpuint (r.out_segs, >=, 2);
  g_assert_cmpuint (r.out_seg.start, ==, 5 * GST_SECOND);
  g_assert_true (gst_segment_is_equal (&r.in_seg, &r.out_seg));
  for (guint i = 0; i < r.out->len; i++) {
    const OutBuf *ob = &g_array_index (r.out, OutBuf, i);

    g_assert_cmpuint (ob->pts, ==, ob->dts);
    g_assert_cmpuint (ob->pts, <, 10 * GST_SECOND);
  }
  /* the last second is all there, as given */
  last_in = &g_array_index (r.in, InBuf, r.in->len - 1);
  last_out = &g_array_index (r.out, OutBuf, r.out->len - 1);
  g_assert_cmpuint (last_out->pts, ==, last_in->pts);
  g_assert_cmpuint (last_out->pts, >=, 5 * GST_SECOND + GST_SECOND - 2 * FRAME_DUR);
  g_free (desc);
  run_clear (&r);
}

/* Index of the input frame with PTS @pts, or -1. */
static gint
input_index (const Run * r, GstClockTime pts)
{
  for (guint i = 0; i < r->in->len; i++) {
    if (g_array_index (r->in, InBuf, i).pts == pts)
      return (gint) i;
  }
  return -1;
}

/* H.264 Main with B frames: VA's timestamps stay (ours: removing the shift would put the first DTS below 0); the
 * output PTS are the input's, reordered, plus the shift VA put on the segment, and DTS increases. */
static void
test_b_frames_timestamps (void)
{
  const Codec *c = &codecs[0];
  Run r = { 0 };
  GstClockTime shift;
  gboolean reordered = FALSE;
  guint64 seen = 0;

  if (!require_va (c))
    return;
  run_simple (c, "pattern=ball", "profile=2 num-B-Frames=2", 60, &r);
  g_assert_cmpuint (r.out->len, ==, 60);
  shift = r.out_seg.start - r.in_seg.start;
  g_assert_cmpuint (shift, ==, 1000 * HOUR);
  for (guint i = 0; i < r.out->len; i++) {
    const OutBuf *ob = &g_array_index (r.out, OutBuf, i);
    gint k = input_index (&r, ob->pts - shift);

    g_assert_cmpint (k, >=, 0);
    g_assert_cmpint (k, <, 64);
    g_assert_false (seen & (G_GUINT64_CONSTANT (1) << k));
    seen |= G_GUINT64_CONSTANT (1) << k;
    if (i > 0) {
      const OutBuf *prev = &g_array_index (r.out, OutBuf, i - 1);

      g_assert_cmpuint (ob->dts, >, prev->dts);
      reordered |= ob->pts < prev->pts;
    }
    g_assert_cmpuint (ob->dts, <=, ob->pts);
  }
  g_assert_true (reordered);
  run_clear (&r);
}

/* copy-meta: the input's batch meta reaches the output buffer of the same frame (with B frames too); without it
 * (the default, DeepStream 8.0 behaviour) no output has one. */
static void
check_copy_meta (const Codec * c, const gchar * props, gboolean copy)
{
  Run r = {.tag = TRUE };
  GstClockTime shift;

  run_simple (c, "pattern=ball", props, 40, &r);
  g_assert_cmpuint (r.out->len, ==, 40);
  shift = r.out_seg.start - r.in_seg.start;
  for (guint i = 0; i < r.out->len; i++) {
    const OutBuf *ob = &g_array_index (r.out, OutBuf, i);

    if (!copy) {
      g_assert_cmpint (ob->source_id, ==, -1);
      continue;
    }
    g_assert_cmpuint (ob->meta_frames, ==, 1);
    g_assert_cmpint (ob->source_id, ==, input_index (&r, ob->pts - shift));
  }
  run_clear (&r);
}

static void
test_copy_meta (gconstpointer data)
{
  const Codec *c = data;

  if (!require_va (c))
    return;
  check_copy_meta (c, "", FALSE);
  check_copy_meta (c, "copy-meta=true", TRUE);
  if (!is_hevc (c))
    check_copy_meta (c, "copy-meta=true profile=2 num-B-Frames=2", TRUE);
}

/* memory:RDS NV12 and RGBA, and system memory, all encode. */
static void
test_inputs (gconstpointer data)
{
  const Codec *c = data;
  static const gchar *const ins[] = {
    "! rdsvideoconvert ! video/x-raw(memory:RDS),format=NV12",
    "! rdsvideoconvert ! video/x-raw(memory:RDS),format=RGBA",
    ",format=RGBA",
    ",format=NV12",
  };

  if (!require_va (c))
    return;
  for (guint i = 0; i < G_N_ELEMENTS (ins); i++) {
    gchar *desc = g_strdup_printf ("videotestsrc num-buffers=10 ! video/x-raw,width=%d,height=%d,framerate=%d/1%s%s "
        "! %s name=enc ! fakesink name=sink", SW, SH, FPS, ins[i][0] == '!' ? " " : "", ins[i], c->element);
    Run r = { 0 };

    run_pipeline (c, desc, &r, GST_CLOCK_TIME_NONE);
    g_assert_cmpuint (r.out->len, ==, 10);
    g_assert_true (g_array_index (r.out, OutBuf, 0).idr);
    run_clear (&r);
    g_free (desc);
  }
}

/* A batch of two frames is an element error (ours: DeepStream 8.0 encodes the first frame only, silently). */
static void
test_batch_rejected (void)
{
  const Codec *c = &codecs[0];
  gchar *desc;
  GstElement *p;
  GstBus *bus;
  GstMessage *m;
  GError *err = NULL;

  if (!require_va (c))
    return;
  desc = g_strdup_printf ("rdsstreammux name=m batch-size=2 width=%d height=%d ! %s name=enc ! fakesink "
      "videotestsrc num-buffers=10 ! video/x-raw,width=%d,height=%d,framerate=%d/1 ! m.sink_0 "
      "videotestsrc num-buffers=10 ! video/x-raw,width=%d,height=%d,framerate=%d/1 ! m.sink_1", SW, SH, c->element,
      SW, SH, FPS, SW, SH, FPS);
  p = gst_parse_launch (desc, NULL);
  g_assert_nonnull (p);
  bus = gst_element_get_bus (p);
  gst_element_set_state (p, GST_STATE_PLAYING);
  m = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (m);
  g_assert_cmpint (GST_MESSAGE_TYPE (m), ==, GST_MESSAGE_ERROR);
  g_assert_cmpstr (GST_MESSAGE_SRC_NAME (m), ==, "enc");
  gst_message_parse_error (m, &err, NULL);
  g_assert_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_WRONG_TYPE);
  g_assert_nonnull (strstr (err->message, "one frame"));
  g_clear_error (&err);
  gst_message_unref (m);
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (p);
  g_free (desc);
}

/* DELTA_UNIT is clear on the IDR and I frames and set on every other frame. */
static void
test_delta_unit (gconstpointer data)
{
  const Codec *c = data;
  Run r = { 0 };
  guint intra = 0;

  if (!require_va (c))
    return;
  run_simple (c, "pattern=ball", "idrinterval=30 iframeinterval=10", 60, &r);
  g_assert_cmpuint (r.out->len, ==, 60);
  for (guint i = 0; i < r.out->len; i++) {
    const OutBuf *ob = &g_array_index (r.out, OutBuf, i);
    gboolean is_intra = ob->idr || ob->first_slice_type == RDS_TEST_H264_SLICE_I;

    intra += is_intra;
    g_assert_cmpint (ob->delta, ==, !is_intra);
  }
  g_assert_cmpuint (intra, ==, 6);      /* 2 IDR, 4 I */
  run_clear (&r);
}

/* A frame-rate change mid-stream: the output caps follow from the first frame at the new rate (ours: VA keeps the
 * old framerate in its caps). */
static void
test_framerate_change (gconstpointer data)
{
  const Codec *c = data;
  gchar *desc;
  Run r = { 0 };
  gint n = 0, d = 0;

  if (!require_va (c))
    return;
  desc = g_strdup_printf ("videotestsrc num-buffers=80 pattern=ball ! capsfilter name=cf caps=video/x-raw,width=%d,"
      "height=%d,framerate=30/1 ! %s name=enc ! fakesink name=sink", SW, SH, c->element);
  r.change_caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, SW, "height", G_TYPE_INT, SH,
      "framerate", GST_TYPE_FRACTION, 15, 1, NULL);
  r.change_at = 40;
  run_pipeline (c, desc, &r, GST_CLOCK_TIME_NONE);
  g_assert_cmpuint (r.out->len, ==, 80);
  /* no B frames: output i is input i */
  for (guint i = 0; i < r.out->len; i++)
    g_assert_cmpint (g_array_index (r.out, OutBuf, i).fps_n, ==, g_array_index (r.in, InBuf, i).fps_n);
  g_assert_cmpint (g_array_index (r.in, InBuf, 0).fps_n, ==, 30);
  g_assert_cmpint (g_array_index (r.in, InBuf, r.in->len - 1).fps_n, ==, 15);
  g_assert_true (gst_structure_get_fraction (gst_caps_get_structure (r.first_caps, 0), "framerate", &n, &d));
  g_assert_cmpint (n, ==, 30);
  g_assert_true (gst_structure_get_fraction (gst_caps_get_structure (r.last_caps, 0), "framerate", &n, &d));
  g_assert_cmpint (n, ==, 15);
  g_assert_cmpint (d, ==, 1);
  run_clear (&r);
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
  g_test_add_func ("/cpu/enc/configure", test_configure);
  g_test_add_func ("/cpu/enc/filler-only-dropped", test_filler_only_dropped);
  g_test_add_func ("/cpu/enc/segment-again", test_segment_again);
  g_test_add_func ("/gpu/enc/preferred-profile", test_preferred_profile);
  g_test_add_data_func ("/gpu/enc/smoke-h264", &codecs[0], test_smoke);
  g_test_add_data_func ("/gpu/enc/smoke-h265", &codecs[1], test_smoke);
  g_test_add_data_func ("/gpu/enc/set-while-playing-h264", &codecs[0], test_set_while_playing);
  g_test_add_data_func ("/gpu/enc/set-while-playing-h265", &codecs[1], test_set_while_playing);
  g_test_add_data_func ("/gpu/enc/gop-h264", &codecs[0], test_gop);
  g_test_add_data_func ("/gpu/enc/gop-h265", &codecs[1], test_gop);
  g_test_add_data_func ("/gpu/enc/profiles-h264", &codecs[0], test_profiles);
  g_test_add_data_func ("/gpu/enc/profiles-h265", &codecs[1], test_profiles);
  g_test_add_data_func ("/gpu/enc/downstream-level-h264", &codecs[0], test_downstream_level);
  g_test_add_data_func ("/gpu/enc/downstream-level-h265", &codecs[1], test_downstream_level);
  g_test_add_data_func ("/gpu/enc/cbr-h264", &codecs[0], test_cbr);
  g_test_add_data_func ("/gpu/enc/cbr-h265", &codecs[1], test_cbr);
  g_test_add_data_func ("/gpu/enc/live-bitrate-h264", &codecs[0], test_live_bitrate);
  g_test_add_data_func ("/gpu/enc/live-bitrate-h265", &codecs[1], test_live_bitrate);
  g_test_add_func ("/gpu/enc/b-frames-h264", test_b_frames);
  g_test_add_data_func ("/gpu/enc/inner-settings-h264", &codecs[0], test_inner_settings);
  g_test_add_data_func ("/gpu/enc/inner-settings-h265", &codecs[1], test_inner_settings);
  g_test_add_data_func ("/gpu/enc/no-filler-h264", &codecs[0], test_no_filler);
  g_test_add_data_func ("/gpu/enc/no-filler-h265", &codecs[1], test_no_filler);
  g_test_add_func ("/gpu/enc/h265-size", test_h265_size);
  g_test_add_data_func ("/gpu/enc/timestamps-h264", &codecs[0], test_timestamps);
  g_test_add_data_func ("/gpu/enc/timestamps-h265", &codecs[1], test_timestamps);
  g_test_add_data_func ("/gpu/enc/timestamps-seek-h264", &codecs[0], test_timestamps_seek);
  g_test_add_data_func ("/gpu/enc/timestamps-seek-h265", &codecs[1], test_timestamps_seek);
  g_test_add_func ("/gpu/enc/b-frames-timestamps-h264", test_b_frames_timestamps);
  g_test_add_data_func ("/gpu/enc/copy-meta-h264", &codecs[0], test_copy_meta);
  g_test_add_data_func ("/gpu/enc/copy-meta-h265", &codecs[1], test_copy_meta);
  g_test_add_data_func ("/gpu/enc/inputs-h264", &codecs[0], test_inputs);
  g_test_add_data_func ("/gpu/enc/inputs-h265", &codecs[1], test_inputs);
  g_test_add_func ("/gpu/enc/batch-rejected", test_batch_rejected);
  g_test_add_data_func ("/gpu/enc/delta-unit-h264", &codecs[0], test_delta_unit);
  g_test_add_data_func ("/gpu/enc/delta-unit-h265", &codecs[1], test_delta_unit);
  g_test_add_data_func ("/gpu/enc/framerate-change-h264", &codecs[0], test_framerate_change);
  g_test_add_data_func ("/gpu/enc/framerate-change-h265", &codecs[1], test_framerate_change);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
