#include <string.h>

#include "wsp.h"

#define LEAVE_DELAY_MS 250 /* after switching away from a workspace, before capturing it */
#define LISTS_DELAY_MS 120 /* window events come in bursts: refresh the lists once they settle */

/* One store serves every bar (Waybar runs one module instance per bar/output): one event connection, one
 * capture per workspace, one set of thumbnails. */
struct _WspStore {
  int refs;
  char* socket_dir;
  WspCaptureOptions options;
  gboolean capture_on_leave;
  int scale; /* the sharpest any bar needs */
  WspEvents* events;
  GCancellable* cancel;      /* cancelled when the last bar goes: pending captures leave the store alone */
  GHashTable* thumbs;        /* name → WspThumbnail* */
  GHashTable* capturing;     /* names being captured */
  GHashTable* class_to_name; /* Waybar's button CSS class → workspace name */
  GPtrArray* names;
  GHashTable* windows; /* name → GPtrArray of WspWindow, most recent first */
  guint windows_serial;
  guint thumb_serial;
  GHashTable* leaving; /* names to capture once LEAVE_DELAY_MS has passed */
  guint leave_source;
  guint lists_source;
  char* active;
  gboolean captured_all;
  GArray* watchers; /* Watcher */
  guint next_watch;
  WspApps* apps;
};

typedef struct {
  guint id;
  WspChangedFunc func;
  gpointer user_data;
} Watcher;

static WspStore* shared;

static void thumbnail_free(WspThumbnail* thumb) {
  g_clear_object(&thumb->pixbuf);
  g_free(thumb);
}

static void notify(WspStore* store, const char* workspace) {
  /* Copy first: a watcher may unwatch (or a bar go away) while being told. */
  GArray* copy = g_array_copy(store->watchers);
  for (guint i = 0; i < copy->len; i++) {
    Watcher* w = &g_array_index(copy, Watcher, i);
    w->func(workspace, w->user_data);
  }
  g_array_unref(copy);
}

/* ── workspace names and window lists ───────────────────────────────────────────────────────────────── */

static void refresh_names(WspStore* store) {
  JsonNode* reply = store->socket_dir ? wsp_hypr_request(store->socket_dir, "j/workspaces", NULL) : NULL;
  if (!reply) return;
  g_clear_pointer(&store->names, g_ptr_array_unref);
  store->names = wsp_hypr_parse_workspace_names(reply);
  json_node_unref(reply);
  g_hash_table_remove_all(store->class_to_name);
  for (guint i = 0; i < store->names->len; i++) {
    const char* name = g_ptr_array_index(store->names, i);
    char* css_class = wsp_css_class_for_name(name);
    if (*css_class && !g_hash_table_contains(store->class_to_name, css_class)) {
      g_hash_table_insert(store->class_to_name, css_class, g_strdup(name));
    } else {
      g_free(css_class);
    }
  }
  /* Thumbnails of workspaces that no longer exist. */
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, store->thumbs);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    gboolean alive = FALSE;
    for (guint i = 0; i < store->names->len && !alive; i++)
      alive = g_strcmp0(g_ptr_array_index(store->names, i), key) == 0;
    if (!alive) g_hash_table_iter_remove(&iter);
  }
}

static gboolean refresh_lists(gpointer data) {
  WspStore* store = data;
  store->lists_source = 0;
  JsonNode* reply = store->socket_dir ? wsp_hypr_request(store->socket_dir, "j/clients", NULL) : NULL;
  if (!reply) return G_SOURCE_REMOVE;
  GHashTable* windows = wsp_hypr_parse_windows_by_workspace(reply);
  json_node_unref(reply);
  GHashTableIter iter;
  gpointer list;
  g_hash_table_iter_init(&iter, windows);
  while (g_hash_table_iter_next(&iter, NULL, &list)) wsp_windows_sort_by_recency(list);
  g_hash_table_unref(store->windows);
  store->windows = windows;
  store->windows_serial++;
  notify(store, NULL);
  return G_SOURCE_REMOVE;
}

static void lists_soon(WspStore* store) {
  if (store->lists_source) g_source_remove(store->lists_source);
  store->lists_source = g_timeout_add(LISTS_DELAY_MS, refresh_lists, store);
}

/* ── captures ───────────────────────────────────────────────────────────────────────────────────────── */

typedef struct {
  WspStore* store; /* valid while @cancel is not cancelled */
  GCancellable* cancel;
  char* name;
  int scale;
} CaptureOp;

static void captured(GObject* source, GAsyncResult* result, gpointer data) {
  (void)source;
  CaptureOp* op = data;
  GError* error = NULL;
  GdkPixbuf* pixbuf = wsp_capture_finish(result, &error);
  if (!g_cancellable_is_cancelled(op->cancel)) {
    WspStore* store = op->store;
    g_hash_table_remove(store->capturing, op->name);
    if (pixbuf) {
      WspThumbnail* thumb = g_new0(WspThumbnail, 1);
      thumb->pixbuf = g_steal_pointer(&pixbuf);
      thumb->scale = op->scale;
      thumb->captured_at = g_get_monotonic_time();
      thumb->serial = ++store->thumb_serial;
      g_hash_table_replace(store->thumbs, g_strdup(op->name), thumb);
      notify(store, op->name);
    } else if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_debug("capturing workspace %s failed: %s", op->name, error->message);
    }
  }
  g_clear_object(&pixbuf);
  g_clear_error(&error);
  g_object_unref(op->cancel);
  g_free(op->name);
  g_free(op);
}

static void capture(WspStore* store, const char* name) {
  if (!store->socket_dir || g_hash_table_contains(store->capturing, name)) return;
  g_hash_table_add(store->capturing, g_strdup(name));
  CaptureOp* op = g_new0(CaptureOp, 1);
  op->store = store;
  op->cancel = g_object_ref(store->cancel);
  op->name = g_strdup(name);
  op->scale = store->scale;
  wsp_capture_async(store->socket_dir, name, store->scale, &store->options, store->cancel, captured, op);
}

void wsp_store_refresh(WspStore* store, const char* workspace, gint64 min_age_us) {
  const WspThumbnail* thumb = g_hash_table_lookup(store->thumbs, workspace);
  if (thumb && thumb->scale >= store->scale && g_get_monotonic_time() - thumb->captured_at < min_age_us) return;
  capture(store, workspace);
}

static gboolean capture_left(gpointer data) {
  WspStore* store = data;
  store->leave_source = 0;
  GHashTableIter iter;
  gpointer name;
  g_hash_table_iter_init(&iter, store->leaving);
  while (g_hash_table_iter_next(&iter, &name, NULL)) {
    if (g_strcmp0(name, store->active) != 0) capture(store, name);
  }
  g_hash_table_remove_all(store->leaving);
  return G_SOURCE_REMOVE;
}

static void capture_all(WspStore* store) {
  for (guint i = 0; store->names && i < store->names->len; i++) capture(store, g_ptr_array_index(store->names, i));
}

/* ── Hyprland events ────────────────────────────────────────────────────────────────────────────────── */

static void read_active(WspStore* store) {
  JsonNode* reply = store->socket_dir ? wsp_hypr_request(store->socket_dir, "j/activeworkspace", NULL) : NULL;
  if (reply && JSON_NODE_HOLDS_OBJECT(reply)) {
    JsonObject* object = json_node_get_object(reply);
    if (json_object_has_member(object, "name")) {
      g_free(store->active);
      store->active = g_strdup(json_object_get_string_member(object, "name"));
    }
  }
  if (reply) json_node_unref(reply);
}

static void on_event(const char* event, const char* data, gpointer user_data) {
  WspStore* store = user_data;
  if (strcmp(event, "wsp-socket-dir") == 0) { /* Hyprland was restarted */
    g_free(store->socket_dir);
    store->socket_dir = g_strdup(data);
    g_hash_table_remove_all(store->thumbs);
    store->captured_all = FALSE;
  } else if (strcmp(event, "wsp-connected") == 0) {
    refresh_names(store);
    read_active(store);
    lists_soon(store);
    if (!store->captured_all) { /* so the first hover already has something to show */
      store->captured_all = TRUE;
      capture_all(store);
    }
  } else if (strcmp(event, "workspacev2") == 0) {
    char* name = wsp_hypr_event_workspace_name(data);
    char* left = g_steal_pointer(&store->active);
    store->active = name;
    if (store->capture_on_leave && left && g_strcmp0(left, name) != 0) {
      g_hash_table_add(store->leaving, g_steal_pointer(&left));
      if (store->leave_source) g_source_remove(store->leave_source);
      store->leave_source = g_timeout_add(LEAVE_DELAY_MS, capture_left, store);
    }
    g_free(left);
  } else if (strcmp(event, "createworkspacev2") == 0 || strcmp(event, "destroyworkspacev2") == 0 ||
             strcmp(event, "renameworkspace") == 0 || strcmp(event, "moveworkspacev2") == 0) {
    refresh_names(store);
    lists_soon(store);
  } else if (strcmp(event, "openwindow") == 0 || strcmp(event, "closewindow") == 0 ||
             strcmp(event, "movewindowv2") == 0 || strcmp(event, "windowtitlev2") == 0 ||
             strcmp(event, "changefloatingmode") == 0 || strcmp(event, "fullscreen") == 0 ||
             strcmp(event, "activewindowv2") == 0) {
    lists_soon(store);
  }
}

/* ── the shared store ───────────────────────────────────────────────────────────────────────────────── */

WspStore* wsp_store_acquire(const WspCaptureOptions* options, gboolean capture_on_leave) {
  if (shared) {
    shared->refs++;
    return shared;
  }
  char* dir = wsp_hypr_socket_dir();
  if (!dir) return NULL;
  WspStore* store = g_new0(WspStore, 1);
  store->refs = 1;
  store->socket_dir = dir;
  store->options.grim = g_strdup(options->grim);
  store->options.width = options->width;
  store->options.background = options->background;
  store->options.wallpaper = options->wallpaper ? g_object_ref(options->wallpaper) : NULL;
  store->options.wallpaper_fit = options->wallpaper_fit;
  store->capture_on_leave = capture_on_leave;
  store->scale = 1;
  store->cancel = g_cancellable_new();
  store->thumbs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)thumbnail_free);
  store->capturing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  store->class_to_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  store->windows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
  store->leaving = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  store->watchers = g_array_new(FALSE, FALSE, sizeof(Watcher));
  shared = store;
  store->events = wsp_events_new(dir, on_event, store);
  return store;
}

void wsp_store_release(WspStore* store) {
  if (!store || --store->refs > 0) return;
  shared = NULL;
  wsp_events_free(store->events);
  g_cancellable_cancel(store->cancel);
  g_object_unref(store->cancel);
  if (store->leave_source) g_source_remove(store->leave_source);
  if (store->lists_source) g_source_remove(store->lists_source);
  g_hash_table_destroy(store->thumbs);
  g_hash_table_destroy(store->capturing);
  g_hash_table_destroy(store->class_to_name);
  g_hash_table_destroy(store->windows);
  g_hash_table_destroy(store->leaving);
  g_clear_pointer(&store->names, g_ptr_array_unref);
  g_array_unref(store->watchers);
  wsp_apps_free(store->apps);
  g_free(store->options.grim);
  g_clear_object(&store->options.wallpaper);
  g_free(store->socket_dir);
  g_free(store->active);
  g_free(store);
}

guint wsp_store_watch(WspStore* store, WspChangedFunc func, gpointer user_data) {
  Watcher w = {++store->next_watch, func, user_data};
  g_array_append_val(store->watchers, w);
  return w.id;
}

void wsp_store_unwatch(WspStore* store, guint id) {
  for (guint i = 0; i < store->watchers->len; i++) {
    if (g_array_index(store->watchers, Watcher, i).id == id) {
      g_array_remove_index(store->watchers, i);
      return;
    }
  }
}

void wsp_store_require_scale(WspStore* store, int scale) {
  store->scale = MAX(store->scale, MAX(1, scale));
}

const char* wsp_store_workspace_for_button(WspStore* store, char** css_classes, const char* label) {
  for (char** c = css_classes; c && *c; c++) {
    const char* name = g_hash_table_lookup(store->class_to_name, *c);
    if (name) return name;
  }
  for (guint i = 0; label && store->names && i < store->names->len; i++) {
    if (g_strcmp0(g_ptr_array_index(store->names, i), label) == 0) return g_ptr_array_index(store->names, i);
  }
  return NULL;
}

const WspThumbnail* wsp_store_thumbnail(WspStore* store, const char* workspace) {
  return g_hash_table_lookup(store->thumbs, workspace);
}

GPtrArray* wsp_store_windows(WspStore* store, const char* workspace) {
  return g_hash_table_lookup(store->windows, workspace);
}

guint wsp_store_windows_serial(WspStore* store) {
  return store->windows_serial;
}

WspApps* wsp_store_apps(WspStore* store) {
  if (!store->apps) store->apps = wsp_apps_new(); /* only built if a bar lists windows */
  return store->apps;
}
