#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>

#include "wsp.h"

#define RECONNECT_MIN_MS 500
#define RECONNECT_MAX_MS 10000

/* ── locating Hyprland ──────────────────────────────────────────────────────────────────────────────── */

static gboolean has_event_socket(const char* dir) {
  char* path = g_build_filename(dir, ".socket2.sock", NULL);
  gboolean ok = g_file_test(path, G_FILE_TEST_EXISTS);
  g_free(path);
  return ok;
}

char* wsp_hypr_socket_dir(void) {
  const char* signature = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
  const char* bases[] = {g_get_user_runtime_dir(), g_get_tmp_dir()}; /* $XDG_RUNTIME_DIR/hypr, then /tmp/hypr */
  for (gsize i = 0; signature && *signature && i < G_N_ELEMENTS(bases); i++) {
    char* dir = g_build_filename(bases[i], "hypr", signature, NULL);
    if (has_event_socket(dir)) return dir;
    g_free(dir);
  }
  /* No (or a stale) signature, e.g. Waybar outlived a Hyprland restart: the newest live instance. */
  char* base = g_build_filename(g_get_user_runtime_dir(), "hypr", NULL);
  GDir* entries = g_dir_open(base, 0, NULL);
  char* best = NULL;
  gint64 best_time = G_MININT64;
  for (const char* entry; entries && (entry = g_dir_read_name(entries));) {
    char* dir = g_build_filename(base, entry, NULL);
    GStatBuf st;
    if (has_event_socket(dir) && g_stat(dir, &st) == 0 && (gint64)st.st_mtime > best_time) {
      best_time = st.st_mtime;
      g_free(best);
      best = g_steal_pointer(&dir);
    }
    g_free(dir);
  }
  if (entries) g_dir_close(entries);
  g_free(base);
  return best;
}

/* ── requests ───────────────────────────────────────────────────────────────────────────────────────── */

JsonNode* wsp_hypr_request(const char* socket_dir, const char* command, GError** error) {
  char* path = g_build_filename(socket_dir, ".socket.sock", NULL);
  GSocketAddress* address = g_unix_socket_address_new(path);
  g_free(path);
  GSocketClient* client = g_socket_client_new();
  g_socket_client_set_timeout(client, 2);
  GSocketConnection* conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, error);
  g_object_unref(address);
  g_object_unref(client);
  if (!conn) return NULL;

  JsonNode* result = NULL;
  GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  if (g_output_stream_write_all(out, command, strlen(command), NULL, NULL, error)) {
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GByteArray* reply = g_byte_array_new();
    guint8 buffer[16384];
    gssize n;
    while ((n = g_input_stream_read(in, buffer, sizeof buffer, NULL, error)) > 0) g_byte_array_append(reply, buffer, n);
    if (n == 0) {
      JsonParser* parser = json_parser_new();
      if (json_parser_load_from_data(parser, (const char*)reply->data, reply->len, error)) {
        result = json_node_copy(json_parser_get_root(parser));
      }
      g_object_unref(parser);
    }
    g_byte_array_unref(reply);
  }
  g_object_unref(conn);
  return result;
}

/* ── parsing ────────────────────────────────────────────────────────────────────────────────────────── */

static double member_double(JsonObject* object, const char* name) {
  JsonNode* node = json_object_get_member(object, name);
  return node && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_double(node) : 0.0;
}

static gint64 member_int(JsonObject* object, const char* name, gint64 fallback) {
  JsonNode* node = json_object_get_member(object, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node)) return fallback;
  if (json_node_get_value_type(node) == G_TYPE_BOOLEAN) return json_node_get_boolean(node);
  return json_node_get_int(node);
}

static gboolean member_bool(JsonObject* object, const char* name) {
  return member_int(object, name, 0) != 0;
}

static const char* member_string(JsonObject* object, const char* name) {
  JsonNode* node = json_object_get_member(object, name);
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
             ? json_node_get_string(node)
             : NULL;
}

/* stableId is a string in current Hyprland; accept a number too. */
static char* member_id_string(JsonObject* object, const char* name) {
  JsonNode* node = json_object_get_member(object, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node)) return NULL;
  if (json_node_get_value_type(node) == G_TYPE_STRING) return g_strdup(json_node_get_string(node));
  return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
}

static void pair(JsonObject* object, const char* name, double* a, double* b) {
  JsonArray* array = json_object_has_member(object, name) ? json_object_get_array_member(object, name) : NULL;
  *a = array && json_array_get_length(array) > 0 ? json_array_get_double_element(array, 0) : 0;
  *b = array && json_array_get_length(array) > 1 ? json_array_get_double_element(array, 1) : 0;
}

static const char* client_workspace(JsonObject* client) {
  JsonObject* ws =
      json_object_has_member(client, "workspace") ? json_object_get_object_member(client, "workspace") : NULL;
  return ws ? member_string(ws, "name") : NULL;
}

static WspWindow* parse_client(JsonObject* client, const WspRect* origin) {
  WspWindow* window = g_new0(WspWindow, 1);
  window->stable_id = member_id_string(client, "stableId");
  window->class_name = g_strdup(member_string(client, "class") ? member_string(client, "class") : "");
  window->title = g_strdup(member_string(client, "title") ? member_string(client, "title") : "");
  pair(client, "at", &window->rect.x, &window->rect.y);
  pair(client, "size", &window->rect.width, &window->rect.height);
  if (origin) {
    window->rect.x -= origin->x;
    window->rect.y -= origin->y;
  }
  window->floating = member_bool(client, "floating");
  window->fullscreen = member_int(client, "fullscreen", 0) != 0;
  window->focus_history = (int)member_int(client, "focusHistoryID", 1000);
  return window;
}

static gboolean client_shown(JsonObject* client) {
  return member_int(client, "mapped", 1) != 0 && !member_bool(client, "hidden");
}

gboolean wsp_hypr_parse_workspace(JsonNode* workspaces, JsonNode* monitors, JsonNode* clients, const char* name,
                                  WspMonitor* monitor, GPtrArray** windows) {
  if (!JSON_NODE_HOLDS_ARRAY(workspaces) || !JSON_NODE_HOLDS_ARRAY(monitors) || !JSON_NODE_HOLDS_ARRAY(clients)) {
    return FALSE;
  }
  const char* monitor_name = NULL;
  JsonArray* ws_array = json_node_get_array(workspaces);
  for (guint i = 0; i < json_array_get_length(ws_array); i++) {
    JsonObject* ws = json_array_get_object_element(ws_array, i);
    if (g_strcmp0(member_string(ws, "name"), name) == 0) monitor_name = member_string(ws, "monitor");
  }
  if (!monitor_name) return FALSE;

  JsonObject* found = NULL;
  JsonArray* mon_array = json_node_get_array(monitors);
  for (guint i = 0; i < json_array_get_length(mon_array) && !found; i++) {
    JsonObject* mon = json_array_get_object_element(mon_array, i);
    if (g_strcmp0(member_string(mon, "name"), monitor_name) == 0) found = mon;
  }
  if (!found) return FALSE;
  double scale = member_double(found, "scale");
  if (scale <= 0) scale = 1;
  monitor->name = g_strdup(monitor_name);
  monitor->rect.x = member_double(found, "x");
  monitor->rect.y = member_double(found, "y");
  monitor->rect.width = member_double(found, "width") / scale;
  monitor->rect.height = member_double(found, "height") / scale;
  if (member_int(found, "transform", 0) % 2 == 1) { /* rotated 90° or 270° */
    double w = monitor->rect.width;
    monitor->rect.width = monitor->rect.height;
    monitor->rect.height = w;
  }

  *windows = g_ptr_array_new_with_free_func((GDestroyNotify)wsp_window_free);
  JsonArray* cl_array = json_node_get_array(clients);
  for (guint i = 0; i < json_array_get_length(cl_array); i++) {
    JsonObject* client = json_array_get_object_element(cl_array, i);
    if (g_strcmp0(client_workspace(client), name) == 0 && client_shown(client)) {
      g_ptr_array_add(*windows, parse_client(client, &monitor->rect));
    }
  }
  return TRUE;
}

GPtrArray* wsp_hypr_parse_workspace_names(JsonNode* workspaces) {
  GPtrArray* names = g_ptr_array_new_with_free_func(g_free);
  if (!JSON_NODE_HOLDS_ARRAY(workspaces)) return names;
  JsonArray* array = json_node_get_array(workspaces);
  for (guint i = 0; i < json_array_get_length(array); i++) {
    const char* name = member_string(json_array_get_object_element(array, i), "name");
    if (name && *name) g_ptr_array_add(names, g_strdup(name));
  }
  return names;
}

GHashTable* wsp_hypr_parse_windows_by_workspace(JsonNode* clients) {
  GHashTable* table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
  if (!JSON_NODE_HOLDS_ARRAY(clients)) return table;
  JsonArray* array = json_node_get_array(clients);
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonObject* client = json_array_get_object_element(array, i);
    const char* ws = client_workspace(client);
    if (!ws || !client_shown(client)) continue;
    GPtrArray* list = g_hash_table_lookup(table, ws);
    if (!list) {
      list = g_ptr_array_new_with_free_func((GDestroyNotify)wsp_window_free);
      g_hash_table_insert(table, g_strdup(ws), list);
    }
    g_ptr_array_add(list, parse_client(client, NULL));
  }
  return table;
}

char* wsp_hypr_event_workspace_name(const char* data) {
  const char* comma = data ? strchr(data, ',') : NULL;
  return g_strdup(comma ? comma + 1 : data ? data : "");
}

/* ── the event stream ───────────────────────────────────────────────────────────────────────────────── */

struct _WspEvents {
  char* socket_dir;
  WspEventFunc func;
  gpointer user_data;
  GCancellable* cancel;
  GSocketConnection* conn;
  GDataInputStream* in;
  guint retry_source;
  guint retry_ms;
};

static void connect_events(WspEvents* events);

static gboolean retry(gpointer data) {
  WspEvents* events = data;
  events->retry_source = 0;
  connect_events(events);
  return G_SOURCE_REMOVE;
}

static void schedule_retry(WspEvents* events) {
  g_clear_object(&events->in);
  g_clear_object(&events->conn);
  events->retry_ms = events->retry_ms ? MIN(events->retry_ms * 2, RECONNECT_MAX_MS) : RECONNECT_MIN_MS;
  events->retry_source = g_timeout_add(events->retry_ms, retry, events);
}

/* Each read carries its own reference to the cancellable: a read that completes just before
 * wsp_events_free() still runs its callback afterwards, which must then leave @events alone. */
typedef struct {
  WspEvents* events;
  GCancellable* cancel;
} ReadOp;

static void read_next(WspEvents* events);

static void line_read(GObject* source, GAsyncResult* result, gpointer data) {
  ReadOp* op = data;
  GError* error = NULL;
  char* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), result, NULL, &error);
  gboolean gone = g_cancellable_is_cancelled(op->cancel);
  WspEvents* events = op->events;
  g_object_unref(op->cancel);
  g_free(op);
  if (gone) {
    g_clear_error(&error);
    g_free(line);
    return;
  }
  if (!line) { /* Hyprland went away (or restarted): try again */
    g_clear_error(&error);
    schedule_retry(events);
    return;
  }
  events->retry_ms = 0;
  char* sep = strstr(line, ">>");
  if (sep) {
    *sep = '\0';
    events->func(line, sep + 2, events->user_data);
  }
  g_free(line);
  read_next(events);
}

static void read_next(WspEvents* events) {
  ReadOp* op = g_new0(ReadOp, 1);
  op->events = events;
  op->cancel = g_object_ref(events->cancel);
  g_data_input_stream_read_line_async(events->in, G_PRIORITY_DEFAULT, events->cancel, line_read, op);
}

static void connect_events(WspEvents* events) {
  char* dir = wsp_hypr_socket_dir();
  if (dir && g_strcmp0(dir, events->socket_dir) != 0) {
    g_free(events->socket_dir);
    events->socket_dir = g_strdup(dir);
    events->func("wsp-socket-dir", dir, events->user_data); /* a new Hyprland instance */
  }
  g_free(dir);
  char* path = events->socket_dir ? g_build_filename(events->socket_dir, ".socket2.sock", NULL) : NULL;
  GSocketClient* client = g_socket_client_new();
  GSocketAddress* address = path ? g_unix_socket_address_new(path) : NULL;
  events->conn = address ? g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL) : NULL;
  g_clear_object(&address);
  g_object_unref(client);
  g_free(path);
  if (!events->conn) {
    schedule_retry(events);
    return;
  }
  events->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(events->conn)));
  read_next(events);
  events->func("wsp-connected", "", events->user_data);
}

WspEvents* wsp_events_new(const char* socket_dir, WspEventFunc func, gpointer user_data) {
  WspEvents* events = g_new0(WspEvents, 1);
  events->socket_dir = g_strdup(socket_dir);
  events->func = func;
  events->user_data = user_data;
  events->cancel = g_cancellable_new();
  connect_events(events);
  return events;
}

void wsp_events_free(WspEvents* events) {
  if (!events) return;
  if (events->retry_source) g_source_remove(events->retry_source);
  g_cancellable_cancel(events->cancel); /* a pending read completes later and sees it: see ReadOp */
  g_object_unref(events->cancel);
  g_clear_object(&events->in);
  g_clear_object(&events->conn);
  g_free(events->socket_dir);
  g_free(events);
}
