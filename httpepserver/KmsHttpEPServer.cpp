/*
 * Copyright (C) 2013 Kurento
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <time.h>
#include <libsoup/soup.h>
#include <uuid/uuid.h>
#include <string.h>
#include <gio/gio.h>
#include <nice/interfaces.h>

#include "KmsHttpEPServer.h"
#include "kms-enumtypes.h"
#include "kms-marshal.h"

#define OBJECT_NAME "HttpEPServer"

/* 36-byte string (plus tailing '\0') */
#define UUID_STR_SIZE 37

#define COOKIE_NAME "HttpEPCookie"

#define KEY_HTTP_EP_SERVER "kms-http-ep-server"
#define KEY_NEW_SAMPLE_HANDLER_ID "kms-new-sample-handler-id"
#define KEY_GOT_CHUNK_HANDLER_ID "kms-got-chunk-handler-id"
#define KEY_FINISHED_HANDLER_ID "kms-finish-handler-id"
#define KEY_EOS_HANDLER_ID "kms-eos-handler-id"
#define KEY_TIMEOUT_ID "kms-timeout-id"
#define KEY_FINISHED "kms-finish"
#define KEY_BOUNDARY "kms-boundary"
#define KEY_MESSAGE "kms-message"
#define KEY_COOKIE "kms-cookie"

#define KEY_PARAM_TIMEOUT "kms-param-timeout"
#define KEY_PARAM_LIFETIME "kms-param-lifetime"

#define GST_CAT_DEFAULT kms_http_ep_server_debug_category
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define KMS_HTTP_EP_SERVER_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), KMS_TYPE_HTTP_EP_SERVER, KmsHttpEPServerPrivate))
struct _KmsHttpEPServerPrivate {
  GHashTable *handlers;
  SoupServer *server;
  gchar *announcedAddr;
  gchar *iface;
  gint port;
  GRand *rand;
};

static GType http_t = G_TYPE_INVALID;

#define KMS_IS_EXPECTED_TYPE(obj, objtype) (G_TYPE_CHECK_INSTANCE_TYPE((obj),(objtype)))

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsHttpEPServer, kms_http_ep_server,
    G_TYPE_OBJECT,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, OBJECT_NAME,
        0, "debug category for " OBJECT_NAME " element") )

/* properties */
enum {
  PROP_0,

  PROP_KMS_HTTP_EP_SERVER_PORT,
  PROP_KMS_HTTP_EP_SERVER_INTERFACE,
  PROP_KMS_HTTP_EP_SERVER_ANNOUNCED_ADDRESS,

  N_PROPERTIES
};

#define KMS_HTTP_EP_SERVER_DEFAULT_PORT 0
#define KMS_HTTP_EP_SERVER_DEFAULT_INTERFACE NULL
#define KMS_HTTP_EP_SERVER_DEFAULT_ANNOUNCED_ADDRESS KMS_HTTP_EP_SERVER_DEFAULT_INTERFACE

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

/* signals */
enum {
  ACTION_REQUESTED,
  URL_REMOVED,
  URL_EXPIRED,
  LAST_SIGNAL
};

static guint obj_signals[LAST_SIGNAL] = { 0 };

struct resolv_data {
  KmsHttpEPServerStartCallback cb;
  KmsHttpEPServer *server;
};

struct sample_data {
  KmsHttpEPServer *httpepserver;
  GstSample *sample;
  SoupMessage *msg;
};

static gchar *
get_address ()
{
  gchar *addressStr;
  GList *ips, *l;
  gboolean done = FALSE;

  ips = nice_interfaces_get_local_ips (FALSE);

  for (l = ips; l != NULL && !done; l = l->next) {
    GInetAddress *addr;

    addr = g_inet_address_new_from_string ( (const gchar *) l->data);

    if (addr == NULL) {
      GST_WARNING ("Can not parse address %s", (const gchar *) l->data);
      continue;
    }

    switch (g_inet_address_get_family (addr) ) {
    case G_SOCKET_FAMILY_INVALID:
    case G_SOCKET_FAMILY_UNIX:
      /* Ignore this addresses */
      break;
    case G_SOCKET_FAMILY_IPV6:
      /* Ignore this addresses */
      break;
    case G_SOCKET_FAMILY_IPV4:
      addressStr = g_strdup ( (const gchar *) l->data);
      done = TRUE;
      break;
    }

    g_object_unref (addr);
  }

  g_list_free_full (ips, g_free);

  return addressStr;
}

static gboolean
cookie_has_expired (SoupCookie *cookie)
{
  SoupDate *date = soup_cookie_get_expires (cookie);

  return soup_date_is_past (date);
}

static void
remove_cookie_timeout (GstElement *httpep)
{
  guint *timeout_id;

  /* Remove timeout cookie if there is any */
  timeout_id = (guint *) g_object_get_data (G_OBJECT (httpep), KEY_TIMEOUT_ID);

  if (timeout_id == NULL)
    return;

  GST_DEBUG ("Remove timeout for cookie %d", *timeout_id);
  g_source_remove (*timeout_id);
  g_object_set_data_full (G_OBJECT (httpep), KEY_TIMEOUT_ID, NULL, NULL);
}

static gboolean
msg_has_finished (SoupMessage *msg)
{
  gboolean *finished;

  finished = (gboolean *) g_object_get_data (G_OBJECT (msg), KEY_FINISHED);

  return *finished;
}

static GstElement *
kms_http_ep_server_get_ep_from_msg (KmsHttpEPServer *self, SoupMessage *msg)
{
  SoupURI *suri = soup_message_get_uri (msg);
  const char *uri = soup_uri_get_path (suri);

  if (uri == NULL || self->priv->handlers == NULL)
    return NULL;

  return (GstElement *) g_hash_table_lookup (self->priv->handlers, uri);
}

static gboolean
send_buffer_cb (gpointer data)
{
  struct sample_data *sdata = (struct sample_data *) data;
  GstBuffer *buffer;
  GstMapInfo info;

  if (msg_has_finished (sdata->msg) ) {
    GST_WARNING ("Client has closed underlaying HTTP connection. "
        "Buffer won't be sent");
    return FALSE;
  }

  buffer = gst_sample_get_buffer (sdata->sample);

  if (buffer == NULL)
    return FALSE;

  if (!gst_buffer_map (buffer, &info, GST_MAP_READ) ) {
    GST_WARNING ("Could not get buffer map");
    return FALSE;
  }

  soup_message_body_append (sdata->msg->response_body, SOUP_MEMORY_COPY,
      info.data, info.size);
  soup_server_unpause_message (sdata->httpepserver->priv->server, sdata->msg);

  gst_buffer_unmap (buffer, &info);
  return FALSE;
}

static void
destroy_sample_data (gpointer data)
{
  struct sample_data *sdata = (struct sample_data *) data;

  if (sdata->sample != NULL)
    gst_sample_unref (sdata->sample);

  if (sdata->httpepserver != NULL)
    g_object_unref (sdata->httpepserver);

  if (sdata->msg != NULL)
    g_object_unref (sdata->msg);

  g_slice_free (struct sample_data, sdata);
}

static GstFlowReturn
new_sample_handler (GstElement *httpep, gpointer data)
{
  SoupMessage *msg = (SoupMessage *) g_object_get_data (G_OBJECT (httpep), KEY_MESSAGE);
  GstSample *sample = NULL;
  struct sample_data *sdata;

  GST_TRACE ("New-sample for message %" GST_PTR_FORMAT, msg);

  g_signal_emit_by_name (httpep, "pull-sample", &sample);

  if (sample == NULL)
    return GST_FLOW_ERROR;

  sdata = g_slice_new (struct sample_data);
  sdata->sample = gst_sample_ref (sample);
  sdata->msg = (SoupMessage *) g_object_ref (G_OBJECT (msg) );
  sdata->httpepserver = KMS_HTTP_EP_SERVER (g_object_ref (
      g_object_get_data (G_OBJECT (msg), KEY_HTTP_EP_SERVER) ) );

  /* Write buffer in the main context thread */
  g_idle_add_full (G_PRIORITY_HIGH_IDLE, send_buffer_cb, sdata,
      destroy_sample_data);

  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

static void
get_recv_eos (GstElement *httep, gpointer data)
{
  SoupMessage *msg = (SoupMessage *) data;

  GST_DEBUG ("EOS received on HttpEndPoint %s", GST_ELEMENT_NAME (httep) );
  soup_message_body_complete (msg->response_body);
}

static void
msg_finished (SoupMessage *msg)
{
  gboolean *finished;

  finished = (gboolean *) g_object_get_data (G_OBJECT (msg), KEY_FINISHED);
  *finished = TRUE;
}

static void
disconnect_eos_new_sample_signals (SoupMessage *msg)
{
  KmsHttpEPServer *serv = KMS_HTTP_EP_SERVER (
      g_object_get_data (G_OBJECT (msg), KEY_HTTP_EP_SERVER) );
  SoupURI *uri = soup_message_get_uri (msg);
  const char *path = soup_uri_get_path (uri);
  GstElement *httpep;
  gulong *handler;

  if (serv->priv->handlers == NULL)
    return;

  if (!g_hash_table_contains (serv->priv->handlers, path) ) {
    GST_WARNING ("Message %" GST_PTR_FORMAT
        " was bounded to an unregistered HttpEndPoint", msg);
    return;
  }

  httpep = (GstElement *) g_hash_table_lookup (serv->priv->handlers, path);
  GST_DEBUG ("Message %" GST_PTR_FORMAT " is bounded to %s", msg,
      GST_ELEMENT_NAME (httpep) );

  /* Disconnect signals */
  handler = (gulong *) g_object_get_data (G_OBJECT (msg),
      KEY_NEW_SAMPLE_HANDLER_ID);
  g_signal_handler_disconnect (httpep, *handler);

  handler = (gulong *) g_object_get_data (G_OBJECT (msg), KEY_EOS_HANDLER_ID);
  g_signal_handler_disconnect (httpep, *handler);
}

static gboolean
emit_expiration_signal_cb (gpointer user_data)
{
  SoupMessage *msg = (SoupMessage *) user_data;
  KmsHttpEPServer *serv = (KmsHttpEPServer *) g_object_get_data (G_OBJECT (msg),
      KEY_HTTP_EP_SERVER);
  SoupURI *uri = soup_message_get_uri (msg);
  const char *path = soup_uri_get_path (uri);
  GstElement *httpep;

  GST_DEBUG ("Cookie expired for %s", path);
  g_signal_emit (G_OBJECT (serv), obj_signals[URL_EXPIRED], 0, path);

  httpep = (GstElement *) g_hash_table_lookup (serv->priv->handlers, path);

  if (httpep != NULL)
    remove_cookie_timeout (httpep);

  return FALSE;
}

static void
destroy_guint (guint *id)
{
  g_slice_free (guint, id);
}

static void
emit_expiration_signal (SoupMessage *msg, GstElement *httpep)
{
  KmsHttpEPServer *self = (KmsHttpEPServer *) g_object_get_data (G_OBJECT (msg),
      KEY_HTTP_EP_SERVER);
  SoupURI *uri = soup_message_get_uri (msg);
  const char *path = soup_uri_get_path (uri);
  SoupCookie *cookie;
  double t_cookie, t_timeout;
  SoupDate *date, *now;
  guint interval, *timeout, *id;

  cookie = (SoupCookie *) g_object_get_data (G_OBJECT (httpep), KEY_COOKIE);

  if (cookie == NULL) {
    GST_WARNING ("No cookie set for element %s", GST_ELEMENT_NAME (httpep) );
    return;
  }

  if (cookie_has_expired (cookie) ) {
    GST_DEBUG ("Emit expiration signal");
    g_signal_emit (G_OBJECT (self), obj_signals[URL_EXPIRED], 0, path);
    return;
  }

  /* Set a timeout if no more connection are done over this httpendpoint */
  /* and the cookie expires */
  now = soup_date_new_from_now (0);
  date = soup_cookie_get_expires (cookie);
  timeout = (guint *) g_object_get_data (G_OBJECT (httpep), KEY_PARAM_TIMEOUT);

  t_cookie = difftime (soup_date_to_time_t (date), soup_date_to_time_t (now) );
  t_timeout = difftime (soup_date_to_time_t (now) + *timeout,
      soup_date_to_time_t (now) );

  interval = (guint) ( (t_cookie < t_timeout) ? t_cookie : t_timeout);

  id = g_slice_new (guint);
  *id = g_timeout_add_full (G_PRIORITY_DEFAULT, interval * 1000,
      emit_expiration_signal_cb, g_object_ref (G_OBJECT (msg) ), g_object_unref);
  g_object_set_data_full (G_OBJECT (httpep), KEY_TIMEOUT_ID, id,
      (GDestroyNotify) destroy_guint);
}

static void
finished_get_processing (SoupMessage *msg, gpointer data)
{
  GstElement *httpep = GST_ELEMENT (data);
  gpointer param;

  GST_DEBUG ("Message finished %" GST_PTR_FORMAT, msg);
  msg_finished (msg);

  disconnect_eos_new_sample_signals (msg);

  /* Drop internal media flowing in the piepline */
  g_object_set (G_OBJECT (httpep), "start", FALSE, NULL);

  param = g_object_steal_data (G_OBJECT (httpep), KEY_MESSAGE);

  if (param != NULL)
    g_object_unref (G_OBJECT (param) );

  emit_expiration_signal (msg, httpep);
}

static void
destroy_gboolean (gboolean *finished)
{
  g_slice_free (gboolean, finished);
}

static void
destroy_ulong (gulong *handlerid)
{
  g_slice_free (gulong, handlerid);
}

static void
msg_add_finished_property (SoupMessage *msg)
{
  gboolean *finished;

  finished = g_slice_new (gboolean);
  *finished = FALSE;

  g_object_set_data_full (G_OBJECT (msg), KEY_FINISHED, finished,
      (GDestroyNotify) destroy_gboolean);
}

static void
kms_http_ep_server_get_handler (KmsHttpEPServer *self, SoupMessage *msg,
    GstElement *httpep)
{
  gulong *handlerid;

  /* TODO: Check wether we support client's capabilities before sending */
  /* back a response code 200 OK. Furthermore, we only provide support  */
  /* for webm in content type response */
  soup_message_set_status (msg, SOUP_STATUS_OK);
  soup_message_headers_set_content_type (msg->response_headers, "video/webm",
      NULL);
  soup_message_headers_set_encoding (msg->response_headers,
      SOUP_ENCODING_CHUNKED);

  msg_add_finished_property (msg);

  handlerid = g_slice_new (gulong);
  *handlerid = g_signal_connect (G_OBJECT (msg), "finished",
      G_CALLBACK (finished_get_processing), httpep);
  g_object_set_data_full (G_OBJECT (msg), KEY_FINISHED_HANDLER_ID, handlerid,
      (GDestroyNotify) destroy_ulong);

  handlerid = g_slice_new (gulong);
  *handlerid = g_signal_connect (httpep, "new-sample",
      G_CALLBACK (new_sample_handler), NULL);
  g_object_set_data_full (G_OBJECT (msg), KEY_NEW_SAMPLE_HANDLER_ID, handlerid,
      (GDestroyNotify) destroy_ulong);

  handlerid = g_slice_new (gulong);
  *handlerid = g_signal_connect (httpep, "eos", G_CALLBACK (get_recv_eos), msg);
  g_object_set_data_full (G_OBJECT (msg), KEY_EOS_HANDLER_ID, handlerid,
      (GDestroyNotify) destroy_ulong);

  /* allow media stream to flow in HttpEndPoint pipeline */
  g_object_set (G_OBJECT (httpep), "start", TRUE, NULL);
}

static void
find_content_part (const gchar *start, const gchar *end,
    const gchar **content_start, const gchar **content_end,
    const gchar *boundary)
{
  const char *b, *c;
  int boundary_len;

  boundary_len = g_utf8_strlen (boundary, -1);
  *content_start = NULL;
  *content_end = NULL;

  for (b = (const char *) memchr (start, '-', end - start);
      b && b + boundary_len + 4 < end; b = (const char *) memchr (b + 2, '-', end - (b + 2) ) ) {
    /* Check for "--boundary" */
    if (b[1] != '-' || g_str_has_prefix (boundary, b + 2) != 0)
      continue;

    /* Check that it's at start of line */
    if (! (b == start || (b[-1] == '\n' && b[-2] == '\r') ) )
      continue;

    /* Check for "--" or "\r\n" after boundary */
    if (b[boundary_len + 2] == '-' && b[boundary_len + 3] == '-') {
      *content_end = b - 2;
    } else if (b[boundary_len + 2] == '\r' && b[boundary_len + 3] == '\n') {
      *content_start = b + boundary_len + 3;
    }
  }

  if (*content_start != NULL) {
    for (c = (const char *) memchr (*content_start, '\r', end - *content_start);
        c < end; c = (const char *) memchr (c + 4, '\r', end - (c + 2) ) ) {
      if (c[1] == '\n' && c[2] == '\r' && c[3] == '\n') {
        *content_start = c + 4;
        break;
      }
    }
  }
}

static void
got_chunk_handler (SoupMessage *msg, SoupBuffer *chunk, gpointer data)
{
  const gchar *content_start = NULL, *content_end = NULL;
  gchar *boundary = (gchar *) g_object_get_data (G_OBJECT (msg), KEY_BOUNDARY);
  GstElement *httpep = GST_ELEMENT (data);
  gconstpointer copy_from;
  GstFlowReturn ret;
  GstBuffer *buffer;
  GstMemory *memory;
  GstMapInfo info;
  gint len = 0;

  guint method;

  GST_INFO ("Chunk callback.");

  if (boundary != NULL)
    find_content_part (chunk->data, chunk->data + chunk->length, &content_start,
        &content_end, boundary);

  if (content_start != NULL) {
    if (content_end != NULL)
      len = content_end - content_start;
    else
      len = chunk->length - (content_start - chunk->data);

    copy_from = content_start;
  } else if (content_end != NULL) {
    len = content_end - chunk->data;
    copy_from = chunk->data;
  } else {
    len = chunk->length;
    copy_from = chunk->data;
  }

  buffer = gst_buffer_new ();
  memory = gst_allocator_alloc (NULL, len, NULL);
  gst_buffer_append_memory (buffer, memory);

  gst_buffer_map (buffer, &info, GST_MAP_WRITE);

  memcpy (info.data, copy_from, info.size);
  gst_buffer_unmap (buffer, &info);

  g_object_get (G_OBJECT (httpep), "http-method", &method, NULL);

  g_signal_emit_by_name (httpep, "push-buffer", buffer, &ret);

  if (ret != GST_FLOW_OK) {
    /* something wrong */
    GST_ERROR ("Could not send buffer to httpep %s. Ret code %d",
        GST_ELEMENT_NAME (httpep), ret);
  }

  gst_buffer_unref (buffer);
}

static void
finished_post_processing (SoupMessage *msg, gpointer data)
{
  GstElement *httpep = GST_ELEMENT (data);
  GstFlowReturn ret;
  gpointer param;

  GST_DEBUG ("POST finished");
  msg_finished (msg);

  g_signal_emit_by_name (httpep, "end-of-stream", &ret);

  if (ret != GST_FLOW_OK) {
    // something wrong
    GST_ERROR ("Could not send EOS to %s. Ret code %d",
        GST_ELEMENT_NAME (httpep), ret);
  }

  param = g_object_steal_data (G_OBJECT (httpep), KEY_MESSAGE);

  if (param != NULL)
    g_object_unref (G_OBJECT (param) );

  emit_expiration_signal (msg, httpep);
}

static void
kms_http_ep_server_post_handler (KmsHttpEPServer *self, SoupMessage *msg,
    GstElement *httpep)
{
  const gchar *content_type;
  GHashTable *params;
  gulong *handlerid;
  gchar *boundary;

  content_type =
    soup_message_headers_get_content_type (msg->request_headers, &params);

  if (content_type == NULL) {
    GST_WARNING ("Content-type header is not present in request");
    soup_message_set_status (msg, SOUP_STATUS_NOT_ACCEPTABLE);
    goto end;
  }

  if (!g_str_has_prefix ("multipart/", content_type) )
    goto get_chunks;

  boundary = g_strdup ( (gchar *) g_hash_table_lookup (params, "boundary") );

  if (boundary == NULL) {
    GST_WARNING ("Malformed multipart POST request");
    soup_message_set_status (msg, SOUP_STATUS_NOT_ACCEPTABLE);
    goto end;
  }

  g_object_set_data_full (G_OBJECT (msg), KEY_BOUNDARY, boundary, g_free);

get_chunks:

  soup_message_set_status (msg, SOUP_STATUS_OK);

  /* Get chunks without filling-in body's data field after */
  /* the body is fully sent/received */
  soup_message_body_set_accumulate (msg->request_body, FALSE);

  msg_add_finished_property (msg);

  handlerid = g_slice_new (gulong);
  *handlerid = g_signal_connect (msg, "got-chunk",
      G_CALLBACK (got_chunk_handler), httpep);
  g_object_set_data_full (G_OBJECT (msg), KEY_GOT_CHUNK_HANDLER_ID, handlerid,
      (GDestroyNotify) destroy_ulong);

  handlerid = g_slice_new (gulong);
  *handlerid = g_signal_connect (msg, "finished",
      G_CALLBACK (finished_post_processing), httpep);
  g_object_set_data_full (G_OBJECT (msg), KEY_FINISHED_HANDLER_ID, handlerid,
      (GDestroyNotify) destroy_ulong);

end:

  if (params != NULL)
    g_hash_table_destroy (params);
}

static void
emit_removed_url_signal (gpointer data, gpointer user_data)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (user_data);
  gchar *uri = (gchar *) data;

  GST_DEBUG ("Emit signal for uri %s", uri);
  g_signal_emit (G_OBJECT (self), obj_signals[URL_REMOVED], 0, uri);
}

static void
kms_http_ep_server_destroy_handlers (KmsHttpEPServer *self)
{
  GList *keys;

  /* Emit removed url signal for each key */
  keys = g_hash_table_get_keys (self->priv->handlers);
  g_list_foreach (keys, (GFunc) emit_removed_url_signal, self);
  g_list_free (keys);

  /* Remove handlers */
  g_hash_table_remove_all (self->priv->handlers);
  self->priv->handlers = NULL;
}

static void
kms_http_ep_server_stop_impl (KmsHttpEPServer *self)
{
  if (self->priv->server == NULL) {
    GST_WARNING ("Server is not started");
    return;
  }

  kms_http_ep_server_destroy_handlers (self);

  /* Stops processing for server */
  soup_server_quit (self->priv->server);
}

static void
destroy_pending_message (SoupMessage *msg)
{
  gulong *handlerid;

  GST_DEBUG ("Destroy pending message %" GST_PTR_FORMAT, msg);

  if (msg->method == SOUP_METHOD_GET) {
    KmsHttpEPServer *serv = KMS_HTTP_EP_SERVER (
        g_object_get_data (G_OBJECT (msg), KEY_HTTP_EP_SERVER) );
    GstElement *httpep = kms_http_ep_server_get_ep_from_msg (serv, msg);

    disconnect_eos_new_sample_signals (msg);

    if (httpep != NULL) {
      /* Drop internal media flowing in the piepline */
      g_object_set (G_OBJECT (httpep), "start", FALSE, NULL);
    }

    soup_server_unpause_message (serv->priv->server, msg);
    soup_message_body_complete (msg->response_body);
  } else if (msg->method == SOUP_METHOD_POST) {
    handlerid = (gulong *) g_object_get_data (G_OBJECT (msg),
        KEY_GOT_CHUNK_HANDLER_ID);
    g_signal_handler_disconnect (G_OBJECT (msg), *handlerid);
  }

  /* Do not call to finished callback */
  handlerid = (gulong *) g_object_get_data (G_OBJECT (msg),
      KEY_FINISHED_HANDLER_ID);
  g_signal_handler_disconnect (G_OBJECT (msg), *handlerid);

  /* Remove internal msg reference */
  g_object_unref (G_OBJECT (msg) );
}

static gboolean
kms_http_ep_server_register_handler (KmsHttpEPServer *self, gchar *uri,
    GstElement *endpoint)
{
  GstElement *element;

  element = (GstElement *) g_hash_table_lookup (self->priv->handlers, uri);

  if (element != NULL) {
    GST_ERROR ("URI %s is already registered for element %s.", uri,
        GST_ELEMENT_NAME (element) );
    return FALSE;
  }

  g_hash_table_insert (self->priv->handlers, uri, g_object_ref (endpoint) );

  return TRUE;
}

static void
kms_http_ep_server_set_cookie (KmsHttpEPServer *self, GstElement *httpep,
    SoupMessage *msg, const char *path)
{
  gchar *id_str, *header;
  SoupCookie *cookie;
  guint *lifetime;
  gint64 id;

  /* No cookie has been set for this httpep */
  lifetime = (guint *) g_object_get_data (G_OBJECT (httpep),
      KEY_PARAM_LIFETIME);
  id = g_rand_double_range (self->priv->rand, G_MININT64, G_MAXINT64);
  id_str = g_strdup_printf ("%" G_GINT64_FORMAT, id);
  cookie = soup_cookie_new (COOKIE_NAME, id_str, self->priv->announcedAddr,
      path, *lifetime);
  g_free (id_str);

  header = soup_cookie_to_set_cookie_header (cookie);
  soup_message_headers_append (msg->response_headers, "Set-Cookie", header);
  g_free (header);

  g_object_set_data_full (G_OBJECT (httpep), KEY_COOKIE, cookie,
      (GDestroyNotify) soup_cookie_free);
}

static gboolean
kms_http_ep_server_check_cookie (SoupCookie *cookie, SoupMessage *msg)
{
  GSList *cookies, *e;
  gboolean ret = FALSE;

  if (cookie_has_expired (cookie) ) {
    GST_WARNING ("Cookie has expired");
    return FALSE;
  }

  /* Check cookie */
  cookies = soup_cookies_from_request (msg);

  if (cookies == NULL) {
    GST_WARNING ("No cookie present in request");
    return FALSE;
  }

  for (e = cookies; e != NULL; e = e->next) {
    SoupCookie *c = (SoupCookie *) e->data;

    if (g_strcmp0 (soup_cookie_get_name (cookie),
        soup_cookie_get_name (c) ) != 0)
      continue;

    if (g_strcmp0 (soup_cookie_get_value (cookie),
        soup_cookie_get_value (c) ) == 0) {
      ret = TRUE;
      break;
    }
  }

  soup_cookies_free (cookies);
  return ret;
}

static gboolean
kms_http_ep_server_manage_cookie_session (KmsHttpEPServer *self,
    GstElement *httpep, SoupMessage *msg, const char *path)
{
  SoupCookie *cookie;

  cookie = (SoupCookie *) g_object_get_data (G_OBJECT (httpep), KEY_COOKIE);

  if (cookie != NULL)
    return kms_http_ep_server_check_cookie (cookie, msg);

  kms_http_ep_server_set_cookie (self, httpep, msg, path);

  return TRUE;
}

static void
got_headers_handler (SoupMessage *msg, gpointer data)
{
  KmsHttpEndPointAction action = KMS_HTTP_END_POINT_ACTION_UNDEFINED;
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (data);
  SoupURI *uri = soup_message_get_uri (msg);
  const char *path = soup_uri_get_path (uri);
  GstElement *httpep;

  httpep = (GstElement *) g_hash_table_lookup (self->priv->handlers, path);

  if (httpep == NULL) {
    /* URI is not registered */
    soup_message_set_status_full (msg, SOUP_STATUS_NOT_FOUND,
        "Http end point not found");
    return;
  }

  if (!kms_http_ep_server_manage_cookie_session (self, httpep, msg, path) ) {
    GST_TRACE ("Request declined because of a cookie error");
    soup_message_set_status_full (msg, SOUP_STATUS_BAD_REQUEST,
        "Invalid cookie");
    return;
  }

  remove_cookie_timeout (httpep);

  /* Bind message life cicle to this httpendpoint */
  g_object_set_data_full (G_OBJECT (httpep), KEY_MESSAGE,
      g_object_ref (G_OBJECT (msg) ), (GDestroyNotify) destroy_pending_message);

  /* Common parameters used for both, get and post operations */
  g_object_set_data_full (G_OBJECT (msg), KEY_HTTP_EP_SERVER,
      g_object_ref (self), g_object_unref);

  if (msg->method == SOUP_METHOD_GET) {
    kms_http_ep_server_get_handler (self, msg, httpep);
    action = KMS_HTTP_END_POINT_ACTION_GET;
  } else if (msg->method == SOUP_METHOD_POST) {
    kms_http_ep_server_post_handler (self, msg, httpep);
    action = KMS_HTTP_END_POINT_ACTION_POST;
  } else {
    GST_WARNING ("HTTP operation %s is not allowed", msg->method);
    soup_message_set_status_full (msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
        "Not allowed");
    return;
  }

  g_signal_emit (G_OBJECT (self), obj_signals[ACTION_REQUESTED], 0, path,
      action);
}

static void
request_started_handler (SoupServer *server, SoupMessage *msg,
    SoupClientContext *client, gpointer data)
{
  g_signal_connect (msg, "got-headers", G_CALLBACK (got_headers_handler), data);
}

static void
kms_http_ep_server_create_server (KmsHttpEPServer *self, SoupAddress *addr)
{
  SoupSocket *listener;

  self->priv->server = soup_server_new (SOUP_SERVER_PORT, self->priv->port,
      SOUP_SERVER_INTERFACE, addr, NULL);

  /* Connect server signals handlers */
  g_signal_connect (self->priv->server, "request-started",
      G_CALLBACK (request_started_handler), self);

  soup_server_run_async (self->priv->server);

  listener = soup_server_get_listener (self->priv->server);

  if (!soup_socket_is_connected (listener) ) {
    GST_ERROR ("Server socket is not connected");
    return;
  }

  addr = soup_socket_get_local_address (listener);

  if (self->priv->iface == NULL) {
    /* Update the recently id adrress */
    self->priv->iface = g_strdup (soup_address_get_physical (addr) );
    /* TODO: Emit property change signal */
  }

  if (self->priv->port == 0) {
    /* Update the recently id adrress */
    self->priv->port = soup_address_get_port (addr);
    /* TODO: Emit property change signal */
  }

  GST_DEBUG ("Http end point server running in %s:%d", self->priv->iface,
      self->priv->port );
}

static void
soup_address_callback (SoupAddress *addr, guint status, gpointer user_data)
{
  struct resolv_data *rdata = (struct resolv_data *) user_data;
  GError *gerr = NULL;

  switch (status) {
  case SOUP_STATUS_OK:
    GST_DEBUG ("Domain name resolved");
    kms_http_ep_server_create_server (rdata->server, addr);
    break;
  case SOUP_STATUS_CANCELLED:
    g_set_error (&gerr, KMS_HTTP_EP_SERVER_ERROR,
        HTTPEPSERVER_RESOLVE_CANCELED_ERROR, "Domain name resolution canceled");
    break;
  case SOUP_STATUS_CANT_RESOLVE:
    g_set_error (&gerr, KMS_HTTP_EP_SERVER_ERROR,
        HTTPEPSERVER_CANT_RESOLVE_ERROR, "Domain name can not be resolved");
    break;
  default:
    g_set_error (&gerr, KMS_HTTP_EP_SERVER_ERROR,
        HTTPEPSERVER_UNEXPECTED_ERROR, "Domain name can not be resolved");
    break;
  }

  rdata->cb (rdata->server, gerr);

  g_object_unref (rdata->server);

  if (gerr != NULL)
    g_error_free (gerr);

  g_slice_free (struct resolv_data, rdata);
}

static void
kms_http_ep_server_start_impl (KmsHttpEPServer *self,
    KmsHttpEPServerStartCallback start_cb)
{
  struct resolv_data *rdata;
  SoupAddress *addr = NULL;

  if (self->priv->server != NULL) {
    GST_WARNING ("Server is already running");
    return;
  }

  if (self->priv->iface == NULL) {
    kms_http_ep_server_create_server (self, NULL);
    start_cb (self, NULL);
    return;
  }

  rdata = g_slice_new (struct resolv_data);
  rdata->cb = start_cb;
  rdata->server = KMS_HTTP_EP_SERVER ( g_object_ref (self) );

  addr = soup_address_new (self->priv->iface, self->priv->port);

  soup_address_resolve_async (addr, NULL,
      NULL /* FIXME: Add cancellable support */,
      (SoupAddressCallback) soup_address_callback, rdata);
}

static void
add_guint_param (GstElement *httpep, const gchar *name, guint val)
{
  guint *param;

  param = g_slice_new (guint);
  *param = val;
  g_object_set_data_full (G_OBJECT (httpep), name, param,
      (GDestroyNotify) destroy_guint);
}

static const gchar *
kms_http_ep_server_register_end_point_impl (KmsHttpEPServer *self,
    GstElement *endpoint, guint lifetime, guint timeout)
{
  gchar *url;
  uuid_t uuid;
  gchar *uuid_str;

  /* Check whether this is really an httpendpoint element */
  if (http_t == G_TYPE_INVALID) {
    GstElementFactory *http_f;

    http_f = gst_element_factory_find ("httpendpoint");

    if (http_f == NULL) {
      GST_ERROR ("No httpendpoint factory found");
      return NULL;
    }

    http_t = gst_element_factory_get_element_type (http_f);
  }

  if (!KMS_IS_EXPECTED_TYPE (endpoint, http_t) ) {
    GST_ERROR ("Element %s is not an httpendpoint", GST_ELEMENT_NAME (endpoint) );
    return NULL;
  }

  uuid_str = (gchar *) g_malloc (UUID_STR_SIZE);
  uuid_generate (uuid);
  uuid_unparse (uuid, uuid_str);

  /* Create URL from uuid string and add it to list of handlers */
  url = g_strdup_printf ("/%s", uuid_str);
  g_free (uuid_str);

  if (!kms_http_ep_server_register_handler (self, url, endpoint) ) {
    g_free (url);
    return NULL;
  }

  add_guint_param (endpoint, KEY_PARAM_LIFETIME, lifetime);
  add_guint_param (endpoint, KEY_PARAM_TIMEOUT, timeout);

  return url;
}

static gboolean
kms_http_ep_server_unregister_end_point_impl (KmsHttpEPServer *self,
    const gchar *uri)
{
  GstElement *httpep;

  GST_DEBUG ("Unregister uri: %s", uri);

  if (self->priv->handlers == NULL)
    return FALSE;

  if (!g_hash_table_contains (self->priv->handlers, uri) ) {
    GST_DEBUG ("Uri %s is not registered", uri);
    return FALSE;
  }

  httpep = (GstElement *) g_hash_table_lookup (self->priv->handlers, uri);

  remove_cookie_timeout (httpep);

  /* Cancel current transtacion */
  g_object_set_data_full (G_OBJECT (httpep), KEY_MESSAGE, NULL, NULL);

  g_hash_table_remove (self->priv->handlers, uri);
  emit_removed_url_signal ( (gpointer) uri, self);
  return TRUE;
}

static void
kms_http_ep_server_dispose (GObject *obj)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (obj);

  GST_DEBUG_OBJECT (self, "dispose");

  if (self->priv->server) {
    soup_server_disconnect (self->priv->server);
    g_clear_object (&self->priv->server);
  }

  /* Chain up to the parent class */
  G_OBJECT_CLASS (kms_http_ep_server_parent_class)->dispose (obj);
}

static void
kms_http_ep_server_finalize (GObject *obj)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (obj);

  GST_DEBUG_OBJECT (self, "finalize");

  if (self->priv->iface) {
    g_free (self->priv->iface);
    self->priv->iface = NULL;
  }

  if (self->priv->announcedAddr) {
    g_free (self->priv->announcedAddr);
    self->priv->announcedAddr = NULL;
  }

  if (self->priv->handlers != NULL)
    kms_http_ep_server_destroy_handlers (self);

  if (self->priv->rand != NULL) {
    g_rand_free (self->priv->rand);
    self->priv->rand = NULL;
  }

  /* Chain up to the parent class */
  G_OBJECT_CLASS (kms_http_ep_server_parent_class)->finalize (obj);
}

static void
kms_http_ep_server_set_property (GObject *obj, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (obj);

  switch (prop_id) {
  case PROP_KMS_HTTP_EP_SERVER_PORT:
    self->priv->port = g_value_get_int (value);
    break;
  case PROP_KMS_HTTP_EP_SERVER_INTERFACE:

    if (self->priv->iface != NULL)
      g_free (self->priv->iface);

    self->priv->iface = g_value_dup_string (value);
    break;
  case PROP_KMS_HTTP_EP_SERVER_ANNOUNCED_ADDRESS: {
    gchar *val = g_value_dup_string (value);

    if (self->priv->announcedAddr != NULL)
      g_free (self->priv->announcedAddr);

    if (val == NULL) {
      self->priv->announcedAddr = get_address ();
      GST_DEBUG ("Announced address is %s", self->priv->announcedAddr);
    } else {
      self->priv->announcedAddr = val;
    }

    break;
  }
  default:
    /* We don't have any other property... */
    G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
    break;
  }
}

static void
kms_http_ep_server_get_property (GObject *obj, guint prop_id, GValue *value,
    GParamSpec *pspec)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (obj);

  switch (prop_id) {
  case PROP_KMS_HTTP_EP_SERVER_PORT:
    g_value_set_int (value, self->priv->port);
    break;
  case PROP_KMS_HTTP_EP_SERVER_INTERFACE:
    g_value_set_string (value, self->priv->iface);
    break;
  case PROP_KMS_HTTP_EP_SERVER_ANNOUNCED_ADDRESS:
    g_value_set_string (value, self->priv->announcedAddr);
    break;
  default:
    /* We don't have any other property... */
    G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
    break;
  }
}

static void
kms_http_ep_server_class_init (KmsHttpEPServerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = kms_http_ep_server_set_property;
  gobject_class->get_property = kms_http_ep_server_get_property;
  gobject_class->dispose = kms_http_ep_server_dispose;
  gobject_class->finalize = kms_http_ep_server_finalize;

  /* Set public virtual methods */
  klass->start = kms_http_ep_server_start_impl;
  klass->stop = kms_http_ep_server_stop_impl;
  klass->register_end_point = kms_http_ep_server_register_end_point_impl;
  klass->unregister_end_point = kms_http_ep_server_unregister_end_point_impl;

  obj_properties[PROP_KMS_HTTP_EP_SERVER_PORT] =
    g_param_spec_int (KMS_HTTP_EP_SERVER_PORT,
        "port number",
        "The TCP port to listen on",
        0,
        G_MAXUSHORT,
        KMS_HTTP_EP_SERVER_DEFAULT_PORT,
        (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE) );

  obj_properties[PROP_KMS_HTTP_EP_SERVER_INTERFACE] =
    g_param_spec_string (KMS_HTTP_EP_SERVER_INTERFACE,
        "IP address",
        "IP address of the network interface to run the server on",
        KMS_HTTP_EP_SERVER_DEFAULT_INTERFACE,
        (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE) );

  obj_properties[PROP_KMS_HTTP_EP_SERVER_ANNOUNCED_ADDRESS] =
    g_param_spec_string (KMS_HTTP_EP_SERVER_ANNOUNCED_IP,
        "Announced IP address",
        "IP address that will be used to compose URLs",
        KMS_HTTP_EP_SERVER_DEFAULT_INTERFACE,
        (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE) );

  g_object_class_install_properties (gobject_class,
      N_PROPERTIES,
      obj_properties);

  obj_signals[ACTION_REQUESTED] =
    g_signal_new ("action-requested",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (KmsHttpEPServerClass, action_requested), NULL, NULL,
        kms_marshal_VOID__STRING_ENUM, G_TYPE_NONE, 2, G_TYPE_STRING,
        GST_TYPE_HTTP_END_POINT_ACTION);

  obj_signals[URL_REMOVED] =
    g_signal_new ("url-removed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (KmsHttpEPServerClass, url_removed), NULL, NULL,
        g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);

  obj_signals[URL_EXPIRED] =
    g_signal_new ("url-expired",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (KmsHttpEPServerClass, url_expired), NULL, NULL,
        g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);

  /* Registers a private structure for an instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsHttpEPServerPrivate) );
}

static gboolean
equal_str_key (gconstpointer a, gconstpointer b)
{
  const char *str1 = (const char *) a;
  const char *str2 = (const char *) b;

  return (g_strcmp0 (str1, str2) == 0);
}

static void
kms_http_ep_server_init (KmsHttpEPServer *self)
{
  self->priv = KMS_HTTP_EP_SERVER_GET_PRIVATE (self);

  /* Set default values */
  self->priv->server = NULL;
  self->priv->port = KMS_HTTP_EP_SERVER_DEFAULT_PORT;
  self->priv->iface = KMS_HTTP_EP_SERVER_DEFAULT_INTERFACE;
  self->priv->announcedAddr = KMS_HTTP_EP_SERVER_DEFAULT_ANNOUNCED_ADDRESS;
  self->priv->handlers = g_hash_table_new_full (g_str_hash, equal_str_key,
      g_free, g_object_unref);

  self->priv->rand = g_rand_new();
}

/* Virtual public methods */
KmsHttpEPServer *
kms_http_ep_server_new (const char *optname1, ...)
{
  KmsHttpEPServer *self;

  va_list ap;

  va_start (ap, optname1);
  self = KMS_HTTP_EP_SERVER (g_object_new_valist (KMS_TYPE_HTTP_EP_SERVER,
      optname1, ap) );
  va_end (ap);

  return KMS_HTTP_EP_SERVER (self);
}

void
kms_http_ep_server_start (KmsHttpEPServer *self,
    KmsHttpEPServerStartCallback start_cb)
{
  g_return_if_fail (KMS_IS_HTTP_EP_SERVER (self) );

  KMS_HTTP_EP_SERVER_GET_CLASS (self)->start (self, start_cb);
}

void
kms_http_ep_server_stop (KmsHttpEPServer *self)
{
  g_return_if_fail (KMS_IS_HTTP_EP_SERVER (self) );

  KMS_HTTP_EP_SERVER_GET_CLASS (self)->stop (self);
}

const gchar *
kms_http_ep_server_register_end_point (KmsHttpEPServer *self,
    GstElement *endpoint, guint lifetime, guint timeout)
{
  g_return_val_if_fail (KMS_IS_HTTP_EP_SERVER (self), NULL);

  return KMS_HTTP_EP_SERVER_GET_CLASS (self)->register_end_point (self,
      endpoint, lifetime, timeout);
}

gboolean
kms_http_ep_server_unregister_end_point (KmsHttpEPServer *self,
    const gchar *uri)
{
  g_return_val_if_fail (KMS_IS_HTTP_EP_SERVER (self), FALSE);

  return KMS_HTTP_EP_SERVER_GET_CLASS (self)->unregister_end_point (self, uri);
}
