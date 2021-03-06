/* GStreamer Streaming Server
 * Copyright (C) 2008-2013 Entropy Wave Inc <info@entropywave.com>
 * Copyright (C) 2008-2013 David Schleef <ds@schleef.org>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <stdlib.h>



typedef struct _GssTranscoder GssTranscoder;
struct _GssTranscoder
{
  GstElement *pipeline;
  GstBus *bus;
  GMainLoop *main_loop;

  GstElement *source_element;
  GstElement *sink_element;

  gboolean paused_for_buffering;
  guint timer_id;
  gboolean audio_linked;

  /* properties */
  char *uri;
  int crop_top;
  int crop_bottom;
  int crop_left;
  int crop_right;
};

typedef struct _GssProfile GssProfile;
struct _GssProfile
{
  int width;
  int height;
  int total_bitrate;
  int audio_bitrate;
};

GssProfile profiles[] = {
  //{1920, 1080, 5000000, 128000},
  {1280, 720, 2500000, 128000},
  //{640, 360, 600000, 128000}
};

GssTranscoder *gss_transcoder_new (void);
void gss_transcoder_free (GssTranscoder * transcoder);
void gss_transcoder_create_pipeline (GssTranscoder * transcoder);
void gss_transcoder_start (GssTranscoder * transcoder);
void gss_transcoder_stop (GssTranscoder * transcoder);

static gboolean gss_transcoder_handle_message (GstBus * bus,
    GstMessage * message, gpointer data);
static gboolean onesecond_timer (gpointer priv);
static void pad_added (GstElement * element, GstPad * pad, gpointer user_data);
static GstPadProbeReturn
segment_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data);
static void parse_crop_string (GssTranscoder * transcoder, const char *s);

GstPad *gst_element_get_sink_pad (GstElement * element);

gboolean verbose;
gboolean use_deinterlace = FALSE;
gboolean use_ivtc = FALSE;
char *crop_string = "0,0,0,0";
double clip_start = 0.0;
double clip_end = 0.0;
char *output = "out";
char *audio_channel_string = "0,1";
gboolean use_stretch = FALSE;


static GOptionEntry entries[] = {
  {"verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Be verbose", NULL},

  {"deinterlace", 0, 0, G_OPTION_ARG_NONE, &use_deinterlace,
      "Enable deinterlacing filter", NULL},
  {"ivtc", 0, 0, G_OPTION_ARG_NONE, &use_ivtc, "Enable inverse telecine filter",
      NULL},
  {"stretch", 0, 0, G_OPTION_ARG_NONE, &use_stretch,
      "Enable stretching instead of letterboxing", NULL},

  {"crop", 0, 0, G_OPTION_ARG_STRING, &crop_string,
      "Crop [top,bottom,left,right]", NULL},
  {"clip-start", 0, 0, G_OPTION_ARG_DOUBLE, &clip_start, "Clip start", NULL},
  {"clip-end", 0, 0, G_OPTION_ARG_DOUBLE, &clip_end, "Clip end", NULL},

  {"audio-channels", 0, 0, G_OPTION_ARG_STRING, &audio_channel_string,
      "Audio channels [0,1]", NULL},

  {"output", 0, 0, G_OPTION_ARG_STRING, &output, "Output base filename", NULL},

  {NULL}
};

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context;
  GssTranscoder *transcoder;
  GMainLoop *main_loop;

  context = g_option_context_new ("- FIXME");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_print ("option parsing failed: %s\n", error->message);
    exit (1);
  }
  g_option_context_free (context);

  transcoder = gss_transcoder_new ();

  parse_crop_string (transcoder, crop_string);

  if (argc > 1) {
    gchar *uri;
    if (gst_uri_is_valid (argv[1])) {
      uri = g_strdup (argv[1]);
    } else {
      uri = gst_filename_to_uri (argv[1], NULL);
    }
    g_print ("URI is %s\n", uri);
    transcoder->uri = uri;
    gss_transcoder_create_pipeline (transcoder);
  } else {
    g_print ("no input filename\n");
    exit (1);
  }


  gss_transcoder_start (transcoder);

  main_loop = g_main_loop_new (NULL, TRUE);
  transcoder->main_loop = main_loop;

  g_main_loop_run (main_loop);

  exit (0);
}

static void
parse_crop_string (GssTranscoder * transcoder, const char *s)
{
  char **parts;

  parts = g_strsplit (s, ",", 5);
  if (parts[0]) {
    transcoder->crop_top = strtoul (parts[0], NULL, 10);
    if (parts[1]) {
      transcoder->crop_bottom = strtoul (parts[1], NULL, 10);
      if (parts[2]) {
        transcoder->crop_left = strtoul (parts[2], NULL, 10);
        if (parts[3]) {
          transcoder->crop_right = strtoul (parts[3], NULL, 10);
        }
      }
    }
  }
  g_strfreev (parts);
}

GssTranscoder *
gss_transcoder_new (void)
{
  GssTranscoder *transcoder;

  transcoder = g_new0 (GssTranscoder, 1);

  return transcoder;
}

void
gss_transcoder_free (GssTranscoder * transcoder)
{
  if (transcoder->source_element) {
    gst_object_unref (transcoder->source_element);
    transcoder->source_element = NULL;
  }
  if (transcoder->sink_element) {
    gst_object_unref (transcoder->sink_element);
    transcoder->sink_element = NULL;
  }

  if (transcoder->pipeline) {
    gst_element_set_state (transcoder->pipeline, GST_STATE_NULL);
    gst_object_unref (transcoder->pipeline);
    transcoder->pipeline = NULL;
  }
  g_free (transcoder);
}

void
gss_transcoder_create_pipeline (GssTranscoder * transcoder)
{
  GstElement *pipeline;
  GError *error = NULL;
  GString *s;
  gboolean is_http;
  GstElement *e;
  GstPad *pad;
  int i;

  s = g_string_new ("");

  if (g_str_has_prefix (transcoder->uri, "http://") ||
      g_str_has_prefix (transcoder->uri, "https://")) {
    is_http = TRUE;
  } else {
    is_http = FALSE;
  }

  if (is_http) {
    g_string_append (s, "souphttpsrc name=src ! ");
  } else {
    g_string_append (s, "filesrc name=src ! ");
  }

  g_string_append (s, "decodebin name=dec ");

  g_string_append (s, "queue name=vqueue ! ");
  g_string_append (s, "videosegmentclip name=vclip ! ");
  if (use_deinterlace) {
    g_string_append (s, "yadif ! ");
  } else if (use_ivtc) {
    g_string_append (s, "ivtc ! ");
  }

  g_string_append (s, "videoconvert ! ");
  g_string_append (s, "video/x-raw,format=I420 ! ");
  g_string_append_printf (s, "videocrop top=%d bottom=%d left=%d right=%d ! ",
      transcoder->crop_top, transcoder->crop_bottom,
      transcoder->crop_left, transcoder->crop_right);
  g_string_append (s, "queue ! ");
  g_string_append (s, "tee name=vtee ");

  g_string_append (s, "queue name=aqueue max-size-time=5000000000 "
      "max-size-bytes=0 max-size-buffers=0 ! ");
  g_string_append (s, "audiosegmentclip name=aclip ! ");
  g_string_append (s, "audioconvert ! ");
  g_string_append (s, "audio/x-raw,channels=2 ! ");
  g_string_append (s, "audioresample ! ");
  g_string_append (s, "audio/x-raw,rate=48000 ! ");
  g_string_append_printf (s, "neroaacenc bitrate=%d ! ",
      profiles[0].audio_bitrate);
  g_string_append (s, "queue ! ");
  g_string_append (s, "tee name=atee ");

  for (i = 0; i < G_N_ELEMENTS (profiles); i++) {
    g_string_append (s, "vtee. ! queue ! ");
    if (use_stretch) {
      g_string_append (s, "videoscale add-borders=false ! ");
    } else {
      g_string_append (s, "videoscale add-borders=true ! ");
    }
    g_string_append_printf (s,
        "video/x-raw,pixel-aspect-ratio=1/1,width=%d,height=%d ! ",
        profiles[i].width, profiles[i].height);
    g_string_append_printf (s,
        "x264enc name=venc%d bitrate=%d tune=zerolatency ! ", i,
        (profiles[i].total_bitrate - profiles[i].audio_bitrate) / 1000);
    g_string_append_printf (s, "queue ! ");
    g_string_append_printf (s, "mp4mux name=mux%d ! ", i);
    g_string_append_printf (s, "watchdog timeout=5000 ! ");
    g_string_append_printf (s, "filesink name=sink%d location=out-%d.mp4 ",
        i, i);

    g_string_append (s, "atee. ! queue ! ");
    g_string_append_printf (s, "mux%d. ", i);

  }

  g_print ("pipeline: %s\n", s->str);

  pipeline = (GstElement *) gst_parse_launch (s->str, &error);
  if (pipeline == NULL) {
    g_print ("pipeline parsing error: %s\n", error->message);
    g_string_free (s, FALSE);
    gst_object_unref (pipeline);
    return;
  }
  g_string_free (s, FALSE);

  transcoder->pipeline = pipeline;

  gst_pipeline_set_auto_flush_bus (GST_PIPELINE (pipeline), FALSE);
  transcoder->bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (transcoder->bus, gss_transcoder_handle_message,
      transcoder);

  e = gst_bin_get_by_name (GST_BIN (pipeline), "src");
  g_assert (e);
  if (is_http) {
    g_object_set (e, "location", transcoder->uri, NULL);
  } else {
    g_object_set (e, "location", transcoder->uri + 7, NULL);
  }
  g_object_unref (e);

  e = gst_bin_get_by_name (GST_BIN (pipeline), "dec");
  g_assert (e);
  g_signal_connect (e, "pad-added", G_CALLBACK (pad_added), transcoder);

  g_object_unref (e);

  e = gst_bin_get_by_name (GST_BIN (pipeline), "aqueue");
  g_assert (e);
  pad = gst_element_get_static_pad (e, "src");
  g_assert (pad);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      segment_probe, transcoder, NULL);
  g_object_unref (pad);
  g_object_unref (e);

  e = gst_bin_get_by_name (GST_BIN (pipeline), "vqueue");
  g_assert (e);
  pad = gst_element_get_static_pad (e, "src");
  g_assert (pad);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      segment_probe, transcoder, NULL);
  g_object_unref (pad);
  g_object_unref (e);

  transcoder->source_element = gst_bin_get_by_name (GST_BIN (pipeline), "src");
  g_print ("source_element is %p\n", transcoder->source_element);

}

static void
pad_added (GstElement * element, GstPad * pad, gpointer user_data)
{
  GssTranscoder *transcoder = (GssTranscoder *) user_data;
  GstCaps *caps;
  GstStructure *structure;
  GstPadLinkReturn ret;

  caps = gst_pad_get_current_caps (pad);
  if (caps == NULL) {
    GST_ERROR ("current caps is NULL");
    g_assert (caps);
    return;
  }

  GST_ERROR ("new pad: %" GST_PTR_FORMAT, caps);
  structure = gst_caps_get_structure (caps, 0);
  if (gst_structure_has_name (structure, "video/x-raw")) {
    GstElement *e;
    GstPad *sinkpad;

    e = gst_bin_get_by_name (GST_BIN (transcoder->pipeline), "vqueue");
    g_assert (e);
    sinkpad = gst_element_get_static_pad (e, "sink");
    g_assert (sinkpad);

    ret = gst_pad_link (pad, sinkpad);
    if (GST_PAD_LINK_FAILED (ret)) {
      GST_ERROR ("link failed");
    }

    g_object_unref (sinkpad);
    g_object_unref (e);
  } else if (gst_structure_has_name (structure, "audio/x-raw")) {
    if (!transcoder->audio_linked) {
      GstElement *e;
      GstPad *sinkpad;

      transcoder->audio_linked = TRUE;

      e = gst_bin_get_by_name (GST_BIN (transcoder->pipeline), "aqueue");
      g_assert (e);
      sinkpad = gst_element_get_static_pad (e, "sink");
      g_assert (sinkpad);

      ret = gst_pad_link (pad, sinkpad);
      if (GST_PAD_LINK_FAILED (ret)) {
        GST_ERROR ("link failed");
      }

      g_object_unref (sinkpad);
      g_object_unref (e);
    } else {
      GstElement *bin;
      GstPad *sinkpad;
      gboolean bret;

      bin =
          gst_parse_bin_from_description
          ("queue max-size-time=5000000000 max-size-bytes=0 "
          "max-size-buffers=0 ! fakesink", TRUE, NULL);
      gst_bin_add (GST_BIN (transcoder->pipeline), bin);
      bret = gst_element_sync_state_with_parent (bin);
      if (!bret) {
        GST_ERROR ("could not sync with parent");
      }

      sinkpad = gst_element_get_sink_pad (bin);
      g_assert (sinkpad);
      ret = gst_pad_link (pad, sinkpad);
      if (GST_PAD_LINK_FAILED (ret)) {
        GST_ERROR ("link failed");
      }

      g_object_unref (sinkpad);
    }

  }
}

static GstPadProbeReturn
segment_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstEvent *event;

  event = gst_pad_probe_info_get_event (info);
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEGMENT:
    {
      const GstSegment *segment;
      GstSegment newsegment;
      GstEvent *new_event;

      gst_event_parse_segment (event, &segment);

      if (segment->start == clip_start * GST_SECOND &&
          (clip_end == 0.0 || segment->stop == clip_end * GST_SECOND)) {
        return GST_PAD_PROBE_OK;
      }

      GST_ERROR ("fixing up segment event %" G_GUINT64_FORMAT ":%"
          G_GUINT64_FORMAT, segment->start, segment->stop);
      gst_segment_copy_into (segment, &newsegment);
      newsegment.start = clip_start * GST_SECOND;
      if (clip_end != 0.0) {
        newsegment.stop = clip_end * GST_SECOND;
      }
      new_event = gst_event_new_segment (&newsegment);
      gst_pad_push_event (pad, new_event);
      return GST_PAD_PROBE_DROP;
    }
      break;
    default:
      break;
  }

  return GST_PAD_PROBE_OK;
}

void
gss_transcoder_start (GssTranscoder * transcoder)
{
  gst_element_set_state (transcoder->pipeline, GST_STATE_READY);

  transcoder->timer_id = g_timeout_add (1000, onesecond_timer, transcoder);
}

void
gss_transcoder_stop (GssTranscoder * transcoder)
{
  gst_element_set_state (transcoder->pipeline, GST_STATE_NULL);

  g_source_remove (transcoder->timer_id);
}

static void
gss_transcoder_handle_eos (GssTranscoder * transcoder)
{
  gss_transcoder_stop (transcoder);
}

static void
gss_transcoder_handle_error (GssTranscoder * transcoder, GError * error,
    const char *debug)
{
  g_print ("error: %s (%s)\n", error->message, debug);
  gss_transcoder_stop (transcoder);
}

static void
gss_transcoder_handle_warning (GssTranscoder * transcoder, GError * error,
    const char *debug)
{
  g_print ("warning: %s\n", error->message);
}

static void
gss_transcoder_handle_info (GssTranscoder * transcoder, GError * error,
    const char *debug)
{
  g_print ("info: %s\n", error->message);
}

static void
gss_transcoder_handle_null_to_ready (GssTranscoder * transcoder)
{
  gst_element_set_state (transcoder->pipeline, GST_STATE_PAUSED);

}

static void
gss_transcoder_handle_ready_to_paused (GssTranscoder * transcoder)
{
  if (!transcoder->paused_for_buffering) {
    GST_ERROR ("set playing");
    gst_element_set_state (transcoder->pipeline, GST_STATE_PLAYING);
  }
}

static void
gss_transcoder_handle_paused_to_playing (GssTranscoder * transcoder)
{
  GST_ERROR ("PLAYING");

}

static void
gss_transcoder_handle_playing_to_paused (GssTranscoder * transcoder)
{

}

static void
gss_transcoder_handle_paused_to_ready (GssTranscoder * transcoder)
{

}

static void
gss_transcoder_handle_ready_to_null (GssTranscoder * transcoder)
{
  g_main_loop_quit (transcoder->main_loop);

}


static gboolean
gss_transcoder_handle_message (GstBus * bus, GstMessage * message,
    gpointer data)
{
  GssTranscoder *transcoder = (GssTranscoder *) data;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_EOS:
      gss_transcoder_handle_eos (transcoder);
      break;
    case GST_MESSAGE_ERROR:
    {
      GError *error = NULL;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      gss_transcoder_handle_error (transcoder, error, debug);
    }
      break;
    case GST_MESSAGE_WARNING:
    {
      GError *error = NULL;
      gchar *debug;

      gst_message_parse_warning (message, &error, &debug);
      gss_transcoder_handle_warning (transcoder, error, debug);
    }
      break;
    case GST_MESSAGE_INFO:
    {
      GError *error = NULL;
      gchar *debug;

      gst_message_parse_info (message, &error, &debug);
      gss_transcoder_handle_info (transcoder, error, debug);
    }
      break;
    case GST_MESSAGE_TAG:
    {
      GstTagList *tag_list;

      gst_message_parse_tag (message, &tag_list);
      if (verbose)
        g_print ("tag\n");
    }
      break;
    case GST_MESSAGE_STATE_CHANGED:
    {
      GstState oldstate, newstate, pending;

      gst_message_parse_state_changed (message, &oldstate, &newstate, &pending);
      if (GST_ELEMENT (message->src) == transcoder->pipeline) {
        if (verbose)
          g_print ("state change from %s to %s\n",
              gst_element_state_get_name (oldstate),
              gst_element_state_get_name (newstate));
        switch (GST_STATE_TRANSITION (oldstate, newstate)) {
          case GST_STATE_CHANGE_NULL_TO_READY:
            gss_transcoder_handle_null_to_ready (transcoder);
            break;
          case GST_STATE_CHANGE_READY_TO_PAUSED:
            gss_transcoder_handle_ready_to_paused (transcoder);
            break;
          case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            gss_transcoder_handle_paused_to_playing (transcoder);
            break;
          case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            gss_transcoder_handle_playing_to_paused (transcoder);
            break;
          case GST_STATE_CHANGE_PAUSED_TO_READY:
            gss_transcoder_handle_paused_to_ready (transcoder);
            break;
          case GST_STATE_CHANGE_READY_TO_NULL:
            gss_transcoder_handle_ready_to_null (transcoder);
            break;
          default:
            if (verbose)
              g_print ("unknown state change from %s to %s\n",
                  gst_element_state_get_name (oldstate),
                  gst_element_state_get_name (newstate));
        }
      }
    }
      break;
    case GST_MESSAGE_BUFFERING:
    {
      int percent;
      gst_message_parse_buffering (message, &percent);
      //g_print("buffering %d\n", percent);
      if (!transcoder->paused_for_buffering && percent < 100) {
        g_print ("pausing for buffing\n");
        transcoder->paused_for_buffering = TRUE;
        gst_element_set_state (transcoder->pipeline, GST_STATE_PAUSED);
      } else if (transcoder->paused_for_buffering && percent == 100) {
        g_print ("unpausing for buffing\n");
        transcoder->paused_for_buffering = FALSE;
        gst_element_set_state (transcoder->pipeline, GST_STATE_PLAYING);
      }
    }
      break;
    case GST_MESSAGE_LATENCY:
    {
      GST_ERROR ("latency message");
    }
      break;
    case GST_MESSAGE_STATE_DIRTY:
    case GST_MESSAGE_CLOCK_PROVIDE:
    case GST_MESSAGE_CLOCK_LOST:
    case GST_MESSAGE_NEW_CLOCK:
    case GST_MESSAGE_STRUCTURE_CHANGE:
    case GST_MESSAGE_STREAM_STATUS:
      break;
    case GST_MESSAGE_STEP_DONE:
    case GST_MESSAGE_APPLICATION:
    case GST_MESSAGE_ELEMENT:
    case GST_MESSAGE_SEGMENT_START:
    case GST_MESSAGE_SEGMENT_DONE:
    case GST_MESSAGE_DURATION:
    case GST_MESSAGE_ASYNC_START:
    case GST_MESSAGE_ASYNC_DONE:
    case GST_MESSAGE_REQUEST_STATE:
    case GST_MESSAGE_STEP_START:
    case GST_MESSAGE_QOS:
    default:
      if (verbose) {
        g_print ("message: %s\n", GST_MESSAGE_TYPE_NAME (message));
      }
      break;
  }

  return TRUE;
}



static gboolean
onesecond_timer (gpointer priv)
{
  //GssTranscoder *transcoder = (GssTranscoder *)priv;

  g_print (".\n");

  return TRUE;
}



/* helper functions */

#if 0
gboolean
have_element (const gchar * element_name)
{
  GstPluginFeature *feature;

  feature = gst_default_registry_find_feature (element_name,
      GST_TYPE_ELEMENT_FACTORY);
  if (feature) {
    g_object_unref (feature);
    return TRUE;
  }
  return FALSE;
}
#endif

GstPad *
gst_element_get_sink_pad (GstElement * element)
{
  GstIterator *iter;
  GstPad *pad = NULL;
  gboolean done = FALSE;
  GValue item = { 0 };

  iter = gst_element_iterate_sink_pads (element);

  while (!done) {
    switch (gst_iterator_next (iter, &item)) {
      case GST_ITERATOR_OK:
        pad = g_value_dup_object (&item);
        g_value_reset (&item);
        done = TRUE;
        break;
      case GST_ITERATOR_RESYNC:
        pad = NULL;
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_ERROR:
        done = TRUE;
        break;
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (iter);

  return pad;
}
