#include <string.h>

#include "wsp.h"

/* Reads the next header token (skipping whitespace and # comments); FALSE at the end of the data. */
static gboolean next_token(const guint8* data, gsize size, gsize* pos, char* out, gsize out_size) {
  while (*pos < size) {
    if (g_ascii_isspace(data[*pos])) {
      (*pos)++;
    } else if (data[*pos] == '#') {
      while (*pos < size && data[*pos] != '\n') (*pos)++;
    } else {
      break;
    }
  }
  gsize len = 0;
  while (*pos < size && !g_ascii_isspace(data[*pos]) && len + 1 < out_size) out[len++] = (char)data[(*pos)++];
  out[len] = '\0';
  return len > 0 && (*pos >= size || g_ascii_isspace(data[*pos]));
}

GdkPixbuf* wsp_ppm_to_pixbuf(GBytes* ppm, GError** error) {
  gsize size = 0;
  const guint8* data = g_bytes_get_data(ppm, &size);
  gsize pos = 0;
  char magic[8], width_s[16], height_s[16], maxval_s[16];
  if (!data || !next_token(data, size, &pos, magic, sizeof magic) || strcmp(magic, "P6") != 0 ||
      !next_token(data, size, &pos, width_s, sizeof width_s) ||
      !next_token(data, size, &pos, height_s, sizeof height_s) ||
      !next_token(data, size, &pos, maxval_s, sizeof maxval_s)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not a binary PPM (P6) image");
    return NULL;
  }
  guint64 width = 0, height = 0, maxval = 0;
  if (!g_ascii_string_to_unsigned(width_s, 10, 1, 1 << 15, &width, NULL) ||
      !g_ascii_string_to_unsigned(height_s, 10, 1, 1 << 15, &height, NULL) ||
      !g_ascii_string_to_unsigned(maxval_s, 10, 1, 65535, &maxval, NULL) || maxval != 255) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unsupported PPM size or depth");
    return NULL;
  }
  pos++; /* the single whitespace character after maxval */
  gsize rowstride = (gsize)width * 3, needed = rowstride * (gsize)height;
  if (pos > size || size - pos < needed) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "PPM image data is cut short");
    return NULL;
  }
  GBytes* pixels = g_bytes_new_from_bytes(ppm, pos, needed); /* no copy */
  GdkPixbuf* pixbuf =
      gdk_pixbuf_new_from_bytes(pixels, GDK_COLORSPACE_RGB, FALSE, 8, (int)width, (int)height, (int)rowstride);
  g_bytes_unref(pixels);
  return pixbuf;
}
