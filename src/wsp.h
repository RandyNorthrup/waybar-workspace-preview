/* waybar-workspace-preview — shared declarations.
 *
 * The plugin is split into small units:
 *   naming.c    workspace name → the CSS class Waybar gives its button
 *   ppm.c       grim's PPM output → GdkPixbuf
 *   layout.c    window/monitor geometry, stacking order, thumbnail placement
 *   hyprland.c  Hyprland IPC: requests, the event stream, parsing its JSON
 *   capture.c   capturing one workspace into a thumbnail (grim per window, composited off the main thread)
 *   apps.c      window class → app name and icon (for the optional window list)
 *   store.c     state shared by every bar: thumbnails, workspace names, window lists, event handling
 *   plugin.c    the Waybar CFFI module: config, hooking the workspace buttons, the tooltip
 */
#pragma once

#include <gdk/gdk.h> /* GdkRGBA */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define WSP_LOG_DOMAIN "waybar-workspace-preview"

/* ── naming.c ─────────────────────────────────────────────────────────────────────────────────────── */

/* The CSS class Waybar (hyprland/workspaces) puts on a workspace's button: lowercase ASCII alphanumerics,
 * other runs collapsed to "-", no leading/trailing "-", "ws-" before a leading digit. "" if none. */
char* wsp_css_class_for_name(const char* name);

/* ── ppm.c ────────────────────────────────────────────────────────────────────────────────────────── */

/* A binary PPM (P6, maxval 255) as an RGB pixbuf; NULL and @error if it is not one. */
GdkPixbuf* wsp_ppm_to_pixbuf(GBytes* ppm, GError** error);

/* ── layout.c ─────────────────────────────────────────────────────────────────────────────────────── */

typedef struct {
  double x, y, width, height;
} WspRect;

typedef struct {
  char* stable_id; /* ext-foreign-toplevel identifier, for grim -T */
  char* class_name;
  char* title;
  WspRect rect; /* logical pixels, relative to the monitor's top-left corner */
  gboolean floating;
  gboolean fullscreen;
  int focus_history; /* 0 = most recently focused */
} WspWindow;

typedef struct {
  char* name;
  WspRect rect; /* logical pixels in the global layout (transform applied) */
} WspMonitor;

void wsp_window_free(WspWindow* window);
void wsp_monitor_clear(WspMonitor* monitor);

/* Bottom to top: tiled, then floating, then fullscreen; least recently focused first within each. */
void wsp_windows_sort_for_drawing(GPtrArray* windows);

/* Most recently focused first (the order of the window list). */
void wsp_windows_sort_by_recency(GPtrArray* windows);

/* Thumbnail size in pixels for a monitor: @width_px wide, the monitor's aspect ratio. */
void wsp_thumbnail_size(const WspMonitor* monitor, int width_px, int* out_width, int* out_height);

/* A window's rectangle in thumbnail pixels (@factor = thumbnail px per logical px). */
WspRect wsp_rect_scale(WspRect rect, double factor);

/* ── hyprland.c ───────────────────────────────────────────────────────────────────────────────────── */

/* Hyprland's socket directory for this session, or NULL if Hyprland is not running. */
char* wsp_hypr_socket_dir(void);

/* One IPC request ("j/clients", ...) answered as parsed JSON. */
JsonNode* wsp_hypr_request(const char* socket_dir, const char* command, GError** error);

/* From the j/workspaces, j/monitors and j/clients answers: the monitor showing workspace @name and its
 * mapped, visible windows (unsorted). FALSE if there is no such workspace. */
gboolean wsp_hypr_parse_workspace(JsonNode* workspaces, JsonNode* monitors, JsonNode* clients, const char* name,
                                  WspMonitor* monitor, GPtrArray** windows);

/* Names of all workspaces in a j/workspaces answer (special ones included). */
GPtrArray* wsp_hypr_parse_workspace_names(JsonNode* workspaces);

/* Windows of every workspace in a j/clients answer: name → GPtrArray of WspWindow. */
GHashTable* wsp_hypr_parse_windows_by_workspace(JsonNode* clients);

/* The workspace name in a "workspacev2>>ID,NAME"-style event payload. */
char* wsp_hypr_event_workspace_name(const char* data);

typedef void (*WspEventFunc)(const char* event, const char* data, gpointer user_data);
typedef struct _WspEvents WspEvents;

/* Follows Hyprland's event socket, calling @func for each event; reconnects if the connection drops. */
WspEvents* wsp_events_new(const char* socket_dir, WspEventFunc func, gpointer user_data);
void wsp_events_free(WspEvents* events);

/* ── capture.c ────────────────────────────────────────────────────────────────────────────────────── */

typedef enum { WSP_FIT_FILL, WSP_FIT_FIT, WSP_FIT_STRETCH, WSP_FIT_CENTER } WspFit;

typedef struct {
  char* grim;           /* grim executable */
  int width;            /* thumbnail width in logical pixels */
  GdkRGBA background;   /* behind everything; transparent shows the tooltip's own background */
  GdkPixbuf* wallpaper; /* optional, drawn over the background */
  WspFit wallpaper_fit;
} WspCaptureOptions;

/* Capture workspace @name at @scale (device pixels per logical pixel) in the background. */
void wsp_capture_async(const char* socket_dir, const char* name, int scale, const WspCaptureOptions* options,
                       GCancellable* cancellable, GAsyncReadyCallback callback, gpointer user_data);
GdkPixbuf* wsp_capture_finish(GAsyncResult* result, GError** error);

/* Draw @windows (sorted bottom to top, with their captured @shots, NULL where a capture failed) onto a new
 * thumbnail of @monitor. Thread-safe: no GTK. */
GdkPixbuf* wsp_compose(const WspMonitor* monitor, GPtrArray* windows, GPtrArray* shots, int width_px,
                       const WspCaptureOptions* options);

/* ── apps.c ───────────────────────────────────────────────────────────────────────────────────────── */

typedef struct _WspApps WspApps;
WspApps* wsp_apps_new(void);
void wsp_apps_free(WspApps* apps);
/* App name and icon (a GIcon string, may be "") for a window; the title is shortened when it only repeats
 * the app name. All three are newly allocated. */
void wsp_apps_describe(WspApps* apps, const WspWindow* window, char** name, char** icon, char** title);

/* ── store.c ──────────────────────────────────────────────────────────────────────────────────────── */

typedef struct _WspStore WspStore;

typedef struct {
  GdkPixbuf* pixbuf; /* device pixels */
  int scale;
  gint64 captured_at; /* monotonic µs */
  guint serial;       /* changes whenever the thumbnail does */
} WspThumbnail;

/* Called after a workspace's thumbnail or window list changed. */
typedef void (*WspChangedFunc)(const char* workspace, gpointer user_data);

/* The shared store (created by the first bar, freed with the last); NULL if Hyprland is not running.
 * Options are taken from the first bar that registers. */
WspStore* wsp_store_acquire(const WspCaptureOptions* options, gboolean capture_on_leave);
void wsp_store_release(WspStore* store);

guint wsp_store_watch(WspStore* store, WspChangedFunc func, gpointer user_data);
void wsp_store_unwatch(WspStore* store, guint id);

/* The bar needs thumbnails at least this sharp (its output's scale). */
void wsp_store_require_scale(WspStore* store, int scale);

/* The workspace name whose button carries CSS class @css_class or label @label (NULL if none). */
const char* wsp_store_workspace_for_button(WspStore* store, char** css_classes, const char* label);

/* The latest thumbnail of @workspace (NULL if none yet). */
const WspThumbnail* wsp_store_thumbnail(WspStore* store, const char* workspace);

/* Windows of @workspace, most recently focused first (NULL if none); owned by the store. */
GPtrArray* wsp_store_windows(WspStore* store, const char* workspace);
guint wsp_store_windows_serial(WspStore* store);

/* Capture @workspace again unless a capture is running or one finished less than @min_age_us ago. */
void wsp_store_refresh(WspStore* store, const char* workspace, gint64 min_age_us);

WspApps* wsp_store_apps(WspStore* store);

G_END_DECLS
