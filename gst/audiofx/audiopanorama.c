/*
 * GStreamer
 * Copyright (C) 2006 Stefan Kost <ensonic@users.sf.net>
 * Copyright (C) 2006 Sebastian Dröge <slomo@circular-chaos.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-audiopanorama
 *
 * Stereo panorama effect with controllable pan position. One can choose between the default psychoacoustic panning method,
 * which keeps the same perceived loudness, and a simple panning method that just controls the volume on one channel.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch audiotestsrc wave=saw ! audiopanorama panorama=-1.00 ! alsasink
 * gst-launch filesrc location="melo1.ogg" ! oggdemux ! vorbisdec ! audioconvert ! audiopanorama panorama=-1.00 ! alsasink
 * gst-launch audiotestsrc wave=saw ! audioconvert ! audiopanorama panorama=-1.00 ! audioconvert ! alsasink
 * gst-launch audiotestsrc wave=saw ! audioconvert ! audiopanorama method=simple panorama=-0.50 ! audioconvert ! alsasink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

#include "audiopanorama.h"

#define GST_CAT_DEFAULT gst_audio_panorama_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_PANORAMA,
  PROP_METHOD
};

enum
{
  METHOD_PSYCHOACOUSTIC = 0,
  METHOD_SIMPLE,
  NUM_METHODS
};

#define GST_TYPE_AUDIO_PANORAMA_METHOD (gst_audio_panorama_method_get_type ())
static GType
gst_audio_panorama_method_get_type (void)
{
  static GType gtype = 0;

  if (gtype == 0) {
    static const GEnumValue values[] = {
      {METHOD_PSYCHOACOUSTIC, "Psychoacoustic Panning (default)",
          "psychoacoustic"},
      {METHOD_SIMPLE, "Simple Panning", "simple"},
      {0, NULL, NULL}
    };

    gtype = g_enum_register_static ("GstAudioPanoramaMethod", values);
  }
  return gtype;
}

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) { " GST_AUDIO_NE (F32) ", " GST_AUDIO_NE (S16) "}, "
        "rate = (int) [ 1, MAX ], " "channels = (int) 1, "
        "layout = (string) interleaved;"
        "audio/x-raw, "
        "format = (string) { " GST_AUDIO_NE (F32) ", " GST_AUDIO_NE (S16) "}, "
        "rate = (int) [ 1, MAX ], " "channels = (int) 2, "
        "layout = (string) interleaved, " "channel-mask = (bitmask) 0x3")
    );

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) { " GST_AUDIO_NE (S32) ", " GST_AUDIO_NE (S16) "}, "
        "rate = (int) [ 1, MAX ], " "channels = (int) 2, "
        "layout = (string) interleaved, " "channel-mask = (bitmask)0x3")
    );

G_DEFINE_TYPE (GstAudioPanorama, gst_audio_panorama, GST_TYPE_BASE_TRANSFORM);

static void gst_audio_panorama_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_audio_panorama_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_audio_panorama_get_unit_size (GstBaseTransform * base,
    GstCaps * caps, gsize * size);
static GstCaps *gst_audio_panorama_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean gst_audio_panorama_set_caps (GstBaseTransform * base,
    GstCaps * incaps, GstCaps * outcaps);

static void gst_audio_panorama_transform_m2s_int (GstAudioPanorama * filter,
    gint16 * idata, gint16 * odata, guint num_samples);
static void gst_audio_panorama_transform_s2s_int (GstAudioPanorama * filter,
    gint16 * idata, gint16 * odata, guint num_samples);
static void gst_audio_panorama_transform_m2s_float (GstAudioPanorama * filter,
    gfloat * idata, gfloat * odata, guint num_samples);
static void gst_audio_panorama_transform_s2s_float (GstAudioPanorama * filter,
    gfloat * idata, gfloat * odata, guint num_samples);

static void gst_audio_panorama_transform_m2s_int_simple (GstAudioPanorama *
    filter, gint16 * idata, gint16 * odata, guint num_samples);
static void gst_audio_panorama_transform_s2s_int_simple (GstAudioPanorama *
    filter, gint16 * idata, gint16 * odata, guint num_samples);
static void gst_audio_panorama_transform_m2s_float_simple (GstAudioPanorama *
    filter, gfloat * idata, gfloat * odata, guint num_samples);
static void gst_audio_panorama_transform_s2s_float_simple (GstAudioPanorama *
    filter, gfloat * idata, gfloat * odata, guint num_samples);

static GstFlowReturn gst_audio_panorama_transform (GstBaseTransform * base,
    GstBuffer * inbuf, GstBuffer * outbuf);


/* Table with processing functions: [channels][format][method] */
static GstAudioPanoramaProcessFunc panorama_process_functions[2][2][2] = {
  {
        {(GstAudioPanoramaProcessFunc) gst_audio_panorama_transform_m2s_int,
              (GstAudioPanoramaProcessFunc)
            gst_audio_panorama_transform_m2s_int_simple},
        {(GstAudioPanoramaProcessFunc) gst_audio_panorama_transform_m2s_float,
              (GstAudioPanoramaProcessFunc)
            gst_audio_panorama_transform_m2s_float_simple}
      },
  {
        {(GstAudioPanoramaProcessFunc) gst_audio_panorama_transform_s2s_int,
              (GstAudioPanoramaProcessFunc)
            gst_audio_panorama_transform_s2s_int_simple},
        {(GstAudioPanoramaProcessFunc) gst_audio_panorama_transform_s2s_float,
              (GstAudioPanoramaProcessFunc)
            gst_audio_panorama_transform_s2s_float_simple}
      }
};

/* GObject vmethod implementations */

static void
gst_audio_panorama_class_init (GstAudioPanoramaClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  GST_DEBUG_CATEGORY_INIT (gst_audio_panorama_debug, "audiopanorama", 0,
      "audiopanorama element");

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_audio_panorama_set_property;
  gobject_class->get_property = gst_audio_panorama_get_property;

  g_object_class_install_property (gobject_class, PROP_PANORAMA,
      g_param_spec_float ("panorama", "Panorama",
          "Position in stereo panorama (-1.0 left -> 1.0 right)", -1.0, 1.0,
          0.0,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS));
  /**
   * GstAudioPanorama:method
   *
   * Panning method: psychoacoustic mode keeps the same perceived loudness,
   * while simple mode just controls the volume of one channel. It's merely
   * a matter of taste which method should be chosen. 
   *
   * Since: 0.10.6
   **/
  g_object_class_install_property (gobject_class, PROP_METHOD,
      g_param_spec_enum ("method", "Panning method",
          "Psychoacoustic mode keeps same perceived loudness, "
          "simple mode just controls volume of one channel.",
          GST_TYPE_AUDIO_PANORAMA_METHOD, METHOD_PSYCHOACOUSTIC,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (gstelement_class, "Stereo positioning",
      "Filter/Effect/Audio",
      "Positions audio streams in the stereo panorama",
      "Stefan Kost <ensonic@users.sf.net>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));

  GST_BASE_TRANSFORM_CLASS (klass)->get_unit_size =
      GST_DEBUG_FUNCPTR (gst_audio_panorama_get_unit_size);
  GST_BASE_TRANSFORM_CLASS (klass)->transform_caps =
      GST_DEBUG_FUNCPTR (gst_audio_panorama_transform_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->set_caps =
      GST_DEBUG_FUNCPTR (gst_audio_panorama_set_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->transform =
      GST_DEBUG_FUNCPTR (gst_audio_panorama_transform);
}

static void
gst_audio_panorama_init (GstAudioPanorama * filter)
{

  filter->panorama = 0;
  filter->method = METHOD_PSYCHOACOUSTIC;
  gst_audio_info_init (&filter->info);
  filter->process = NULL;

  gst_base_transform_set_gap_aware (GST_BASE_TRANSFORM (filter), TRUE);
}

static gboolean
gst_audio_panorama_set_process_function (GstAudioPanorama * filter,
    GstAudioInfo * info)
{
  gint channel_index, format_index, method_index;
  const GstAudioFormatInfo *finfo = info->finfo;

  /* set processing function */
  channel_index = GST_AUDIO_INFO_CHANNELS (info) - 1;
  if (channel_index > 1 || channel_index < 0) {
    filter->process = NULL;
    return FALSE;
  }

  format_index = GST_AUDIO_FORMAT_INFO_IS_FLOAT (finfo) ? 1 : 0;

  method_index = filter->method;
  if (method_index >= NUM_METHODS || method_index < 0)
    method_index = METHOD_PSYCHOACOUSTIC;

  filter->process =
      panorama_process_functions[channel_index][format_index][method_index];
  return TRUE;
}

static void
gst_audio_panorama_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAudioPanorama *filter = GST_AUDIO_PANORAMA (object);

  switch (prop_id) {
    case PROP_PANORAMA:
      filter->panorama = g_value_get_float (value);
      break;
    case PROP_METHOD:
      filter->method = g_value_get_enum (value);
      gst_audio_panorama_set_process_function (filter, &filter->info);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_audio_panorama_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAudioPanorama *filter = GST_AUDIO_PANORAMA (object);

  switch (prop_id) {
    case PROP_PANORAMA:
      g_value_set_float (value, filter->panorama);
      break;
    case PROP_METHOD:
      g_value_set_enum (value, filter->method);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstBaseTransform vmethod implementations */

static gboolean
gst_audio_panorama_get_unit_size (GstBaseTransform * base, GstCaps * caps,
    gsize * size)
{
  GstAudioInfo info;

  g_assert (size);

  if (!gst_audio_info_from_caps (&info, caps))
    return FALSE;

  *size = GST_AUDIO_INFO_BPF (&info);

  return TRUE;
}

static GstCaps *
gst_audio_panorama_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *res;
  GstStructure *structure;

  /* transform caps gives one single caps so we can just replace
   * the channel property with our range. */
  res = gst_caps_copy (caps);
  structure = gst_caps_get_structure (res, 0);
  if (direction == GST_PAD_SRC) {
    GST_INFO ("allow 1-2 channels");
    gst_structure_set (structure, "channels", GST_TYPE_INT_RANGE, 1, 2, NULL);
  } else {
    GST_INFO ("allow 2 channels");
    gst_structure_set (structure, "channels", G_TYPE_INT, 2, NULL);
  }

  return res;
}

static gboolean
gst_audio_panorama_set_caps (GstBaseTransform * base, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstAudioPanorama *filter = GST_AUDIO_PANORAMA (base);
  GstAudioInfo info;

  /*GST_INFO ("incaps are %" GST_PTR_FORMAT, incaps); */
  if (!gst_audio_info_from_caps (&info, incaps))
    goto no_format;

  GST_DEBUG ("try to process %d input with %d channels",
      GST_AUDIO_INFO_FORMAT (&info), GST_AUDIO_INFO_CHANNELS (&info));

  if (!gst_audio_panorama_set_process_function (filter, &info))
    goto no_format;

  filter->info = info;

  return TRUE;

no_format:
  {
    GST_DEBUG ("invalid caps");
    return FALSE;
  }
}

/* psychoacoustic processing functions */
static void
gst_audio_panorama_transform_m2s_int (GstAudioPanorama * filter, gint16 * idata,
    gint16 * odata, guint num_samples)
{
  guint i;
  gdouble val;
  glong lval, rval;
  gdouble rpan, lpan;

  /* pan:  -1.0  0.0  1.0
   * lpan:  1.0  0.5  0.0  
   * rpan:  0.0  0.5  1.0
   *
   * FIXME: we should use -3db (1/sqtr(2)) for 50:50
   */
  rpan = (gdouble) (filter->panorama + 1.0) / 2.0;
  lpan = 1.0 - rpan;

  for (i = 0; i < num_samples; i++) {
    val = (gdouble) * idata++;

    lval = (glong) (val * lpan);
    rval = (glong) (val * rpan);

    *odata++ = (gint16) CLAMP (lval, G_MININT16, G_MAXINT16);
    *odata++ = (gint16) CLAMP (rval, G_MININT16, G_MAXINT16);
  }
}

static void
gst_audio_panorama_transform_s2s_int (GstAudioPanorama * filter, gint16 * idata,
    gint16 * odata, guint num_samples)
{
  guint i;
  glong lval, rval;
  gdouble lival, rival;
  gdouble lrpan, llpan, rrpan, rlpan;

  /* pan:  -1.0  0.0  1.0
   * llpan: 1.0  1.0  0.0
   * lrpan: 1.0  0.0  0.0
   * rrpan: 0.0  1.0  1.0
   * rlpan: 0.0  0.0  1.0
   */
  if (filter->panorama > 0) {
    rlpan = (gdouble) filter->panorama;
    llpan = 1.0 - rlpan;
    lrpan = 0.0;
    rrpan = 1.0;
  } else {
    rrpan = (gdouble) (1.0 + filter->panorama);
    lrpan = 1.0 - rrpan;
    rlpan = 0.0;
    llpan = 1.0;
  }

  for (i = 0; i < num_samples; i++) {
    lival = (gdouble) * idata++;
    rival = (gdouble) * idata++;

    lval = lival * llpan + rival * lrpan;
    rval = lival * rlpan + rival * rrpan;

    *odata++ = (gint16) CLAMP (lval, G_MININT16, G_MAXINT16);
    *odata++ = (gint16) CLAMP (rval, G_MININT16, G_MAXINT16);
  }
}

static void
gst_audio_panorama_transform_m2s_float (GstAudioPanorama * filter,
    gfloat * idata, gfloat * odata, guint num_samples)
{
  guint i;
  gfloat val;
  gdouble rpan, lpan;

  /* pan:  -1.0  0.0  1.0
   * lpan:  1.0  0.5  0.0  
   * rpan:  0.0  0.5  1.0
   *
   * FIXME: we should use -3db (1/sqtr(2)) for 50:50
   */
  rpan = (gdouble) (filter->panorama + 1.0) / 2.0;
  lpan = 1.0 - rpan;

  for (i = 0; i < num_samples; i++) {
    val = *idata++;

    *odata++ = val * lpan;
    *odata++ = val * rpan;
  }
}

static void
gst_audio_panorama_transform_s2s_float (GstAudioPanorama * filter,
    gfloat * idata, gfloat * odata, guint num_samples)
{
  guint i;
  gfloat lival, rival;
  gdouble lrpan, llpan, rrpan, rlpan;

  /* pan:  -1.0  0.0  1.0
   * llpan: 1.0  1.0  0.0
   * lrpan: 1.0  0.0  0.0
   * rrpan: 0.0  1.0  1.0
   * rlpan: 0.0  0.0  1.0
   */
  if (filter->panorama > 0) {
    rlpan = (gdouble) filter->panorama;
    llpan = 1.0 - rlpan;
    lrpan = 0.0;
    rrpan = 1.0;
  } else {
    rrpan = (gdouble) (1.0 + filter->panorama);
    lrpan = 1.0 - rrpan;
    rlpan = 0.0;
    llpan = 1.0;
  }

  for (i = 0; i < num_samples; i++) {
    lival = *idata++;
    rival = *idata++;

    *odata++ = lival * llpan + rival * lrpan;
    *odata++ = lival * rlpan + rival * rrpan;
  }
}

/* simple processing functions */
static void
gst_audio_panorama_transform_m2s_int_simple (GstAudioPanorama * filter,
    gint16 * idata, gint16 * odata, guint num_samples)
{
  guint i;
  gdouble pan;
  glong lval, rval;

  if (filter->panorama > 0.0) {
    pan = 1.0 - filter->panorama;
    for (i = 0; i < num_samples; i++) {
      rval = *idata++;
      lval = (glong) ((gdouble) rval * pan);

      *odata++ = (gint16) CLAMP (lval, G_MININT16, G_MAXINT16);
      *odata++ = (gint16) rval;
    }
  } else {
    pan = 1.0 + filter->panorama;
    for (i = 0; i < num_samples; i++) {
      lval = *idata++;
      rval = (glong) ((gdouble) lval * pan);

      *odata++ = (gint16) lval;
      *odata++ = (gint16) CLAMP (rval, G_MININT16, G_MAXINT16);
    }
  }
}

static void
gst_audio_panorama_transform_s2s_int_simple (GstAudioPanorama * filter,
    gint16 * idata, gint16 * odata, guint num_samples)
{
  guint i;
  glong lval, rval;
  gdouble lival, rival, pan;

  if (filter->panorama > 0.0) {
    pan = 1.0 - filter->panorama;
    for (i = 0; i < num_samples; i++) {
      lival = (gdouble) * idata++;
      rival = (gdouble) * idata++;

      lval = (glong) (lival * pan);
      rval = (glong) rival;

      *odata++ = (gint16) CLAMP (lval, G_MININT16, G_MAXINT16);
      *odata++ = (gint16) rval;
    }
  } else {
    pan = 1.0 + filter->panorama;
    for (i = 0; i < num_samples; i++) {
      lival = (gdouble) * idata++;
      rival = (gdouble) * idata++;

      lval = (glong) lival;
      rval = (glong) (rival * pan);

      *odata++ = (gint16) lval;
      *odata++ = (gint16) CLAMP (rval, G_MININT16, G_MAXINT16);
    }
  }
}

static void
gst_audio_panorama_transform_m2s_float_simple (GstAudioPanorama * filter,
    gfloat * idata, gfloat * odata, guint num_samples)
{
  guint i;
  gfloat val, pan;

  if (filter->panorama > 0.0) {
    pan = 1.0 - filter->panorama;
    for (i = 0; i < num_samples; i++) {
      val = *idata++;

      *odata++ = val * pan;
      *odata++ = val;
    }
  } else {
    pan = 1.0 + filter->panorama;
    for (i = 0; i < num_samples; i++) {
      val = *idata++;

      *odata++ = val;
      *odata++ = val * pan;
    }
  }
}

static void
gst_audio_panorama_transform_s2s_float_simple (GstAudioPanorama * filter,
    gfloat * idata, gfloat * odata, guint num_samples)
{
  guint i;
  gfloat lival, rival, pan;

  if (filter->panorama > 0.0) {
    pan = 1.0 - filter->panorama;
    for (i = 0; i < num_samples; i++) {
      lival = *idata++;
      rival = *idata++;

      *odata++ = lival * pan;
      *odata++ = rival;
    }
  } else {
    pan = 1.0 + filter->panorama;
    for (i = 0; i < num_samples; i++) {
      lival = *idata++;
      rival = *idata++;

      *odata++ = lival;
      *odata++ = rival * pan;
    }
  }
}

/* this function does the actual processing
 */
static GstFlowReturn
gst_audio_panorama_transform (GstBaseTransform * base, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstAudioPanorama *filter = GST_AUDIO_PANORAMA (base);
  GstClockTime timestamp, stream_time;
  GstMapInfo inmap, outmap;

  timestamp = GST_BUFFER_TIMESTAMP (inbuf);
  stream_time =
      gst_segment_to_stream_time (&base->segment, GST_FORMAT_TIME, timestamp);

  GST_DEBUG_OBJECT (filter, "sync to %" GST_TIME_FORMAT,
      GST_TIME_ARGS (timestamp));

  if (GST_CLOCK_TIME_IS_VALID (stream_time))
    gst_object_sync_values (GST_OBJECT (filter), stream_time);

  gst_buffer_map (inbuf, &inmap, GST_MAP_READ);
  gst_buffer_map (outbuf, &outmap, GST_MAP_WRITE);

  if (G_UNLIKELY (GST_BUFFER_FLAG_IS_SET (inbuf, GST_BUFFER_FLAG_GAP))) {
    GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_GAP);
    memset (outmap.data, 0, outmap.size);
  } else {
    /* output always stereo, input mono or stereo,
     * and info describes input format */
    guint num_samples = outmap.size / (2 * GST_AUDIO_INFO_BPS (&filter->info));

    filter->process (filter, inmap.data, outmap.data, num_samples);
  }

  gst_buffer_unmap (inbuf, &inmap);
  gst_buffer_unmap (outbuf, &outmap);

  return GST_FLOW_OK;
}