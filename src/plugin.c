/* waybar-workspace-preview: a thumbnail of each Hyprland workspace in the tooltip of its Waybar button.
 *
 * A Waybar CFFI module with no widget of its own. It finds the hyprland/workspaces module in the same bar
 * (the box Waybar names "workspaces") and answers its buttons' tooltip queries with a preview; Waybar keeps
 * drawing and handling the buttons. Buttons are matched to workspaces through the CSS class Waybar derives
 * from the workspace name, so any label format (icons, ids, custom text) works.
 */
#include <gtk/gtk.h>
#include <string.h>

#include "waybar_cffi_module.h"
#include "wsp.h"

/* Only Waybar's entry points leave the library; everything else is built with hidden visibility. */
#define WSP_EXPORT __attribute__((visibility("default")))

WSP_EXPORT const size_t wbcffi_version = 2;

#define HOOK_INTERVAL_MS 250
#define HOOK_TRIES 120 /* the workspaces module may be built after this one: keep looking for 30 s */
#define CHAR_PIXELS 8  /* rough width of a character in the tooltip font, to fit titles under the thumbnail */
#define ICON_PIXELS 16
#define HOOKED_KEY "wsp-hooked"

typedef struct {
  GtkWidget* widget;
  guint thumb_serial;
  guint windows_serial;
  int scale;
} Tooltip;

typedef struct {
  GtkWidget* root;
  GtkWidget* workspaces; /* weak */
  gulong layout_handler;
  guint hook_source;
  int hook_tries;
  WspStore* store;
  guint watch;
  gboolean show_windows;
  int width;             /* thumbnail width, logical pixels */
  gint64 refresh_age_us; /* < 0: never refresh on hover */
  GHashTable* tooltips;  /* workspace name → Tooltip* */
} Bar;

static void tooltip_free(Tooltip* tooltip) {
  g_clear_object(&tooltip->widget);
  g_free(tooltip);
}

/* ── config ─────────────────────────────────────────────────────────────────────────────────────────── */

typedef struct {
  WspCaptureOptions capture;
  gboolean show_windows;
  gboolean capture_on_leave;
  gboolean refresh_on_hover;
  double refresh_interval;
} Config;

static JsonNode* parse_value(const char* json) {
  JsonParser* parser = json_parser_new();
  JsonNode* node =
      json_parser_load_from_data(parser, json, -1, NULL) ? json_node_copy(json_parser_get_root(parser)) : NULL;
  g_object_unref(parser);
  return node;
}

static gboolean value_bool(JsonNode* node, gboolean fallback) {
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN
             ? json_node_get_boolean(node)
             : fallback;
}

static double value_number(JsonNode* node, double fallback) {
  if (!node || !JSON_NODE_HOLDS_VALUE(node)) return fallback;
  GType type = json_node_get_value_type(node);
  return type == G_TYPE_INT64 || type == G_TYPE_DOUBLE ? json_node_get_double(node) : fallback;
}

static const char* value_string(JsonNode* node) {
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
             ? json_node_get_string(node)
             : NULL;
}

static char* expand_home(const char* path) {
  if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) return g_build_filename(g_get_home_dir(), path + 1, NULL);
  return g_strdup(path);
}

static void read_config(Config* config, const wbcffi_config_entry* entries, size_t count) {
  config->capture.grim = g_strdup("grim");
  config->capture.width = 320;
  gdk_rgba_parse(&config->capture.background, "transparent");
  config->capture.wallpaper_fit = WSP_FIT_FILL;
  config->capture_on_leave = TRUE;
  config->refresh_on_hover = TRUE;
  config->refresh_interval = 1.5;
  for (size_t i = 0; i < count; i++) {
    const char* key = entries[i].key;
    JsonNode* value = parse_value(entries[i].value);
    const char* text = value_string(value);
    if (strcmp(key, "width") == 0) {
      config->capture.width = CLAMP((int)value_number(value, 320), 64, 1920);
    } else if (strcmp(key, "windows") == 0) {
      config->show_windows = value_bool(value, FALSE);
    } else if (strcmp(key, "capture-on-leave") == 0) {
      config->capture_on_leave = value_bool(value, TRUE);
    } else if (strcmp(key, "refresh-on-hover") == 0) {
      config->refresh_on_hover = value_bool(value, TRUE);
    } else if (strcmp(key, "refresh-interval") == 0) {
      config->refresh_interval = MAX(0.0, value_number(value, 1.5));
    } else if (strcmp(key, "grim") == 0 && text && *text) {
      g_free(config->capture.grim);
      config->capture.grim = expand_home(text);
    } else if (strcmp(key, "background") == 0 && text) {
      if (!gdk_rgba_parse(&config->capture.background, text)) {
        g_warning("waybar-workspace-preview: \"background\": cannot read colour \"%s\"", text);
      }
    } else if (strcmp(key, "wallpaper") == 0 && text && *text) {
      char* path = expand_home(text);
      GError* error = NULL;
      g_clear_object(&config->capture.wallpaper);
      config->capture.wallpaper = gdk_pixbuf_new_from_file(path, &error);
      if (!config->capture.wallpaper) {
        g_warning("waybar-workspace-preview: \"wallpaper\": %s", error->message);
        g_error_free(error);
      }
      g_free(path);
    } else if (strcmp(key, "wallpaper-fit") == 0 && text) {
      config->capture.wallpaper_fit = strcmp(text, "fit") == 0       ? WSP_FIT_FIT
                                      : strcmp(text, "stretch") == 0 ? WSP_FIT_STRETCH
                                      : strcmp(text, "center") == 0  ? WSP_FIT_CENTER
                                                                     : WSP_FIT_FILL;
    }
    if (value) json_node_unref(value);
  }
}

static void config_clear(Config* config) {
  g_free(config->capture.grim);
  g_clear_object(&config->capture.wallpaper);
}

/* ── the preview widget ─────────────────────────────────────────────────────────────────────────────── */

static void add_class(GtkWidget* widget, const char* name) {
  gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

static GtkWidget* window_row(WspStore* store, const WspWindow* window, int width) {
  char *name, *icon, *title;
  wsp_apps_describe(wsp_store_apps(store), window, &name, &icon, &title);
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  add_class(row, "workspace-preview-window");
  GIcon* gicon = *icon ? g_icon_new_for_string(icon, NULL) : NULL;
  GtkIconInfo* info = gicon ? gtk_icon_theme_lookup_by_gicon(gtk_icon_theme_get_default(), gicon, ICON_PIXELS,
                                                             GTK_ICON_LOOKUP_FORCE_SIZE)
                            : NULL;
  if (info) { /* only icons the theme really has: no "missing image" placeholders */
    GtkWidget* image = gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(image), ICON_PIXELS);
    gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 0);
    g_object_unref(info);
  }
  g_clear_object(&gicon);
  char* markup = *title ? g_markup_printf_escaped("<b>%s</b>  <span alpha=\"70%%\">%s</span>", name, title)
                        : g_markup_printf_escaped("<b>%s</b>", name);
  GtkWidget* label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  /* No wider than the thumbnail (less the icon): long titles end in "…" instead of widening the tooltip. */
  gtk_label_set_max_width_chars(GTK_LABEL(label), MAX(12, (width - ICON_PIXELS - 8) / CHAR_PIXELS));
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
  g_free(markup);
  g_free(name);
  g_free(icon);
  g_free(title);
  return row;
}

/* The tooltip content for @workspace, or NULL when there is nothing to show (Waybar's own tooltip then). */
static GtkWidget* build_preview(Bar* bar, const char* workspace, const WspThumbnail* thumb, int scale) {
  GPtrArray* windows = bar->show_windows ? wsp_store_windows(bar->store, workspace) : NULL;
  if (!thumb && !(bar->show_windows && windows && windows->len)) return NULL;
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  add_class(box, "workspace-preview");
  if (thumb) {
    /* Captured at the output's scale: draw it at that scale so it stays sharp. */
    cairo_surface_t* surface = gdk_cairo_surface_create_from_pixbuf(thumb->pixbuf, MAX(thumb->scale, scale), NULL);
    GtkWidget* image = gtk_image_new_from_surface(surface);
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER); /* its frame (CSS border) hugs the picture */
    add_class(image, "workspace-preview-image");
    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    cairo_surface_destroy(surface);
  }
  for (guint i = 0; windows && i < windows->len; i++) {
    gtk_box_pack_start(GTK_BOX(box), window_row(bar->store, g_ptr_array_index(windows, i), bar->width), FALSE, FALSE,
                       0);
  }
  gtk_widget_show_all(box);
  return g_object_ref_sink(box);
}

static GtkWidget* preview_for(Bar* bar, const char* workspace, int scale) {
  const WspThumbnail* thumb = wsp_store_thumbnail(bar->store, workspace);
  guint thumb_serial = thumb ? thumb->serial : 0;
  guint windows_serial = bar->show_windows ? wsp_store_windows_serial(bar->store) : 0;
  Tooltip* cached = g_hash_table_lookup(bar->tooltips, workspace);
  if (cached && cached->thumb_serial == thumb_serial && cached->windows_serial == windows_serial &&
      cached->scale == scale) {
    return cached->widget;
  }
  Tooltip* tooltip = g_new0(Tooltip, 1);
  tooltip->widget = build_preview(bar, workspace, thumb, scale);
  tooltip->thumb_serial = thumb_serial;
  tooltip->windows_serial = windows_serial;
  tooltip->scale = scale;
  g_hash_table_replace(bar->tooltips, g_strdup(workspace), tooltip);
  return tooltip->widget;
}

/* ── the workspace buttons ──────────────────────────────────────────────────────────────────────────── */

static const char* first_label_text(GtkWidget* widget) {
  if (GTK_IS_LABEL(widget)) return gtk_label_get_text(GTK_LABEL(widget));
  if (!GTK_IS_CONTAINER(widget)) return NULL;
  GList* children = gtk_container_get_children(GTK_CONTAINER(widget));
  const char* text = NULL;
  for (GList* l = children; l && !text; l = l->next) text = first_label_text(l->data);
  g_list_free(children);
  return text;
}

static const char* workspace_of(Bar* bar, GtkWidget* button) {
  GList* classes = gtk_style_context_list_classes(gtk_widget_get_style_context(button));
  GPtrArray* names = g_ptr_array_new();
  for (GList* l = classes; l; l = l->next) g_ptr_array_add(names, l->data);
  g_ptr_array_add(names, NULL);
  const char* workspace = wsp_store_workspace_for_button(bar->store, (char**)names->pdata, first_label_text(button));
  g_ptr_array_unref(names);
  g_list_free(classes);
  return workspace;
}

/* Runs before Waybar's own tooltip for the button (its "tooltips" text): TRUE shows the preview instead. */
static gboolean query_tooltip(GtkWidget* button, gint x, gint y, gboolean keyboard, GtkTooltip* tooltip,
                              gpointer data) {
  (void)x;
  (void)y;
  Bar* bar = data;
  if (keyboard) return FALSE;
  const char* workspace = workspace_of(bar, button);
  if (!workspace) return FALSE;
  int scale = gtk_widget_get_scale_factor(button);
  wsp_store_require_scale(bar->store, scale);
  if (bar->refresh_age_us >= 0) wsp_store_refresh(bar->store, workspace, bar->refresh_age_us);
  GtkWidget* preview = preview_for(bar, workspace, scale);
  if (!preview) return FALSE;
  gtk_tooltip_set_custom(tooltip, preview);
  return TRUE;
}

/* Waybar turns a button's tooltip off when its own tooltip text is empty ("tooltip": false); keep it on so
 * the preview still shows. */
static void keep_tooltip_on(GtkWidget* button, GParamSpec* pspec, gpointer data) {
  (void)pspec;
  (void)data;
  if (!gtk_widget_get_has_tooltip(button)) gtk_widget_set_has_tooltip(button, TRUE);
}

/* Hooks every button; runs again whenever the module lays its buttons out, which is when Waybar adds one. */
static void hook_buttons(Bar* bar) {
  if (!bar->workspaces) return;
  GList* children = gtk_container_get_children(GTK_CONTAINER(bar->workspaces));
  for (GList* l = children; l; l = l->next) {
    GtkWidget* button = l->data;
    if (g_object_get_data(G_OBJECT(button), HOOKED_KEY)) continue;
    g_object_set_data(G_OBJECT(button), HOOKED_KEY, GINT_TO_POINTER(1));
    gtk_widget_set_has_tooltip(button, TRUE);
    g_signal_connect(button, "query-tooltip", G_CALLBACK(query_tooltip), bar);
    g_signal_connect(button, "notify::has-tooltip", G_CALLBACK(keep_tooltip_on), bar);
  }
  g_list_free(children);
}

static void laid_out(GtkWidget* box, GdkRectangle* allocation, gpointer data) {
  (void)box;
  (void)allocation;
  hook_buttons(data);
}

static void changed(const char* workspace, gpointer data) {
  (void)workspace;
  Bar* bar = data;
  if (bar->workspaces) gtk_widget_trigger_tooltip_query(bar->workspaces); /* the hovered preview, updated */
}

typedef struct {
  const char* name;
  GtkWidget* found;
} Search;

static void search(GtkWidget* widget, gpointer data) {
  Search* s = data;
  if (s->found) return;
  if (g_strcmp0(gtk_widget_get_name(widget), s->name) == 0 && GTK_IS_CONTAINER(widget)) {
    s->found = widget;
  } else if (GTK_IS_CONTAINER(widget)) {
    gtk_container_forall(GTK_CONTAINER(widget), search, s);
  }
}

static gboolean hook(gpointer data) {
  Bar* bar = data;
  GtkWidget* top = gtk_widget_get_toplevel(bar->root);
  if (gtk_widget_is_toplevel(top)) {
    Search s = {"workspaces", NULL};
    gtk_container_forall(GTK_CONTAINER(top), search, &s);
    if (s.found) {
      bar->workspaces = s.found;
      g_object_add_weak_pointer(G_OBJECT(bar->workspaces), (gpointer*)&bar->workspaces);
      bar->layout_handler = g_signal_connect(bar->workspaces, "size-allocate", G_CALLBACK(laid_out), bar);
      hook_buttons(bar);
      bar->hook_source = 0;
      return G_SOURCE_REMOVE;
    }
  }
  if (++bar->hook_tries >= HOOK_TRIES) {
    g_warning("waybar-workspace-preview: this bar has no hyprland/workspaces module; nothing to preview");
    bar->hook_source = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

/* ── Waybar's module interface ──────────────────────────────────────────────────────────────────────── */

WSP_EXPORT void* wbcffi_init(const wbcffi_init_info* init_info, const wbcffi_config_entry* config_entries,
                             size_t config_entries_len) {
  Config config = {0};
  read_config(&config, config_entries, config_entries_len);
  char* grim = g_find_program_in_path(config.capture.grim);
  if (!grim) {
    g_warning("waybar-workspace-preview: \"%s\" not found: install grim 1.5 or later (or set \"grim\")",
              config.capture.grim);
  }
  g_free(grim);

  Bar* bar = g_new0(Bar, 1);
  bar->root = GTK_WIDGET(init_info->get_root_widget(init_info->obj));
  bar->show_windows = config.show_windows;
  bar->width = config.capture.width;
  bar->refresh_age_us = config.refresh_on_hover ? (gint64)(config.refresh_interval * G_USEC_PER_SEC) : -1;
  bar->tooltips = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)tooltip_free);
  bar->store = wsp_store_acquire(&config.capture, config.capture_on_leave);
  config_clear(&config);
  if (!bar->store) {
    g_warning("waybar-workspace-preview: Hyprland is not running; nothing to preview");
    return bar; /* inert; Waybar still loads the bar */
  }
  bar->watch = wsp_store_watch(bar->store, changed, bar);
  bar->hook_source = g_timeout_add(HOOK_INTERVAL_MS, hook, bar);
  return bar;
}

WSP_EXPORT void wbcffi_deinit(void* instance) {
  Bar* bar = instance;
  if (bar->hook_source) g_source_remove(bar->hook_source);
  if (bar->workspaces) {
    g_signal_handler_disconnect(bar->workspaces, bar->layout_handler);
    GList* children = gtk_container_get_children(GTK_CONTAINER(bar->workspaces));
    for (GList* l = children; l; l = l->next) {
      g_signal_handlers_disconnect_by_data(l->data, bar);
      g_object_set_data(G_OBJECT(l->data), HOOKED_KEY, NULL);
    }
    g_list_free(children);
    g_object_remove_weak_pointer(G_OBJECT(bar->workspaces), (gpointer*)&bar->workspaces);
  }
  if (bar->store) {
    wsp_store_unwatch(bar->store, bar->watch);
    wsp_store_release(bar->store);
  }
  g_hash_table_destroy(bar->tooltips);
  g_free(bar);
}
