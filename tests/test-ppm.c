#include <string.h>

#include "wsp.h"

static GBytes* bytes(const char* header, gsize pixels, guint8 fill) {
  gsize hl = strlen(header);
  guint8* data = g_malloc(MAX(1, hl + pixels));
  if (hl) memcpy(data, header, hl);
  if (pixels) memset(data + hl, fill, pixels);
  return g_bytes_new_take(data, hl + pixels);
}

static void test_valid(void) {
  GBytes* b = bytes("P6\n3 2\n255\n", 3 * 2 * 3, 0x7f);
  GError* error = NULL;
  GdkPixbuf* p = wsp_ppm_to_pixbuf(b, &error);
  g_assert_no_error(error);
  g_assert_nonnull(p);
  g_assert_cmpint(gdk_pixbuf_get_width(p), ==, 3);
  g_assert_cmpint(gdk_pixbuf_get_height(p), ==, 2);
  g_assert_false(gdk_pixbuf_get_has_alpha(p));
  g_assert_cmpint(gdk_pixbuf_get_pixels(p)[0], ==, 0x7f);
  g_object_unref(p);
  g_bytes_unref(b);
}

static void test_comments(void) {
  GBytes* b = bytes("P6 # made by a test\n2 # width\n1\n255 ", 2 * 1 * 3, 1);
  GdkPixbuf* p = wsp_ppm_to_pixbuf(b, NULL);
  g_assert_nonnull(p);
  g_assert_cmpint(gdk_pixbuf_get_width(p), ==, 2);
  g_object_unref(p);
  g_bytes_unref(b);
}

static void expect_error(const char* header, gsize pixels) {
  GBytes* b = bytes(header, pixels, 0);
  GError* error = NULL;
  GdkPixbuf* p = wsp_ppm_to_pixbuf(b, &error);
  g_assert_null(p);
  g_assert_nonnull(error);
  g_error_free(error);
  g_bytes_unref(b);
}

static void test_invalid(void) {
  expect_error("P5\n3 2\n255\n", 6);    /* greyscale */
  expect_error("P6\n3 2\n65535\n", 36); /* 16-bit */
  expect_error("P6\n3 2\n255\n", 17);   /* cut short */
  expect_error("P6\n0 2\n255\n", 0);    /* empty */
  expect_error("P6\n3", 0);             /* no header */
  expect_error("", 0);
}

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ppm/valid", test_valid);
  g_test_add_func("/ppm/comments", test_comments);
  g_test_add_func("/ppm/invalid", test_invalid);
  return g_test_run();
}
