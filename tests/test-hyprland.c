#include "wsp.h"

static JsonNode* fixture(const char* name) {
  char* path = g_build_filename(g_getenv("FIXTURES"), name, NULL);
  JsonParser* parser = json_parser_new();
  GError* error = NULL;
  json_parser_load_from_file(parser, path, &error);
  g_assert_no_error(error);
  JsonNode* node = json_node_copy(json_parser_get_root(parser));
  g_object_unref(parser);
  g_free(path);
  return node;
}

typedef struct {
  JsonNode *workspaces, *monitors, *clients;
} Fixture;

static void setup(Fixture* f, gconstpointer data) {
  f->workspaces = fixture("workspaces.json");
  f->monitors = fixture("monitors.json");
  f->clients = fixture("clients.json");
}

static void teardown(Fixture* f, gconstpointer data) {
  json_node_unref(f->workspaces);
  json_node_unref(f->monitors);
  json_node_unref(f->clients);
}

static WspWindow* find(GPtrArray* windows, const char* class_name) {
  for (guint i = 0; i < windows->len; i++) {
    WspWindow* w = g_ptr_array_index(windows, i);
    if (g_strcmp0(w->class_name, class_name) == 0) return w;
  }
  return NULL;
}

static void test_workspace(Fixture* f, gconstpointer data) {
  WspMonitor m = {0};
  GPtrArray* windows = NULL;
  g_assert_true(wsp_hypr_parse_workspace(f->workspaces, f->monitors, f->clients, "1", &m, &windows));
  g_assert_cmpstr(m.name, ==, "eDP-1");
  g_assert_cmpfloat(m.rect.width, ==, 1280); /* 2560 px at scale 2 */
  g_assert_cmpfloat(m.rect.height, ==, 800);
  g_assert_cmpuint(windows->len, ==, 3); /* hidden and unmapped windows left out */
  g_assert_null(find(windows, "unmapped"));
  WspWindow* firefox = find(windows, "firefox");
  g_assert_cmpstr(firefox->stable_id, ==, "402653187"); /* a numeric id becomes a string */
  g_assert_cmpstr(firefox->title, ==, "Example Domain — Mozilla Firefox");
  g_assert_true(find(windows, "org.gnome.Calculator")->floating);
  g_ptr_array_unref(windows);
  wsp_monitor_clear(&m);
}

static void test_other_monitor(Fixture* f, gconstpointer data) {
  WspMonitor m = {0};
  GPtrArray* windows = NULL;
  g_assert_true(wsp_hypr_parse_workspace(f->workspaces, f->monitors, f->clients, "dev", &m, &windows));
  g_assert_cmpstr(m.name, ==, "DP-1");
  WspWindow* code = g_ptr_array_index(windows, 0);
  g_assert_cmpfloat(code->rect.x, ==, 0); /* relative to the monitor at x = 1280 */
  g_assert_true(code->fullscreen);
  g_ptr_array_unref(windows);
  wsp_monitor_clear(&m);
}

static void test_rotated_monitor(Fixture* f, gconstpointer data) {
  WspMonitor m = {0};
  GPtrArray* windows = NULL;
  g_assert_true(wsp_hypr_parse_workspace(f->workspaces, f->monitors, f->clients, "4", &m, &windows));
  g_assert_cmpfloat(m.rect.width, ==, 1080);
  g_assert_cmpfloat(m.rect.height, ==, 1920);
  g_assert_cmpuint(windows->len, ==, 0);
  g_ptr_array_unref(windows);
  wsp_monitor_clear(&m);
}

static void test_special_and_missing(Fixture* f, gconstpointer data) {
  WspMonitor m = {0};
  GPtrArray* windows = NULL;
  g_assert_true(wsp_hypr_parse_workspace(f->workspaces, f->monitors, f->clients, "special:magic", &m, &windows));
  g_assert_cmpuint(windows->len, ==, 1);
  g_assert_null(((WspWindow*)g_ptr_array_index(windows, 0))->stable_id); /* no stableId: listed, not captured */
  g_ptr_array_unref(windows);
  wsp_monitor_clear(&m);
  windows = NULL;
  g_assert_false(wsp_hypr_parse_workspace(f->workspaces, f->monitors, f->clients, "nope", &m, &windows));
  g_assert_null(windows);
}

static void test_names_and_lists(Fixture* f, gconstpointer data) {
  GPtrArray* names = wsp_hypr_parse_workspace_names(f->workspaces);
  g_assert_cmpuint(names->len, ==, 5);
  g_assert_cmpstr(g_ptr_array_index(names, 2), ==, "dev");
  g_ptr_array_unref(names);
  GHashTable* lists = wsp_hypr_parse_windows_by_workspace(f->clients);
  g_assert_cmpuint(((GPtrArray*)g_hash_table_lookup(lists, "1"))->len, ==, 3);
  g_assert_null(g_hash_table_lookup(lists, "2"));
  g_hash_table_unref(lists);
}

static void test_event_name(void) {
  char* name = wsp_hypr_event_workspace_name("3,dev");
  g_assert_cmpstr(name, ==, "dev");
  g_free(name);
  name = wsp_hypr_event_workspace_name("-98,special:magic");
  g_assert_cmpstr(name, ==, "special:magic");
  g_free(name);
  name = wsp_hypr_event_workspace_name("plain");
  g_assert_cmpstr(name, ==, "plain");
  g_free(name);
}

static void test_garbage(void) {
  JsonNode* object = json_from_string("{\"not\": \"an array\"}", NULL);
  WspMonitor m = {0};
  GPtrArray* windows = NULL;
  g_assert_false(wsp_hypr_parse_workspace(object, object, object, "1", &m, &windows));
  GPtrArray* names = wsp_hypr_parse_workspace_names(object);
  g_assert_cmpuint(names->len, ==, 0);
  g_ptr_array_unref(names);
  json_node_unref(object);
}

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add("/hyprland/workspace", Fixture, NULL, setup, test_workspace, teardown);
  g_test_add("/hyprland/other-monitor", Fixture, NULL, setup, test_other_monitor, teardown);
  g_test_add("/hyprland/rotated-monitor", Fixture, NULL, setup, test_rotated_monitor, teardown);
  g_test_add("/hyprland/special-and-missing", Fixture, NULL, setup, test_special_and_missing, teardown);
  g_test_add("/hyprland/names-and-lists", Fixture, NULL, setup, test_names_and_lists, teardown);
  g_test_add_func("/hyprland/event-name", test_event_name);
  g_test_add_func("/hyprland/garbage", test_garbage);
  return g_test_run();
}
