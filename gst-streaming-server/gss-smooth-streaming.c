/* GStreamer Streaming Server
 * Copyright (C) 2013 Rdio Inc <ingestions@rd.io>
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

#include "config.h"

#include <gst/gst.h>
#include <gst/base/gstbytereader.h>

#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#include "gss-content.h"
#include "gss-ism-parser.h"
#include "gss-utils.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


typedef struct _GssISM GssISM;
typedef struct _GssISMLevel GssISMLevel;

struct _GssISM
{
  guint64 duration;

  int max_width;
  int max_height;

  int n_audio_levels;
  int n_video_levels;

  const char *video_codec_data;
  const char *audio_codec_data;
  gboolean playready;
  int audio_rate;

  GssISMLevel *audio_levels;
  GssISMLevel *video_levels;
};

struct _GssISMLevel
{
  const char *filename;

  int n_fragments;
  int bitrate;
  int video_width;
  int video_height;

  GssISMFragment *fragments;
};


GssISM *gss_ism_new (void);
void gss_ism_free (GssISM * ism);
GssISMLevel *gss_ism_get_level (GssISM * ism, gboolean video, guint64 bitrate);
GssISMFragment *gss_ism_level_get_fragment (GssISM * ism, GssISMLevel * level,
    guint64 timestamp);

GssISM *global_ism;


static void gss_smooth_streaming_resource_get_manifest (GssTransaction * t);
static void gss_smooth_streaming_resource_get_content (GssTransaction * t);

typedef struct _GssFileFragment GssFileFragment;

struct _GssFileFragment
{
  GssServer *server;
  SoupMessage *msg;
  SoupClientContext *client;

  int fd;
  guint64 offset;
  guint64 size;
};



#define SIZE 65536

static void
gss_file_fragment_wrote_chunk (SoupMessage * msg, GssFileFragment * ff)
{
  char *chunk;
  int len;
  guint64 size;

  if (ff->size == 0) {
    soup_message_body_complete (msg->response_body);
    return;
  }

  size = ff->size;
  if (size >= SIZE)
    size = SIZE;

  chunk = g_malloc (size);
  len = read (ff->fd, chunk, size);
  if (len < 0) {
    GST_ERROR ("read error");
  }
  ff->size -= len;
  soup_message_body_append (msg->response_body, SOUP_MEMORY_TAKE, chunk, len);
}

static void
gss_file_fragment_finished (SoupMessage * msg, GssFileFragment * ff)
{
  GST_ERROR ("done");
  close (ff->fd);
  g_free (ff);
}

static void
gss_file_fragment_serve_file (GssTransaction * t, const char *filename,
    guint64 offset, guint64 size)
{
  GssFileFragment *ff;
  off_t ret;

  ff = g_malloc0 (sizeof (GssFileFragment));
  ff->msg = t->msg;
  ff->client = t->client;
  ff->server = t->server;
  ff->size = size;

  ff->fd = open (filename, O_RDONLY);
  if (ff->fd < 0) {
    GST_WARNING ("file not found %s", filename);
    g_free (ff);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  ret = lseek (ff->fd, offset, SEEK_SET);
  if (ret < 0) {
    GST_WARNING ("failed to seek");
    g_free (ff);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  soup_message_set_status (t->msg, SOUP_STATUS_OK);

  soup_message_headers_set_encoding (t->msg->response_headers,
      SOUP_ENCODING_CHUNKED);

  soup_message_body_set_accumulate (t->msg->response_body, FALSE);

  /* FIXME check for zero size */
  gss_file_fragment_wrote_chunk (t->msg, ff);

  g_signal_connect (t->msg, "wrote-chunk",
      G_CALLBACK (gss_file_fragment_wrote_chunk), ff);
  g_signal_connect (t->msg, "finished", G_CALLBACK (gss_file_fragment_finished),
      ff);

}

static void
gss_file_fragment_serve_file_enc (GssTransaction * t, const char *filename,
    guint64 offset, guint64 size, guint64 enc_offset)
{
  off_t ret;
  guint8 *data;
  int fd;
  ssize_t n;
  //int eret;
  //gnutls_cipher_hd_t handle;
  //gnutls_datum_t key;
  //gnutls_datum_t iv;

  fd = open (filename, O_RDONLY);
  if (fd < 0) {
    GST_WARNING ("file not found %s", filename);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  ret = lseek (fd, offset, SEEK_SET);
  if (ret < 0) {
    GST_WARNING ("failed to seek");
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  data = g_malloc ((size + 127) & (~127));
  n = read (fd, data, size);
  if (n < size) {
    GST_WARNING ("read failed");
    g_free (data);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }
#if 0
  key.data = raw_key;
  key.size = 16;

  iv.data = raw_iv;
  iv.size = 16;

  gnutls_cipher_init (&handle, GNUTLS_CIPHER_AES_128_CBC, &key, &iv);

  GST_ERROR ("enc offset %" G_GUINT64_FORMAT " size %" G_GUINT64_FORMAT,
      enc_offset, size);
  eret = gnutls_cipher_encrypt (handle, data + enc_offset, size - enc_offset);
  GST_ERROR ("encrypt returned %d", eret);

  gnutls_cipher_deinit (handle);
#endif
#if 0
  {
    unsigned char raw_key[] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    unsigned char raw_iv[] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    unsigned char ecount_buf[16] = { 0 };
    unsigned int num = 0;
    AES_KEY key;

    AES_set_encrypt_key (raw_key, 16 * 8, &key);

    GST_ERROR ("enc offset %" G_GUINT64_FORMAT " size %" G_GUINT64_FORMAT,
        enc_offset, size);

    AES_ctr128_encrypt (data + enc_offset, data + enc_offset,
        size - enc_offset, &key, raw_iv, ecount_buf, &num);
    GST_ERROR ("num %d", num);
  }
#endif

  soup_message_set_status (t->msg, SOUP_STATUS_OK);
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_TAKE, data,
      size);
}

/* FIXME should be extracted from file */
typedef struct _ISMInfo ISMInfo;
struct _ISMInfo
{
  const char *filename;
  const char *mount;
  int width;
  int height;
  gboolean playready;
  int video_bitrate;
  const char *video_codec_data;
  int audio_bitrate;
  int audio_rate;
  const char *audio_codec_data;
};

ISMInfo ism_files[] = {
  {
        "SuperSpeedway_720_2962.ismv",
        "SuperSpeedwayPR",
        1280, 720, TRUE,
        2962000,
        "000000016764001FAC2CA5014016EFFC100010014808080A000007D200017700C100005A648000B4C9FE31C6080002D3240005A64FF18E1DA12251600000000168E9093525",
        128000, 44100, "1210",
      },
  {
        "SuperSpeedway/SuperSpeedway_720_2962.ismv",
        "SuperSpeedway",
        1280, 720, FALSE,
        2962000,
        "000000016764001FAC2CA5014016EFFC100010014808080A000007D200017700C100005A648000B4C9FE31C6080002D3240005A64FF18E1DA12251600000000168E9093525",
        128000, 44100, "1210",
      },
  {
        "boondocks-408.ismv",
        "boondocks",
        1024, 576, FALSE,
        2500000,
        "01640029ffe1001967640029ace50100126c0440000003005dcd650003c60c458001000468e923cb",
        128000, 48000, "1210",
      },
};


void
gss_smooth_streaming_setup (GssServer * server)
{
  int i;

  for (i = 0; i < 3; i++) {
    ISMInfo *info = &ism_files[i];
    GssISM *ism;
    GssISMParser *parser;
    int n_fragments;
    char *s;

    ism = gss_ism_new ();

    /* FIXME */
    ism->max_width = info->width;
    ism->max_height = info->height;

    ism->n_video_levels = 1;
    ism->video_levels = g_malloc0 (ism->n_video_levels * sizeof (GssISMLevel));
    ism->n_audio_levels = 1;
    ism->audio_levels = g_malloc0 (ism->n_audio_levels * sizeof (GssISMLevel));

    parser = gss_ism_parser_new ();
    gss_ism_parser_parse_file (parser, info->filename);

    ism->audio_levels[0].fragments =
        gss_ism_parser_get_fragments (parser, 1, &n_fragments);
    ism->audio_levels[0].n_fragments = n_fragments;

    ism->audio_levels[0].filename = g_strdup (info->filename);
    ism->audio_levels[0].bitrate = info->audio_bitrate;

    ism->video_levels[0].fragments =
        gss_ism_parser_get_fragments (parser, 2, &n_fragments);
    ism->video_levels[0].n_fragments = n_fragments;
    ism->video_levels[0].filename = g_strdup (info->filename);
    ism->video_levels[0].bitrate = info->video_bitrate;
    ism->video_levels[0].video_width = info->width;
    ism->video_levels[0].video_height = info->height;

    ism->duration =
        ism->video_levels[0].fragments[ism->video_levels[0].n_fragments -
        1].timestamp +
        ism->video_levels[0].fragments[ism->video_levels[0].n_fragments -
        1].duration;

    ism->video_codec_data = info->video_codec_data;
    ism->audio_codec_data = info->audio_codec_data;
    ism->audio_rate = info->audio_rate;
    ism->playready = info->playready;

    global_ism = ism;

    s = g_strdup_printf ("/%s/Manifest", info->mount);
    gss_server_add_resource (server, s, 0, "text/xml;charset=utf-8",
        gss_smooth_streaming_resource_get_manifest, NULL, NULL, global_ism);
    g_free (s);
    s = g_strdup_printf ("/%s/content", info->mount);
    gss_server_add_resource (server, s, 0, "video/mp4",
        gss_smooth_streaming_resource_get_content, NULL, NULL, global_ism);
    g_free (s);
  }
}

static void
gss_smooth_streaming_resource_get_manifest (GssTransaction * t)
{
  GssISM *ism = (GssISM *) t->resource->priv;
  GString *s = g_string_new ("");
  int i;

  t->s = s;

  GSS_A ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");

  GSS_P
      ("<SmoothStreamingMedia MajorVersion=\"2\" MinorVersion=\"1\" Duration=\"%"
      G_GUINT64_FORMAT "\">\n", ism->duration);
  GSS_P
      ("  <StreamIndex Type=\"video\" Name=\"video\" Chunks=\"%d\" QualityLevels=\"%d\" MaxWidth=\"%d\" MaxHeight=\"%d\" "
      "DisplayWidth=\"%d\" DisplayHeight=\"%d\" "
      "Url=\"content?stream=video&amp;bitrate={bitrate}&amp;start_time={start time}\">\n",
      ism->video_levels[0].n_fragments, ism->n_video_levels, ism->max_width,
      ism->max_height, ism->max_width, ism->max_height);
  /* also IsLive, LookaheadCount, DVRWindowLength */

  for (i = 0; i < ism->n_video_levels; i++) {
    GssISMLevel *level = &ism->video_levels[i];

    GSS_P ("    <QualityLevel Index=\"%d\" Bitrate=\"%d\" "
        "FourCC=\"H264\" MaxWidth=\"%d\" MaxHeight=\"%d\" "
        "CodecPrivateData=\"%s\" />\n", i, level->bitrate, level->video_width,
        level->video_height, ism->video_codec_data);
  }
  {
    GssISMLevel *level = &ism->video_levels[0];

    for (i = 0; i < ism->video_levels[0].n_fragments; i++) {
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) level->fragments[i].duration);
    }
  }
  GSS_A ("  </StreamIndex>\n");

  GSS_P ("  <StreamIndex Type=\"audio\" Index=\"0\" Name=\"audio\" "
      "Chunks=\"%d\" QualityLevels=\"%d\" "
      "Url=\"content?stream=audio&amp;bitrate={bitrate}&amp;start_time={start time}\">\n",
      ism->audio_levels[0].n_fragments, ism->n_audio_levels);
  for (i = 0; i < ism->n_video_levels; i++) {
    GssISMLevel *level = &ism->audio_levels[i];

    GSS_P ("    <QualityLevel FourCC=\"AACL\" Bitrate=\"%d\" "
        "SamplingRate=\"%d\" Channels=\"2\" BitsPerSample=\"16\" "
        "PacketSize=\"4\" AudioTag=\"255\" CodecPrivateData=\"%s\" />\n",
        level->bitrate, ism->audio_rate, ism->audio_codec_data);
  }
  {
    GssISMLevel *level = &ism->audio_levels[0];

    for (i = 0; i < ism->audio_levels[0].n_fragments; i++) {
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) level->fragments[i].duration);
    }
  }

  GSS_A ("  </StreamIndex>\n");
  if (ism->playready) {
    GSS_A ("<Protection>\n");
    GSS_A
        ("  <ProtectionHeader SystemID=\"9a04f079-9840-4286-ab92-e65be0885f95\">");
    {
      char *wrmheader;
      char *prot_header_base64;
      gunichar2 *utf16;
      glong items;
      int len;
      guchar *content;
      const char *la_url;

      la_url = "http://localhost:8080/rightsmanager.asmx";
      //la_url = "http://playready.directtaps.net/pr/svc/rightsmanager.asmx";

      /* this all needs to be on one line, to satisfy clients */
      wrmheader =
          g_strdup_printf
          ("<WRMHEADER xmlns=\"http://schemas.microsoft.com/DRM/2007/03/PlayReadyHeader\" "
          "version=\"4.0.0.0\">" "<DATA>" "<PROTECTINFO>" "<KEYLEN>16</KEYLEN>"
          "<ALGID>AESCTR</ALGID>" "</PROTECTINFO>"
          "<KID>AmfjCTOPbEOl3WD/5mcecA==</KID>"
          "<CHECKSUM>BGw1aYZ1YXM=</CHECKSUM>" "<CUSTOMATTRIBUTES>"
          "<IIS_DRM_VERSION>7.1.1064.0</IIS_DRM_VERSION>" "</CUSTOMATTRIBUTES>"
          "<LA_URL>%s</LA_URL>" "<DS_ID>AH+03juKbUGbHl1V/QIwRA==</DS_ID>"
          "</DATA>" "</WRMHEADER>", la_url);
      len = strlen (wrmheader);
      utf16 = g_utf8_to_utf16 (wrmheader, len, NULL, &items, NULL);

      content = g_malloc (items * sizeof (gunichar2) + 10);
      memcpy (content + 10, utf16, items * sizeof (gunichar2));
      GST_WRITE_UINT32_LE (content, items * sizeof (gunichar2) + 10);
      GST_WRITE_UINT16_LE (content + 4, 1);
      GST_WRITE_UINT16_LE (content + 6, 1);
      GST_WRITE_UINT16_LE (content + 8, items * sizeof (gunichar2));

      prot_header_base64 =
          g_base64_encode (content, items * sizeof (gunichar2) + 10);

      GSS_P ("%s", prot_header_base64);

      g_free (prot_header_base64);
      g_free (content);
      g_free (utf16);
    }
    GSS_A ("</ProtectionHeader>\n");
    GSS_A ("</Protection>\n");
  }
  GSS_A ("</SmoothStreamingMedia>\n");

}

static gboolean
g_hash_table_lookup_guint64 (GHashTable * hash, const char *key,
    guint64 * value)
{
  const char *s;
  char *end;

  s = g_hash_table_lookup (hash, key);
  if (s == NULL)
    return FALSE;

  if (s[0] == '\0')
    return FALSE;

  *value = g_ascii_strtoull (s, &end, 0);

  if (end[0] != '\0')
    return FALSE;
  return TRUE;
}

static void
gss_smooth_streaming_resource_get_content (GssTransaction * t)
{
  GssISM *ism = (GssISM *) t->resource->priv;
  guint64 start_time;
  const char *stream;
  guint64 bitrate;
  GssISMLevel *level;
  GssISMFragment *fragment;

  //GST_ERROR ("content request");

  if (t->query == NULL) {
    GST_ERROR ("no query");
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  stream = g_hash_table_lookup (t->query, "stream");

  if (stream == NULL ||
      !g_hash_table_lookup_guint64 (t->query, "start_time", &start_time) ||
      !g_hash_table_lookup_guint64 (t->query, "bitrate", &bitrate)) {
    GST_ERROR ("bad params");
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  if (strcmp (stream, "audio") != 0 && strcmp (stream, "video") != 0) {
    GST_ERROR ("bad stream %s", stream);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  level = gss_ism_get_level (ism, (stream[0] == 'v'), bitrate);
  if (level == NULL) {
    GST_ERROR ("no level for %s, %" G_GUINT64_FORMAT, stream, bitrate);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  fragment = gss_ism_level_get_fragment (ism, level, start_time);
  if (fragment == NULL) {
    GST_ERROR ("no fragment for %" G_GUINT64_FORMAT, start_time);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }
  //GST_ERROR ("frag %s %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
  //    level->filename, fragment->offset, fragment->size);
  gss_file_fragment_serve_file (t, level->filename, fragment->offset,
      fragment->size);
}

GssISM *
gss_ism_new (void)
{
  GssISM *ism;

  ism = g_malloc0 (sizeof (GssISM));

  return ism;

}

void
gss_ism_free (GssISM * ism)
{

  g_free (ism->audio_levels);
  g_free (ism->video_levels);
  g_free (ism);
}

GssISMLevel *
gss_ism_get_level (GssISM * ism, gboolean video, guint64 bitrate)
{
  int i;
  if (video) {
    for (i = 0; i < ism->n_video_levels; i++) {
      if (ism->video_levels[i].bitrate == bitrate) {
        return &ism->video_levels[i];
      }
    }
  } else {
    for (i = 0; i < ism->n_audio_levels; i++) {
      if (ism->audio_levels[i].bitrate == bitrate) {
        return &ism->audio_levels[i];
      }
    }
  }
  return NULL;
}

GssISMFragment *
gss_ism_level_get_fragment (GssISM * ism, GssISMLevel * level,
    guint64 timestamp)
{
  int i;
  for (i = 0; i < level->n_fragments; i++) {
    if (level->fragments[i].timestamp == timestamp) {
      return &level->fragments[i];
    }
  }
  return NULL;
}