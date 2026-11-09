#include <math.h>
#include <string.h>

#include "wsp.h"

#define GRIM_TIMEOUT_MS 3000

/* ── drawing (any thread) ───────────────────────────────────────────────────────────────────────────── */

static guint32 rgba_pixel(const GdkRGBA* c) {
  guint r = (guint)lround(CLAMP(c->red, 0, 1) * 255), g = (guint)lround(CLAMP(c->green, 0, 1) * 255);
  guint b = (guint)lround(CLAMP(c->blue, 0, 1) * 255), a = (guint)lround(CLAMP(c->alpha, 0, 1) * 255);
  return (r << 24) | (g << 16) | (b << 8) | a;
}

/* Scales @src by (sx, sy), places its top-left at (ox, oy) and draws it onto @dest, clipped. */
static void draw_scaled(GdkPixbuf* src, GdkPixbuf* dest, double ox, double oy, double sx, double sy) {
  int dw = gdk_pixbuf_get_width(dest), dh = gdk_pixbuf_get_height(dest);
  int x0 = MAX(0, (int)floor(ox)), y0 = MAX(0, (int)floor(oy));
  int x1 = MIN(dw, (int)ceil(ox + gdk_pixbuf_get_width(src) * sx));
  int y1 = MIN(dh, (int)ceil(oy + gdk_pixbuf_get_height(src) * sy));
  if (x1 <= x0 || y1 <= y0) return;
  gdk_pixbuf_composite(src, dest, x0, y0, x1 - x0, y1 - y0, ox, oy, sx, sy, GDK_INTERP_BILINEAR, 255);
}

static void draw_wallpaper(GdkPixbuf* canvas, GdkPixbuf* wallpaper, WspFit fit) {
  double cw = gdk_pixbuf_get_width(canvas), ch = gdk_pixbuf_get_height(canvas);
  double iw = gdk_pixbuf_get_width(wallpaper), ih = gdk_pixbuf_get_height(wallpaper);
  double sx = cw / iw, sy = ch / ih;
  switch (fit) {
    case WSP_FIT_STRETCH:
      break;
    case WSP_FIT_FIT:
      sx = sy = MIN(sx, sy);
      break;
    case WSP_FIT_CENTER: /* unscaled at the monitor's size: same proportions as fill without enlarging */
      sx = sy = MIN(1.0, MAX(sx, sy));
      break;
    case WSP_FIT_FILL:
    default:
      sx = sy = MAX(sx, sy);
      break;
  }
  draw_scaled(wallpaper, canvas, (cw - iw * sx) / 2, (ch - ih * sy) / 2, sx, sy);
}

GdkPixbuf* wsp_compose(const WspMonitor* monitor, GPtrArray* windows, GPtrArray* shots, int width_px,
                       const WspCaptureOptions* options) {
  int tw, th;
  wsp_thumbnail_size(monitor, width_px, &tw, &th);
  GdkPixbuf* canvas = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, tw, th);
  gdk_pixbuf_fill(canvas, rgba_pixel(&options->background));
  if (options->wallpaper) draw_wallpaper(canvas, options->wallpaper, options->wallpaper_fit);
  double factor = monitor->rect.width > 0 ? tw / monitor->rect.width : 1.0;
  for (guint i = 0; i < windows->len; i++) {
    GdkPixbuf* shot = i < shots->len ? g_ptr_array_index(shots, i) : NULL;
    if (!shot) continue;
    WspRect r = wsp_rect_scale(((WspWindow*)g_ptr_array_index(windows, i))->rect, factor);
    draw_scaled(shot, canvas, r.x, r.y, r.width / gdk_pixbuf_get_width(shot), r.height / gdk_pixbuf_get_height(shot));
  }
  return canvas;
}

/* ── capturing ──────────────────────────────────────────────────────────────────────────────────────── */

typedef struct {
  WspMonitor monitor;
  GPtrArray* windows; /* bottom to top */
  GPtrArray* shots;   /* GdkPixbuf or NULL, same order */
  int pending;
  int width_px;
  WspCaptureOptions options; /* own copy */
} Job;

static void job_free(Job* job) {
  wsp_monitor_clear(&job->monitor);
  g_clear_pointer(&job->windows, g_ptr_array_unref);
  g_clear_pointer(&job->shots, g_ptr_array_unref);
  g_free(job->options.grim);
  g_clear_object(&job->options.wallpaper);
  g_free(job);
}

static void compose_in_thread(GTask* task, gpointer source, gpointer data, GCancellable* cancellable) {
  (void)source;
  (void)cancellable;
  Job* job = data;
  g_task_return_pointer(task, wsp_compose(&job->monitor, job->windows, job->shots, job->width_px, &job->options),
                        g_object_unref);
}

/* One capture (or the initial hold) is finished; consumes the reference it held on @task. */
static void one_done(GTask* task) {
  Job* job = g_task_get_task_data(task);
  if (--job->pending == 0 && !g_task_return_error_if_cancelled(task)) {
    g_task_run_in_thread(task, compose_in_thread);
  }
  g_object_unref(task);
}

typedef struct {
  GTask* task;
  guint index;
} ShotOp;

static void shot_done(GObject* source, GAsyncResult* result, gpointer data) {
  ShotOp* op = data;
  GBytes* out = NULL;
  GError* error = NULL;
  if (g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, NULL, &error) &&
      g_subprocess_get_if_exited(G_SUBPROCESS(source)) && g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0) {
    GdkPixbuf* shot = wsp_ppm_to_pixbuf(out, &error);
    if (shot) g_ptr_array_index(((Job*)g_task_get_task_data(op->task))->shots, op->index) = shot;
  }
  if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_debug("window capture failed: %s", error->message);
  }
  g_clear_error(&error);
  g_clear_pointer(&out, g_bytes_unref);
  one_done(op->task);
  g_free(op);
}

static gboolean give_up(gpointer data) {
  g_subprocess_force_exit(G_SUBPROCESS(data)); /* a capture that never comes back must not block the rest */
  return G_SOURCE_REMOVE;
}

static void spawn_grim(GTask* task, guint index, const char* stable_id) {
  Job* job = g_task_get_task_data(task);
  GError* error = NULL;
  GSubprocess* proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error,
                                       job->options.grim, "-T", stable_id, "-t", "ppm", "-", NULL);
  if (!proc) {
    g_debug("cannot run %s: %s", job->options.grim, error->message);
    g_error_free(error);
    return;
  }
  job->pending++;
  ShotOp* op = g_new0(ShotOp, 1);
  op->task = g_object_ref(task);
  op->index = index;
  g_timeout_add_full(G_PRIORITY_DEFAULT, GRIM_TIMEOUT_MS, give_up, g_object_ref(proc), g_object_unref);
  g_subprocess_communicate_async(proc, NULL, g_task_get_cancellable(task), shot_done, op);
  g_object_unref(proc);
}

void wsp_capture_async(const char* socket_dir, const char* name, int scale, const WspCaptureOptions* options,
                       GCancellable* cancellable, GAsyncReadyCallback callback, gpointer user_data) {
  GTask* task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, wsp_capture_async);
  Job* job = g_new0(Job, 1);
  job->width_px = MAX(1, options->width * MAX(1, scale));
  job->options.grim = g_strdup(options->grim);
  job->options.width = options->width;
  job->options.background = options->background;
  job->options.wallpaper = options->wallpaper ? g_object_ref(options->wallpaper) : NULL;
  job->options.wallpaper_fit = options->wallpaper_fit;
  g_task_set_task_data(task, job, (GDestroyNotify)job_free);

  GError* error = NULL;
  JsonNode* workspaces = wsp_hypr_request(socket_dir, "j/workspaces", &error);
  JsonNode* monitors = workspaces ? wsp_hypr_request(socket_dir, "j/monitors", &error) : NULL;
  JsonNode* clients = monitors ? wsp_hypr_request(socket_dir, "j/clients", &error) : NULL;
  gboolean found =
      clients && wsp_hypr_parse_workspace(workspaces, monitors, clients, name, &job->monitor, &job->windows);
  g_clear_pointer(&workspaces, json_node_unref);
  g_clear_pointer(&monitors, json_node_unref);
  g_clear_pointer(&clients, json_node_unref);
  if (!found) {
    if (error) {
      g_task_return_error(task, error);
    } else {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no workspace named %s", name);
    }
    g_object_unref(task);
    return;
  }

  wsp_windows_sort_for_drawing(job->windows);
  job->shots = g_ptr_array_new_full(job->windows->len, (GDestroyNotify)g_object_unref);
  g_ptr_array_set_size(job->shots, (gint)job->windows->len);
  job->pending = 1; /* held until every grim has been started */
  for (guint i = 0; i < job->windows->len; i++) {
    const WspWindow* window = g_ptr_array_index(job->windows, i);
    if (window->stable_id && *window->stable_id) spawn_grim(task, i, window->stable_id);
  }
  one_done(task); /* releases the hold; composes now if nothing was started */
}

GdkPixbuf* wsp_capture_finish(GAsyncResult* result, GError** error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}
