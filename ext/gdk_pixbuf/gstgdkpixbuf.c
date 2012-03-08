/* GStreamer GdkPixbuf-based image decoder
 * Copyright (C) 1999-2001 Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) 2003 David A. Schleef <ds@schleef.org>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

#include "gstgdkpixbuf.h"
#include "gstgdkpixbufoverlay.h"
#include "gstgdkpixbufsink.h"
#include "pixbufscale.h"

GST_DEBUG_CATEGORY_STATIC (gst_gdk_pixbuf_debug);
#define GST_CAT_DEFAULT gst_gdk_pixbuf_debug

enum
{
  ARG_0,
};

static GstStaticPadTemplate gst_gdk_pixbuf_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/png; "
        /* "image/jpeg; " disabled because we can't handle MJPEG */
        "image/gif; "
        "image/x-icon; "
        "application/x-navi-animation; "
        "image/x-cmu-raster; "
        "image/x-sun-raster; "
        "image/x-pixmap; "
        "image/tiff; "
        "image/x-portable-anymap; "
        "image/x-portable-bitmap; "
        "image/x-portable-graymap; "
        "image/x-portable-pixmap; "
        "image/bmp; "
        "image/x-bmp; "
        "image/x-MS-bmp; "
        "image/vnd.wap.wbmp; " "image/x-bitmap; " "image/x-tga; "
        "image/x-pcx; image/svg; image/svg+xml")
    );

static GstStaticPadTemplate gst_gdk_pixbuf_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("RGB") "; "
        GST_VIDEO_CAPS_MAKE ("RGBA"))
    );

static GstStateChangeReturn
gst_gdk_pixbuf_change_state (GstElement * element, GstStateChange transition);
static GstFlowReturn gst_gdk_pixbuf_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer);
static gboolean gst_gdk_pixbuf_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);

#ifdef enable_typefind
static void gst_gdk_pixbuf_type_find (GstTypeFind * tf, gpointer ignore);
#endif

#define gst_gdk_pixbuf_parent_class parent_class
G_DEFINE_TYPE (GstGdkPixbuf, gst_gdk_pixbuf, GST_TYPE_ELEMENT);

static gboolean
gst_gdk_pixbuf_sink_setcaps (GstGdkPixbuf * filter, GstCaps * caps)
{
  const GValue *framerate;
  GstStructure *s;

  s = gst_caps_get_structure (caps, 0);

  if ((framerate = gst_structure_get_value (s, "framerate")) != NULL) {
    filter->in_fps_n = gst_value_get_fraction_numerator (framerate);
    filter->in_fps_d = gst_value_get_fraction_denominator (framerate);
    GST_DEBUG_OBJECT (filter, "got framerate of %d/%d fps => packetized mode",
        filter->in_fps_n, filter->in_fps_d);
  } else {
    filter->in_fps_n = 0;
    filter->in_fps_d = 1;
    GST_DEBUG_OBJECT (filter, "no framerate, assuming single image");
  }

  return TRUE;
}

static GstCaps *
gst_gdk_pixbuf_get_capslist (GstCaps * filter)
{
  GSList *slist;
  GSList *slist0;
  GstCaps *capslist = NULL;
  GstCaps *return_caps = NULL;
  GstCaps *tmpl_caps;

  capslist = gst_caps_new_empty ();
  slist0 = gdk_pixbuf_get_formats ();

  for (slist = slist0; slist; slist = g_slist_next (slist)) {
    GdkPixbufFormat *pixbuf_format;
    char **mimetypes;
    char **mimetype;

    pixbuf_format = slist->data;
    mimetypes = gdk_pixbuf_format_get_mime_types (pixbuf_format);

    for (mimetype = mimetypes; *mimetype; mimetype++) {
      gst_caps_append_structure (capslist, gst_structure_new_empty (*mimetype));
    }
    g_strfreev (mimetypes);
  }
  g_slist_free (slist0);

  tmpl_caps = gst_static_caps_get (&gst_gdk_pixbuf_sink_template.static_caps);
  return_caps = gst_caps_intersect (capslist, tmpl_caps);

  gst_caps_unref (tmpl_caps);
  gst_caps_unref (capslist);

  if (filter && return_caps) {
    GstCaps *temp;

    temp = gst_caps_intersect (return_caps, filter);
    gst_caps_unref (return_caps);
    return_caps = temp;
  }

  return return_caps;
}

static gboolean
gst_gdk_pixbuf_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter, *caps;

      gst_query_parse_caps (query, &filter);
      caps = gst_gdk_pixbuf_get_capslist (filter);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);

      res = TRUE;
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }
  return res;
}


/* initialize the plugin's class */
static void
gst_gdk_pixbuf_class_init (GstGdkPixbufClass * klass)
{
  GstElementClass *gstelement_class;

  gstelement_class = (GstElementClass *) klass;

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_gdk_pixbuf_change_state);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_gdk_pixbuf_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_gdk_pixbuf_sink_template));
  gst_element_class_set_static_metadata (gstelement_class,
      "GdkPixbuf image decoder", "Codec/Decoder/Image",
      "Decodes images in a video stream using GdkPixbuf",
      "David A. Schleef <ds@schleef.org>, Renato Filho <renato.filho@indt.org.br>");
}

static void
gst_gdk_pixbuf_init (GstGdkPixbuf * filter)
{
  filter->sinkpad =
      gst_pad_new_from_static_template (&gst_gdk_pixbuf_sink_template, "sink");
  gst_pad_set_query_function (filter->sinkpad,
      GST_DEBUG_FUNCPTR (gst_gdk_pixbuf_sink_query));
  gst_pad_set_chain_function (filter->sinkpad,
      GST_DEBUG_FUNCPTR (gst_gdk_pixbuf_chain));
  gst_pad_set_event_function (filter->sinkpad,
      GST_DEBUG_FUNCPTR (gst_gdk_pixbuf_sink_event));
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad =
      gst_pad_new_from_static_template (&gst_gdk_pixbuf_src_template, "src");
  gst_pad_use_fixed_caps (filter->srcpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->last_timestamp = GST_CLOCK_TIME_NONE;
  filter->pixbuf_loader = NULL;
}

static gboolean
gst_gdk_pixbuf_setup_pool (GstGdkPixbuf * filter, GstVideoInfo * info)
{
  GstCaps *target;
  GstQuery *query;
  GstBufferPool *pool;
  GstStructure *config;
  guint size, min, max;

  target = gst_pad_get_current_caps (filter->srcpad);

  /* try to get a bufferpool now */
  /* find a pool for the negotiated caps now */
  query = gst_query_new_allocation (target, TRUE);

  if (!gst_pad_peer_query (filter->srcpad, query)) {
    /* not a problem, we use the query defaults */
    GST_DEBUG_OBJECT (filter, "ALLOCATION query failed");
  }

  if (gst_query_get_n_allocation_pools (query) > 0) {
    /* we got configuration from our peer, parse them */
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
  } else {
    pool = NULL;
    size = info->size;
    min = max = 0;
  }

  if (pool == NULL) {
    /* we did not get a pool, make one ourselves then */
    pool = gst_buffer_pool_new ();
  }

  /* and configure */
  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, target, size, min, max);
  gst_buffer_pool_set_config (pool, config);

  if (filter->pool) {
    gst_buffer_pool_set_active (filter->pool, FALSE);
    gst_object_unref (filter->pool);
  }
  filter->pool = pool;

  /* and activate */
  gst_buffer_pool_set_active (filter->pool, TRUE);

  gst_caps_unref (target);

  return TRUE;
}

static GstFlowReturn
gst_gdk_pixbuf_flush (GstGdkPixbuf * filter)
{
  GstBuffer *outbuf;
  GdkPixbuf *pixbuf;
  int y;
  guint8 *out_pix;
  guint8 *in_pix;
  int in_rowstride, out_rowstride;
  GstFlowReturn ret;
  GstCaps *caps = NULL;
  gint width, height;
  gint n_channels;
  GstVideoFrame frame;

  pixbuf = gdk_pixbuf_loader_get_pixbuf (filter->pixbuf_loader);
  if (pixbuf == NULL)
    goto no_pixbuf;

  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);

  if (GST_VIDEO_INFO_FORMAT (&filter->info) == GST_VIDEO_FORMAT_UNKNOWN) {
    GstVideoInfo info;
    GstVideoFormat fmt;

    GST_DEBUG ("Set size to %dx%d", width, height);

    n_channels = gdk_pixbuf_get_n_channels (pixbuf);
    switch (n_channels) {
      case 3:
        fmt = GST_VIDEO_FORMAT_RGB;
        break;
      case 4:
        fmt = GST_VIDEO_FORMAT_RGBA;
        break;
      default:
        goto channels_not_supported;
    }


    gst_video_info_init (&info);
    gst_video_info_set_format (&info, fmt, width, height);
    info.fps_n = filter->in_fps_n;
    info.fps_d = filter->in_fps_d;
    caps = gst_video_info_to_caps (&info);

    filter->info = info;

    gst_pad_set_caps (filter->srcpad, caps);
    gst_caps_unref (caps);

    gst_gdk_pixbuf_setup_pool (filter, &info);
  }

  ret = gst_buffer_pool_acquire_buffer (filter->pool, &outbuf, NULL);
  if (ret != GST_FLOW_OK)
    goto no_buffer;

  GST_BUFFER_TIMESTAMP (outbuf) = filter->last_timestamp;
  GST_BUFFER_DURATION (outbuf) = GST_CLOCK_TIME_NONE;

  in_pix = gdk_pixbuf_get_pixels (pixbuf);
  in_rowstride = gdk_pixbuf_get_rowstride (pixbuf);

  gst_video_frame_map (&frame, &filter->info, outbuf, GST_MAP_WRITE);
  out_pix = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);
  out_rowstride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);

  for (y = 0; y < height; y++) {
    memcpy (out_pix, in_pix, width * GST_VIDEO_FRAME_COMP_PSTRIDE (&frame, 0));
    in_pix += in_rowstride;
    out_pix += out_rowstride;
  }

  gst_video_frame_unmap (&frame);

  GST_DEBUG ("pushing... %d bytes", gst_buffer_get_size (outbuf));
  ret = gst_pad_push (filter->srcpad, outbuf);

  if (ret != GST_FLOW_OK)
    GST_DEBUG_OBJECT (filter, "flow: %s", gst_flow_get_name (ret));

  return ret;

  /* ERRORS */
no_pixbuf:
  {
    GST_ELEMENT_ERROR (filter, STREAM, DECODE, (NULL), ("error geting pixbuf"));
    return GST_FLOW_ERROR;
  }
channels_not_supported:
  {
    GST_ELEMENT_ERROR (filter, STREAM, DECODE, (NULL),
        ("%d channels not supported", n_channels));
    return GST_FLOW_ERROR;
  }
no_buffer:
  {
    GST_DEBUG ("Failed to create outbuffer - %s", gst_flow_get_name (ret));
    return ret;
  }
}

static gboolean
gst_gdk_pixbuf_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstFlowReturn res = GST_FLOW_OK;
  gboolean ret = TRUE, forward = TRUE;
  GstGdkPixbuf *pixbuf;

  pixbuf = GST_GDK_PIXBUF (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_gdk_pixbuf_sink_setcaps (pixbuf, caps);
      forward = FALSE;
      break;
    }
    case GST_EVENT_EOS:
      if (pixbuf->pixbuf_loader != NULL) {
        gdk_pixbuf_loader_close (pixbuf->pixbuf_loader, NULL);
        res = gst_gdk_pixbuf_flush (pixbuf);
        g_object_unref (G_OBJECT (pixbuf->pixbuf_loader));
        pixbuf->pixbuf_loader = NULL;
        /* as long as we don't have flow returns for event functions we need
         * to post an error here, or the application might never know that
         * things failed */
        if (res != GST_FLOW_OK && res != GST_FLOW_FLUSHING) {
          GST_ELEMENT_ERROR (pixbuf, STREAM, FAILED, (NULL),
              ("Flow: %s", gst_flow_get_name (res)));
          forward = FALSE;
          ret = FALSE;
        }
      }
      break;
    case GST_EVENT_SEGMENT:
    case GST_EVENT_FLUSH_STOP:
      if (pixbuf->pixbuf_loader != NULL) {
        gdk_pixbuf_loader_close (pixbuf->pixbuf_loader, NULL);
        g_object_unref (G_OBJECT (pixbuf->pixbuf_loader));
        pixbuf->pixbuf_loader = NULL;
      }
      break;
    default:
      break;
  }
  if (forward) {
    ret = gst_pad_event_default (pad, parent, event);
  } else {
    gst_event_unref (event);
  }
  return ret;
}

static GstFlowReturn
gst_gdk_pixbuf_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstGdkPixbuf *filter;
  GstFlowReturn ret = GST_FLOW_OK;
  GError *error = NULL;
  GstClockTime timestamp;
  GstMapInfo map;

  filter = GST_GDK_PIXBUF (parent);

  timestamp = GST_BUFFER_TIMESTAMP (buf);

  if (GST_CLOCK_TIME_IS_VALID (timestamp))
    filter->last_timestamp = timestamp;

  GST_LOG_OBJECT (filter, "buffer with ts: %" GST_TIME_FORMAT,
      GST_TIME_ARGS (timestamp));

  if (filter->pixbuf_loader == NULL)
    filter->pixbuf_loader = gdk_pixbuf_loader_new ();

  gst_buffer_map (buf, &map, GST_MAP_READ);

  GST_LOG_OBJECT (filter, "Writing buffer size %d", (gint) map.size);
  if (!gdk_pixbuf_loader_write (filter->pixbuf_loader, map.data, map.size,
          &error))
    goto error;

  /* packetised mode? */
  if (filter->in_fps_n != 0) {
    gdk_pixbuf_loader_close (filter->pixbuf_loader, NULL);
    ret = gst_gdk_pixbuf_flush (filter);
    g_object_unref (filter->pixbuf_loader);
    filter->pixbuf_loader = NULL;
  }

  gst_buffer_unmap (buf, &map);
  gst_buffer_unref (buf);

  return ret;

  /* ERRORS */
error:
  {
    GST_ELEMENT_ERROR (filter, STREAM, DECODE, (NULL),
        ("gdk_pixbuf_loader_write error: %s", error->message));
    g_error_free (error);
    gst_buffer_unmap (buf, &map);
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
}

static GstStateChangeReturn
gst_gdk_pixbuf_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstGdkPixbuf *dec = GST_GDK_PIXBUF (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      /* default to single image mode, setcaps function might not be called */
      dec->in_fps_n = 0;
      dec->in_fps_d = 1;
      gst_video_info_init (&dec->info);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      dec->in_fps_n = 0;
      dec->in_fps_d = 0;
      if (dec->pool) {
        gst_buffer_pool_set_active (dec->pool, FALSE);
        gst_object_replace ((GstObject **) & dec->pool, NULL);
      }
      break;
    default:
      break;
  }

  return ret;
}

#define GST_GDK_PIXBUF_TYPE_FIND_SIZE 1024

#ifdef enable_typefind
static void
gst_gdk_pixbuf_type_find (GstTypeFind * tf, gpointer ignore)
{
  guint8 *data;
  GdkPixbufLoader *pixbuf_loader;
  GdkPixbufFormat *format;

  data = gst_type_find_peek (tf, 0, GST_GDK_PIXBUF_TYPE_FIND_SIZE);
  if (data == NULL)
    return;

  GST_DEBUG ("creating new loader");

  pixbuf_loader = gdk_pixbuf_loader_new ();

  gdk_pixbuf_loader_write (pixbuf_loader, data, GST_GDK_PIXBUF_TYPE_FIND_SIZE,
      NULL);

  format = gdk_pixbuf_loader_get_format (pixbuf_loader);

  if (format != NULL) {
    GstCaps *caps;
    gchar **p;
    gchar **mlist = gdk_pixbuf_format_get_mime_types (format);

    for (p = mlist; *p; ++p) {
      GST_DEBUG ("suggesting mime type %s", *p);
      caps = gst_caps_new_simple (*p, NULL);
      gst_type_find_suggest (tf, GST_TYPE_FIND_MINIMUM, caps);
      gst_caps_free (caps);
    }
    g_strfreev (mlist);
  }

  GST_DEBUG ("closing pixbuf loader, hope it doesn't hang ...");
  /* librsvg 2.4.x has a bug where it triggers an endless loop in trying
     to close a gzip that's not an svg; fixed upstream but no good way
     to work around it */
  gdk_pixbuf_loader_close (pixbuf_loader, NULL);
  GST_DEBUG ("closed pixbuf loader");
  g_object_unref (G_OBJECT (pixbuf_loader));
}
#endif

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and pad templates
 * register the features
 */
static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_gdk_pixbuf_debug, "gdkpixbuf", 0,
      "gdk pixbuf loader");

  if (!gst_element_register (plugin, "gdkpixbufdec", GST_RANK_SECONDARY,
          GST_TYPE_GDK_PIXBUF))
    return FALSE;

#ifdef enable_typefind
  gst_type_find_register (plugin, "image/*", GST_RANK_MARGINAL,
      gst_gdk_pixbuf_type_find, NULL, GST_CAPS_ANY, NULL);
#endif

  if (!gst_element_register (plugin, "gdkpixbufoverlay", GST_RANK_NONE,
          GST_TYPE_GDK_PIXBUF_OVERLAY))
    return FALSE;

  if (!gst_element_register (plugin, "gdkpixbufsink", GST_RANK_NONE,
          GST_TYPE_GDK_PIXBUF_SINK))
    return FALSE;

  if (!pixbufscale_init (plugin))
    return FALSE;

  /* plugin initialisation succeeded */
  return TRUE;
}


/* this is the structure that gst-register looks for
 * so keep the name plugin_desc, or you cannot get your plug-in registered */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    gdkpixbuf,
    "GdkPixbuf-based image decoder, scaler and sink",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)