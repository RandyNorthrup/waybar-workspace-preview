#include <math.h>

#include "wsp.h"

void wsp_window_free(WspWindow* window) {
  if (!window) return;
  g_free(window->stable_id);
  g_free(window->class_name);
  g_free(window->title);
  g_free(window);
}

void wsp_monitor_clear(WspMonitor* monitor) {
  g_clear_pointer(&monitor->name, g_free);
}

static int layer_of(const WspWindow* w) {
  return w->fullscreen ? 2 : w->floating ? 1 : 0;
}

static gint compare_for_drawing(gconstpointer a, gconstpointer b) {
  const WspWindow* wa = *(WspWindow* const*)a;
  const WspWindow* wb = *(WspWindow* const*)b;
  int la = layer_of(wa), lb = layer_of(wb);
  if (la != lb) return la - lb;
  return wb->focus_history - wa->focus_history; /* least recently focused (highest number) first */
}

static gint compare_by_recency(gconstpointer a, gconstpointer b) {
  const WspWindow* wa = *(WspWindow* const*)a;
  const WspWindow* wb = *(WspWindow* const*)b;
  return wa->focus_history - wb->focus_history;
}

void wsp_windows_sort_for_drawing(GPtrArray* windows) {
  g_ptr_array_sort(windows, compare_for_drawing);
}

void wsp_windows_sort_by_recency(GPtrArray* windows) {
  g_ptr_array_sort(windows, compare_by_recency);
}

void wsp_thumbnail_size(const WspMonitor* monitor, int width_px, int* out_width, int* out_height) {
  double aspect = monitor->rect.width > 0 ? monitor->rect.height / monitor->rect.width : 10.0 / 16.0;
  *out_width = MAX(1, width_px);
  *out_height = MAX(1, (int)lround(width_px * aspect));
}

WspRect wsp_rect_scale(WspRect rect, double factor) {
  return (WspRect){rect.x * factor, rect.y * factor, rect.width * factor, rect.height * factor};
}
