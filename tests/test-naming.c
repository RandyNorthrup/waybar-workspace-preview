#include "wsp.h"

static void check(const char* name, const char* expected) {
  char* got = wsp_css_class_for_name(name);
  g_assert_cmpstr(got, ==, expected);
  g_free(got);
}

/* The same results as Waybar's sanitizeCssClass(). */
static void test_css_class(void) {
  check("1", "ws-1");
  check("10", "ws-10");
  check("dev", "dev");
  check("Dev Tools", "dev-tools");
  check("  web!!", "web");
  check("special:magic", "special-magic");
  check("a--b", "a-b");
  check("-x-", "x");
  check("2nd", "ws-2nd");
  check("!!!", "");
  check("", "");
  check("café", "caf");
  check("Ünïcode", "n-code");
  check(NULL, "");
}

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/naming/css-class", test_css_class);
  return g_test_run();
}
