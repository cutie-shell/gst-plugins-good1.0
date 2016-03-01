/* GStreamer unit tests for flvdemux
 *
 * Copyright (C) 2009 Tim-Philipp Müller  <tim centricular net>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>

#include <gst/gst.h>
#include <gst/tag/tag.h>

static void
pad_added_cb (GstElement * flvdemux, GstPad * pad, GstBin * pipeline)
{
  GstElement *sink;

  sink = gst_bin_get_by_name (pipeline, "fakesink");
  fail_unless (gst_element_link (flvdemux, sink));
  gst_object_unref (sink);

  gst_element_set_state (sink, GST_STATE_PAUSED);
}

static GstBusSyncReply
error_cb (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    const gchar *file = (const gchar *) user_data;
    GError *err = NULL;
    gchar *dbg = NULL;

    gst_message_parse_error (msg, &err, &dbg);
    g_error ("ERROR for %s: %s\n%s\n", file, err->message, dbg);
  }

  return GST_BUS_PASS;
}

static void
handoff_cb (GstElement * element, GstBuffer * buf, GstPad * pad,
    gint * p_counter)
{
  *p_counter += 1;
  GST_LOG ("counter = %d", *p_counter);
}

static void
process_file (const gchar * file, gboolean push_mode, gint repeat,
    gint num_buffers)
{
  GstElement *src, *sep, *sink, *flvdemux, *pipeline;
  GstBus *bus;
  gchar *path;
  gint counter;

  pipeline = gst_pipeline_new ("pipeline");
  fail_unless (pipeline != NULL, "Failed to create pipeline!");

  bus = gst_element_get_bus (pipeline);

  /* kids, don't use a sync handler for this at home, really; we do because
   * we just want to abort and nothing else */
  gst_bus_set_sync_handler (bus, error_cb, (gpointer) file, NULL);

  src = gst_element_factory_make ("filesrc", "filesrc");
  fail_unless (src != NULL, "Failed to create 'filesrc' element!");

  if (push_mode) {
    sep = gst_element_factory_make ("queue", "queue");
    fail_unless (sep != NULL, "Failed to create 'queue' element");
  } else {
    sep = gst_element_factory_make ("identity", "identity");
    fail_unless (sep != NULL, "Failed to create 'identity' element");
  }

  flvdemux = gst_element_factory_make ("flvdemux", "flvdemux");
  fail_unless (flvdemux != NULL, "Failed to create 'flvdemux' element!");

  sink = gst_element_factory_make ("fakesink", "fakesink");
  fail_unless (sink != NULL, "Failed to create 'fakesink' element!");

  g_object_set (sink, "signal-handoffs", TRUE, NULL);
  g_signal_connect (sink, "handoff", G_CALLBACK (handoff_cb), &counter);

  gst_bin_add_many (GST_BIN (pipeline), src, sep, flvdemux, sink, NULL);

  fail_unless (gst_element_link (src, sep));
  fail_unless (gst_element_link (sep, flvdemux));

  /* can't link flvdemux and sink yet, do that later */
  g_signal_connect (flvdemux, "pad-added", G_CALLBACK (pad_added_cb), pipeline);

  path = g_build_filename (GST_TEST_FILES_PATH, file, NULL);
  GST_LOG ("processing file '%s'", path);
  g_object_set (src, "location", path, NULL);

  do {
    GstStateChangeReturn state_ret;
    GstMessage *msg;

    GST_LOG ("repeat=%d", repeat);

    counter = 0;

    state_ret = gst_element_set_state (pipeline, GST_STATE_PAUSED);
    fail_unless (state_ret != GST_STATE_CHANGE_FAILURE);

    if (state_ret == GST_STATE_CHANGE_ASYNC) {
      GST_LOG ("waiting for pipeline to reach PAUSED state");
      state_ret = gst_element_get_state (pipeline, NULL, NULL, -1);
      fail_unless_equals_int (state_ret, GST_STATE_CHANGE_SUCCESS);
    }

    GST_LOG ("PAUSED, let's read all of it");

    state_ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
    fail_unless (state_ret != GST_STATE_CHANGE_FAILURE);

    msg = gst_bus_poll (bus, GST_MESSAGE_EOS, -1);
    fail_unless (msg != NULL, "Expected EOS message on bus! (%s)", file);

    gst_message_unref (msg);

    if (num_buffers >= 0) {
      fail_unless_equals_int (counter, num_buffers);
    }

    fail_unless_equals_int (gst_element_set_state (pipeline, GST_STATE_NULL),
        GST_STATE_CHANGE_SUCCESS);

    --repeat;
  } while (repeat > 0);

  gst_object_unref (bus);
  gst_object_unref (pipeline);

  g_free (path);
}

GST_START_TEST (test_reuse_pull)
{
  process_file ("pcm16sine.flv", FALSE, 3, 129);
  gst_task_cleanup_all ();
}

GST_END_TEST;

GST_START_TEST (test_reuse_push)
{
  process_file ("pcm16sine.flv", TRUE, 3, 129);
  gst_task_cleanup_all ();
}

GST_END_TEST;

static GstBuffer *
create_buffer (guint8 * data, gsize size)
{
  GstBuffer *buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
      data, size, 0, size, NULL, NULL);
  GST_BUFFER_PTS (buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_OFFSET (buf) = GST_BUFFER_OFFSET_NONE;
  GST_BUFFER_OFFSET_END (buf) = GST_BUFFER_OFFSET_NONE;
  return buf;
}

static void
flvdemux_pad_added (GstElement * flvdemux, GstPad * srcpad, GstHarness * h)
{
  (void) flvdemux;
  gst_harness_add_element_src_pad (h, srcpad);
}

GST_START_TEST (test_speex)
{
  guint8 flv_header0[] = {
    0x46, 0x4c, 0x56, 0x01, 0x04, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x00
  };

  guint8 flv_header1[] = {
    0x12, 0x00, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x0a, 0x6f, 0x6e,
    0x4d, 0x65, 0x74, 0x61, 0x44, 0x61, 0x74, 0x61,
    0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x0c, 0x61,
    0x75, 0x64, 0x69, 0x6f, 0x63, 0x6f, 0x64, 0x65,
    0x63, 0x69, 0x64, 0x00, 0x40, 0x26, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x6d, 0x65,
    0x74, 0x61, 0x64, 0x61, 0x74, 0x61, 0x63, 0x72,
    0x65, 0x61, 0x74, 0x6f, 0x72, 0x02, 0x00, 0x13,
    0x47, 0x53, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x65,
    0x72, 0x20, 0x46, 0x4c, 0x56, 0x20, 0x6d, 0x75,
    0x78, 0x65, 0x72, 0x00, 0x0c, 0x63, 0x72, 0x65,
    0x61, 0x74, 0x69, 0x6f, 0x6e, 0x64, 0x61, 0x74,
    0x65, 0x02, 0x00, 0x18, 0x57, 0x65, 0x64, 0x20,
    0x53, 0x65, 0x70, 0x20, 0x32, 0x33, 0x20, 0x31,
    0x30, 0x3a, 0x34, 0x39, 0x3a, 0x35, 0x36, 0x20,
    0x32, 0x30, 0x31, 0x35, 0x00, 0x00, 0x09, 0x00,
    0x00, 0x00, 0x87,
  };

  guint8 speex_header0[] = {
    0x08, 0x00, 0x00, 0x51, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xb2, 0x53, 0x70, 0x65, 0x65,
    0x78, 0x20, 0x20, 0x20, 0x31, 0x2e, 0x32, 0x72,
    0x63, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00,
    0x80, 0x3e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0x40, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c,
  };

  guint8 speex_header1[] = {
    0x08, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xb2, 0x1f, 0x00, 0x00, 0x00,
    0x45, 0x6e, 0x63, 0x6f, 0x64, 0x65, 0x64, 0x20,
    0x77, 0x69, 0x74, 0x68, 0x20, 0x47, 0x53, 0x74,
    0x72, 0x65, 0x61, 0x6d, 0x65, 0x72, 0x20, 0x53,
    0x70, 0x65, 0x65, 0x78, 0x65, 0x6e, 0x63, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x34,
  };

  guint8 buffer[] = {
    0x08, 0x00, 0x00, 0x47, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xb2, 0x36, 0x9d, 0x1b, 0x9a,
    0x20, 0x00, 0x01, 0x68, 0xe8, 0xe8, 0xe8, 0xe8,
    0xe8, 0xe8, 0xe8, 0x84, 0x00, 0xb4, 0x74, 0x74,
    0x74, 0x74, 0x74, 0x74, 0x74, 0x42, 0x00, 0x5a,
    0x3a, 0x3a, 0x3a, 0x3a, 0x3a, 0x3a, 0x3a, 0x21,
    0x00, 0x2d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d,
    0x1d, 0x1b, 0x3b, 0x60, 0xab, 0xab, 0xab, 0xab,
    0xab, 0x0a, 0xba, 0xba, 0xba, 0xba, 0xb0, 0xab,
    0xab, 0xab, 0xab, 0xab, 0x0a, 0xba, 0xba, 0xba,
    0xba, 0xb7, 0x00, 0x00, 0x00, 0x52,
  };

  GstHarness *h = gst_harness_new_with_padnames ("flvdemux", "sink", NULL);
  gst_harness_set_src_caps_str (h, "video/x-flv");

  g_signal_connect (h->element, "pad-added",
      G_CALLBACK (flvdemux_pad_added), h);

  gst_harness_push (h, create_buffer (flv_header0, sizeof (flv_header0)));
  gst_harness_push (h, create_buffer (flv_header1, sizeof (flv_header1)));
  gst_harness_push (h, create_buffer (speex_header0, sizeof (speex_header0)));
  gst_harness_push (h, create_buffer (speex_header1, sizeof (speex_header1)));
  gst_harness_push (h, create_buffer (buffer, sizeof (buffer)));

  {
    GstCaps *caps;
    const GstStructure *s;
    const GValue *streamheader;
    const GValue *header;
    const GValue *vorbiscomment;
    GstBuffer *buf;
    GstTagList *list;
    gint rate;
    gint channels;

    caps = gst_pad_get_current_caps (h->sinkpad);
    s = gst_caps_get_structure (caps, 0);

    fail_unless (gst_structure_has_name (s, "audio/x-speex"));

    streamheader = gst_structure_get_value (s, "streamheader");
    fail_unless (streamheader != NULL);
    fail_unless (G_VALUE_HOLDS (streamheader, GST_TYPE_ARRAY));
    fail_unless_equals_int (2, gst_value_array_get_size (streamheader));

    header = gst_value_array_get_value (streamheader, 0);
    fail_unless (header != NULL);
    fail_unless (G_VALUE_HOLDS (header, GST_TYPE_BUFFER));
    buf = gst_value_get_buffer (header);

    vorbiscomment = gst_value_array_get_value (streamheader, 1);
    fail_unless (header != NULL);
    fail_unless (G_VALUE_HOLDS (header, GST_TYPE_BUFFER));
    buf = gst_value_get_buffer (vorbiscomment);
    list = gst_tag_list_from_vorbiscomment_buffer (buf, NULL, 0, NULL);
    fail_unless (list != NULL);
    gst_tag_list_unref (list);

    gst_structure_get_int (s, "rate", &rate);
    fail_unless_equals_int (16000, rate);

    gst_structure_get_int (s, "channels", &channels);
    fail_unless_equals_int (1, channels);

    gst_caps_unref (caps);
  }

  /* we should have gotten 2x speex-headers, and one encoded buffer */
  fail_unless_equals_int (3, gst_harness_buffers_in_queue (h));

  gst_harness_teardown (h);
}

GST_END_TEST;

static Suite *
flvdemux_suite (void)
{
  Suite *s = suite_create ("flvdemux");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_reuse_push);
  tcase_add_test (tc_chain, test_reuse_pull);

  tcase_add_test (tc_chain, test_speex);

  return s;
}

GST_CHECK_MAIN (flvdemux)
