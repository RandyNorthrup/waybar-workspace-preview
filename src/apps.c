#include <gio/gdesktopappinfo.h>
#include <string.h>

#include "wsp.h"

/* Window class → (app name, icon), from the installed desktop entries: matched on the desktop file id, its
 * last dotted part (org.gnome.Nautilus → nautilus) and StartupWMClass, all lowercase. */
struct _WspApps {
  GHashTable* by_key; /* key → App* (shared) */
  GPtrArray* apps;    /* owns the App structs */
};

typedef struct {
  char* name;
  char* icon;
} App;

static void app_free(App* app) {
  g_free(app->name);
  g_free(app->icon);
  g_free(app);
}

/* Browsers whose "open as window" sites get classes like chrome-<site>-<profile>: the site's window title
 * is its name, the browser's icon stands in for its own. */
static const char* const site_app_browsers[][2] = {
    {"chrome-", "google-chrome"}, {"brave-", "brave-browser"}, {"msedge-", "microsoft-edge"}, {"vivaldi-", "vivaldi"}};

static void add_key(WspApps* apps, const char* key, App* app) {
  if (!key || !*key) return;
  char* lower = g_ascii_strdown(key, -1);
  if (!g_hash_table_contains(apps->by_key, lower)) {
    g_hash_table_insert(apps->by_key, lower, app);
  } else {
    g_free(lower);
  }
}

WspApps* wsp_apps_new(void) {
  WspApps* apps = g_new0(WspApps, 1);
  apps->by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  apps->apps = g_ptr_array_new_with_free_func((GDestroyNotify)app_free);
  GList* all = g_app_info_get_all();
  for (GList* l = all; l; l = l->next) {
    if (!G_IS_DESKTOP_APP_INFO(l->data)) continue;
    GDesktopAppInfo* info = l->data;
    App* app = g_new0(App, 1);
    app->name = g_strdup(g_app_info_get_name(G_APP_INFO(info)));
    GIcon* icon = g_app_info_get_icon(G_APP_INFO(info));
    app->icon = icon ? g_icon_to_string(icon) : g_strdup("");
    if (!app->icon) app->icon = g_strdup("");
    g_ptr_array_add(apps->apps, app);

    const char* id = g_app_info_get_id(G_APP_INFO(info));
    char* stem = id ? g_strndup(id, g_str_has_suffix(id, ".desktop") ? strlen(id) - 8 : strlen(id)) : NULL;
    add_key(apps, stem, app);
    const char* dot = stem ? strrchr(stem, '.') : NULL;
    add_key(apps, dot ? dot + 1 : NULL, app);
    add_key(apps, g_desktop_app_info_get_startup_wm_class(info), app);
    g_free(stem);
  }
  g_list_free_full(all, g_object_unref);
  return apps;
}

void wsp_apps_free(WspApps* apps) {
  if (!apps) return;
  g_hash_table_destroy(apps->by_key);
  g_ptr_array_unref(apps->apps);
  g_free(apps);
}

static char* strip_suffix(const char* title, const char* name) {
  const char* seps[] = {" - ", " — ", " – "};
  for (gsize i = 0; name && *name && i < G_N_ELEMENTS(seps); i++) {
    char* suffix = g_strconcat(seps[i], name, NULL);
    gboolean match = g_str_has_suffix(title, suffix);
    gsize cut = strlen(title) - strlen(suffix);
    g_free(suffix);
    if (match) return g_strndup(title, cut);
  }
  return g_strdup(title);
}

void wsp_apps_describe(WspApps* apps, const WspWindow* window, char** name, char** icon, char** title) {
  const char* cls = window->class_name ? window->class_name : "";
  char* key = g_ascii_strdown(cls, -1);
  App* app = g_hash_table_lookup(apps->by_key, key);
  g_free(key);
  if (app) {
    *name = g_strdup(app->name);
    *icon = g_strdup(app->icon);
  } else {
    *name = NULL;
    *icon = NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(site_app_browsers) && !*name; i++) {
      if (!g_str_has_prefix(cls, site_app_browsers[i][0])) continue;
      App* browser = g_hash_table_lookup(apps->by_key, site_app_browsers[i][1]);
      *name = g_strdup(window->title && *window->title ? window->title : cls);
      *icon = g_strdup(browser ? browser->icon : site_app_browsers[i][1]);
    }
    if (!*name) {
      *name = g_strdup(*cls ? cls : "Window");
      *icon = g_ascii_strdown(cls, -1);
    }
  }
  char* shortened = strip_suffix(window->title ? window->title : "", *name);
  *title = g_strcmp0(shortened, *name) == 0 ? g_strdup("") : g_strdup(shortened);
  g_free(shortened);
}
