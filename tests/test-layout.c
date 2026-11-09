#include "wsp.h"

static WspWindow* win(const char* id, gboolean floating, gboolean fullscreen, int focus) {
  WspWindow* w = g_new0(WspWindow, 1);
  w->stable_id = g_strdup(id);
  w->floating = floating;
  w->fullscreen = fullscreen;
  w->focus_history = focus;
  return w;
}

static void test_drawing_order(void) {
  GPtrArray* ws = g_ptr_array_new_with_free_func((GDestroyNotify)wsp_window_free);
  g_ptr_array_add(ws, win("float-recent", TRUE, FALSE, 0));
  g_ptr_array_add(ws, win("tiled-old", FALSE, FALSE, 4));
  g_ptr_array_add(ws, win("full", FALSE, TRUE, 3));
  g_ptr_array_add(ws, win("tiled-recent", FALSE, FALSE, 1));
  g_ptr_array_add(ws, win("float-old", TRUE, FALSE, 2));
  wsp_windows_sort_for_drawing(ws);
  const char* expected[] = {"tiled-old", "tiled-recent", "float-old", "float-recent", "full"};
  for (guint i = 0; i < ws->len; i++) {
    g_assert_cmpstr(((WspWindow*)g_ptr_array_index(ws, i))->stable_id, ==, expected[i]);
  }
  wsp_windows_sort_by_recency(ws);
  g_assert_cmpstr(((WspWindow*)g_ptr_array_index(ws, 0))->stable_id, ==, "float-recent");
  g_ptr_array_unref(ws);
}

static void test_thumbnail_size(void) {
  WspMonitor m = {NULL, {0, 0, 1280, 800}};
  int w, h;
  wsp_thumbnail_size(&m, 640, &w, &h);
  g_assert_cmpint(w, ==, 640);
  g_assert_cmpint(h, ==, 400);
  WspMonitor tall = {NULL, {0, 0, 1080, 1920}};
  wsp_thumbnail_size(&tall, 320, &w, &h);
  g_assert_cmpint(h, ==, 569);
  WspMonitor broken = {NULL, {0, 0, 0, 0}};
  wsp_thumbnail_size(&broken, 320, &w, &h);
  g_assert_cmpint(h, ==, 200);
}

static void test_rect_scale(void) {
  WspRect r = wsp_rect_scale((WspRect){10, 40, 1260, 750}, 0.5);
  g_assert_cmpfloat(r.x, ==, 5);
  g_assert_cmpfloat(r.y, ==, 20);
  g_assert_cmpfloat(r.width, ==, 630);
  g_assert_cmpfloat(r.height, ==, 375);
}

/* A window drawn into the thumbnail lands where it is on the monitor. */
static void test_compose(void) {
  WspMonitor m = {NULL, {0, 0, 100, 50}};
  GPtrArray* windows = g_ptr_array_new_with_free_func((GDestroyNotify)wsp_window_free);
  WspWindow* w = win("1", FALSE, FALSE, 0);
  w->rect = (WspRect){50, 0, 50, 50};
  g_ptr_array_add(windows, w);
  GdkPixbuf* shot = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 100, 100);
  gdk_pixbuf_fill(shot, 0xff0000ff);
  GPtrArray* shots = g_ptr_array_new_with_free_func(g_object_unref);
  g_ptr_array_add(shots, shot);
  WspCaptureOptions options = {0};
  gdk_rgba_parse(&options.background, "#0000ff");
  GdkPixbuf* thumb = wsp_compose(&m, windows, shots, 20, &options);
  g_assert_cmpint(gdk_pixbuf_get_width(thumb), ==, 20);
  g_assert_cmpint(gdk_pixbuf_get_height(thumb), ==, 10);
  const guint8* px = gdk_pixbuf_get_pixels(thumb);
  int stride = gdk_pixbuf_get_rowstride(thumb);
  const guint8* left = px + 5 * stride + 2 * 4;   /* background */
  const guint8* right = px + 5 * stride + 15 * 4; /* the window */
  g_assert_cmpint(left[2], ==, 255);
  g_assert_cmpint(left[0], ==, 0);
  g_assert_cmpint(right[0], ==, 255);
  g_assert_cmpint(right[2], ==, 0);
  g_object_unref(thumb);
  g_ptr_array_unref(shots);
  g_ptr_array_unref(windows);
}

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/layout/drawing-order", test_drawing_order);
  g_test_add_func("/layout/thumbnail-size", test_thumbnail_size);
  g_test_add_func("/layout/rect-scale", test_rect_scale);
  g_test_add_func("/layout/compose", test_compose);
  return g_test_run();
}
