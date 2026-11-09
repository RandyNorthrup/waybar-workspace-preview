#include <string.h>
#include <glib.h>

#include "wsp.h"

/* Mirrors sanitizeCssClass() in Waybar's src/modules/hyprland/workspace.cpp, so a workspace's button can be
 * found from its name whatever label format the user chose. */
char* wsp_css_class_for_name(const char* name) {
  GString* result = g_string_sized_new(name ? strlen(name) + 3 : 0);
  for (const char* p = name; p && *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (g_ascii_isalnum(c)) {
      g_string_append_c(result, (char)g_ascii_tolower(c));
    } else if (result->len > 0 && result->str[result->len - 1] != '-') {
      g_string_append_c(result, '-');
    }
  }
  if (result->len > 0 && result->str[result->len - 1] == '-') g_string_truncate(result, result->len - 1);
  if (result->len > 0 && g_ascii_isdigit(result->str[0])) g_string_prepend(result, "ws-");
  return g_string_free(result, FALSE);
}
