#include "window_frame.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// libhandy's corner masking copies frames off the GPU (ubuntu/app-center#2080).

static constexpr char kChannelName[] = "handy_window/window_frame";
static constexpr char kDataKey[] = "handy-window-frame";

// Corners may reach this far past the radius; libhandy's rim goes 1px past.
static constexpr int kMaxCornerOverhang = 4;

// Extremes and mid grey, plus Yaru's orange and dark greys.
static constexpr uint32_t kRimTestBackdrops[] = {
    0xff000000, 0xffffffff, 0xff808080, 0xffe95420, 0xff252525, 0xff3d3d3d};

// A square image in cairo's ARGB32: native-endian, premultiplied.
struct Pixels {
  int size = 0;
  std::vector<uint32_t> data;

  uint32_t at(int x, int y) const { return data[y * size + x]; }
};

struct WindowFrame {
  GtkWindow* window;
  FlView* view;
  FlMethodChannel* channel;
  // Styled like libhandy's private decoration and decoration-overlay contexts.
  GtkStyleContext* decoration = nullptr;
  GtkStyleContext* rim = nullptr;
  bool had_first_frame = false;
  // nullptr unless enabled; a null value while libhandy wouldn't mask, so that
  // it draws the window as usual.
  FlValue* style = nullptr;
};

// pixman's x * y / 255, rounded; the Dart side checks the GPU rounds alike.
static uint32_t mul(uint32_t x, uint32_t y) {
  uint32_t t = x * y + 0x80;
  return ((t >> 8) + t) >> 8;
}

static uint32_t over(uint32_t source, uint32_t destination) {
  uint32_t inverse_alpha = 255 - (source >> 24);
  uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    uint32_t channel = ((source >> shift) & 0xff) +
                       mul((destination >> shift) & 0xff, inverse_alpha);
    result |= channel << shift;
  }
  return result;
}

// Draws `context` over `backdrop` the way libhandy draws its nodes.
static bool render(GtkStyleContext* context,
                   int size,
                   int scale,
                   uint32_t backdrop,
                   Pixels* pixels) {
  pixels->size = size * scale;
  pixels->data.assign(pixels->size * pixels->size, backdrop);
  // ARGB32 rows have no padding, so cairo can draw straight into `pixels`.
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(pixels->data.data()),
      CAIRO_FORMAT_ARGB32, pixels->size, pixels->size,
      pixels->size * sizeof(uint32_t));
  cairo_surface_set_device_scale(surface, scale, scale);
  cairo_t* cr = cairo_create(surface);
  gtk_render_background(context, cr, 0, 0, size, size);
  gtk_render_frame(context, cr, 0, 0, size, size);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  bool rendered = cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
  cairo_surface_destroy(surface);
  return rendered;
}

// Set up like libhandy's create_child_context() and update_child_context().
static GtkStyleContext* new_child_context(GtkWidget* window) {
  GtkStyleContext* parent = gtk_widget_get_style_context(window);
  GtkStyleContext* child = gtk_style_context_new();
  gtk_style_context_set_parent(child, parent);
  gtk_style_context_set_screen(child, gtk_style_context_get_screen(parent));
  return child;
}

static void update_child_context(GtkWidget* window,
                                 GtkStyleContext* child,
                                 const char* name) {
  GtkWidgetPath* path = gtk_widget_path_new();
  gtk_widget_path_append_for_widget(path, window);
  gint position = gtk_widget_path_append_type(path, GTK_TYPE_WIDGET);
  gtk_widget_path_iter_set_object_name(path, position, name);
  gtk_style_context_set_path(child, path);
  gtk_widget_path_unref(path);
  gtk_style_context_set_state(
      child, gtk_style_context_get_state(gtk_widget_get_style_context(window)));
}

// The top-left radius, which is the one libhandy reads.
static int border_radius(GtkStyleContext* context) {
  int radius = 0;
  gtk_style_context_get(context, gtk_style_context_get_state(context),
                        GTK_STYLE_PROPERTY_BORDER_RADIUS, &radius, nullptr);
  return radius;
}

// Whether Canvas.drawImageNine would stretch atlas_rgba(pixels, inset) into
// `target`.
static bool stretches_to(const Pixels& pixels,
                         int inset,
                         const Pixels& target) {
  auto source = [&](int i) {
    if (i < inset) {
      return i;
    }
    if (i >= target.size - inset) {
      return i - target.size + pixels.size;
    }
    return inset;
  };
  for (int y = 0; y < target.size; y++) {
    for (int x = 0; x < target.size; x++) {
      if (target.at(x, y) != pixels.at(source(x), source(y))) {
        return false;
      }
    }
  }
  return true;
}

// The nine-slice atlas of `pixels`, with corners `inset` wide, as RGBA.
static FlValue* atlas_rgba(const Pixels& pixels, int inset) {
  int atlas_size = 2 * inset + 1;
  auto source = [&](int i) {
    return i <= inset ? i : i - atlas_size + pixels.size;
  };
  std::vector<uint8_t> bytes;
  for (int y = 0; y < atlas_size; y++) {
    for (int x = 0; x < atlas_size; x++) {
      uint32_t pixel = pixels.at(source(x), source(y));
      bytes.push_back((pixel >> 16) & 0xff);
      bytes.push_back((pixel >> 8) & 0xff);
      bytes.push_back(pixel & 0xff);
      bytes.push_back(pixel >> 24);
    }
  }
  return fl_value_new_uint8_list(bytes.data(), bytes.size());
}

// libhandy's create_masks(), as the alpha of one RGBA image whose quadrants
// are the corners in reading order.
static FlValue* corner_masks(int radius, int scale) {
  int size = radius * scale;
  double correction = 0.5 / scale;
  double r = radius - correction;
  std::vector<uint8_t> bytes(4 * 2 * size * 2 * size);
  for (int i = 0; i < 4; i++) {
    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_A8, size, size);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    cairo_arc(cr, i % 2 == 0 ? r : correction, i / 2 == 0 ? r : correction, r,
              0, G_PI * 2);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        int position = (i / 2 * size + y) * 2 * size + i % 2 * size + x;
        bytes[4 * position + 3] = data[y * stride + x];
      }
    }
    cairo_surface_destroy(surface);
  }
  return fl_value_new_uint8_list(bytes.data(), bytes.size());
}

// Dart blends the rim as one image; GTK's layers must blend the same way.
static bool rim_composites_exactly(GtkStyleContext* rim_context,
                                   int size,
                                   int scale,
                                   const Pixels& rim) {
  for (uint32_t backdrop : kRimTestBackdrops) {
    Pixels composited;
    if (!render(rim_context, size, scale, backdrop, &composited)) {
      return false;
    }
    for (size_t i = 0; i < composited.data.size(); i++) {
      if (composited.data[i] != over(rim.data[i], backdrop)) {
        return false;
      }
    }
  }
  return true;
}

// Dart paints only inside the view, so it must cover all that libhandy masks.
static bool view_fills_window(WindowFrame* self) {
  GtkWidget* window = GTK_WIDGET(self->window);
  GtkWidget* content = gtk_bin_get_child(GTK_BIN(window));
  GtkWidget* titlebar = gtk_window_get_titlebar(self->window);
  GtkWidget* view = GTK_WIDGET(self->view);
  int content_x, content_y, view_x, view_y;
  return content != nullptr &&
         (titlebar == nullptr || !gtk_widget_get_visible(titlebar)) &&
         gtk_widget_translate_coordinates(content, window, 0, 0, &content_x,
                                          &content_y) &&
         gtk_widget_translate_coordinates(view, window, 0, 0, &view_x,
                                          &view_y) &&
         view_x == content_x && view_y == content_y &&
         gtk_widget_get_allocated_width(view) ==
             gtk_widget_get_allocated_width(content) &&
         gtk_widget_get_allocated_height(view) ==
             gtk_widget_get_allocated_height(content);
}

// Dart matches pixman, which composites image surfaces; X11 uses XRender.
static bool window_surface_is_image(GdkWindow* gdk_window) {
  cairo_surface_t* surface = gdk_window_create_similar_surface(
      gdk_window, CAIRO_CONTENT_COLOR_ALPHA, 1, 1);
  bool is_image = cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE;
  cairo_surface_destroy(surface);
  return is_image;
}

// 3.24.39-41 skip GL blending, which wipes the corner shadow: GNOME/gtk#6359.
static bool gtk_blends_gl_textures() {
  guint micro = gtk_get_micro_version();
  return gtk_get_minor_version() != 24 || micro < 39 || micro > 41;
}

// nullptr if Dart can't paint the current style exactly; a null value while
// libhandy wouldn't mask.
static FlValue* compute_style(WindowFrame* self) {
  GtkWidget* window = GTK_WIDGET(self->window);
  // libhandy updates its own contexts on the same signals.
  update_child_context(window, self->decoration, "decoration");
  update_child_context(window, self->rim, "decoration-overlay");
  GdkWindow* gdk_window = gtk_widget_get_window(window);
  if (gdk_window == nullptr || !window_surface_is_image(gdk_window) ||
      !gtk_blends_gl_textures() || !view_fills_window(self)) {
    return nullptr;
  }
  GtkStyleContext* context = gtk_widget_get_style_context(window);
  int radius = border_radius(context);
  bool fullscreen =
      gdk_window_get_state(gdk_window) & GDK_WINDOW_STATE_FULLSCREEN;
  // libhandy's conditions for masking, from hdy_window_mixin_draw().
  if (!gtk_window_get_decorated(self->window) ||
      gtk_style_context_has_class(context, "solid-csd") || fullscreen ||
      gtk_window_is_maximized(self->window) || radius <= 0) {
    return fl_value_new_null();
  }

  int scale = gdk_window_get_scale_factor(gdk_window);
  // Rendered at two sizes, so that the slicing can't depend on the size.
  int small_size = 2 * (radius + kMaxCornerOverhang) + 1;
  int large_size = 2 * small_size + 3;
  Pixels background, large_background, rim, large_rim;
  if (!render(context, small_size, scale, 0, &background) ||
      !render(context, large_size, scale, 0, &large_background) ||
      !render(self->rim, small_size, scale, 0, &rim) ||
      !render(self->rim, large_size, scale, 0, &large_rim) ||
      !rim_composites_exactly(self->rim, small_size, scale, rim)) {
    return nullptr;
  }
  for (int inset = 0; inset <= (radius + kMaxCornerOverhang) * scale; inset++) {
    if (!stretches_to(background, inset, background) ||
        !stretches_to(background, inset, large_background) ||
        !stretches_to(rim, inset, rim) ||
        !stretches_to(rim, inset, large_rim)) {
      continue;
    }
    FlValue* style = fl_value_new_map();
    fl_value_set_string_take(style, "scale", fl_value_new_int(scale));
    fl_value_set_string_take(style, "inset", fl_value_new_int(inset));
    fl_value_set_string_take(style, "background",
                             atlas_rgba(background, inset));
    fl_value_set_string_take(style, "rim", atlas_rgba(rim, inset));
    fl_value_set_string_take(style, "radius", fl_value_new_int(radius * scale));
    fl_value_set_string_take(style, "masks", corner_masks(radius, scale));
    return style;
  }
  return nullptr;
}

// libhandy's draw minus its offscreen pass, whose window background, rim and
// corner masks Dart paints instead.
static gboolean draw_cb(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
  auto* self = static_cast<WindowFrame*>(user_data);
  if (!self->had_first_frame || self->style == nullptr ||
      fl_value_get_type(self->style) == FL_VALUE_TYPE_NULL) {
    return FALSE;
  }
  if (gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget))) {
    // libhandy's get_shadow_width(); view_fills_window() rules out a titlebar.
    GtkAllocation alloc, content_alloc;
    gtk_widget_get_allocation(widget, &alloc);
    gtk_widget_get_allocation(gtk_bin_get_child(GTK_BIN(widget)),
                              &content_alloc);
    GtkBorder margin;
    gtk_style_context_get_margin(self->decoration,
                                 gtk_style_context_get_state(self->decoration),
                                 &margin);
    int left = std::max<int>(content_alloc.x - alloc.x, margin.left);
    int top = std::max<int>(content_alloc.y - alloc.y, margin.top);
    int right = std::max<int>(
        alloc.width - content_alloc.width - content_alloc.x, margin.right);
    int bottom = std::max<int>(
        alloc.height - content_alloc.height - content_alloc.y, margin.bottom);
    int width = alloc.width - left - right;
    int height = alloc.height - top - bottom;
    gtk_render_background(self->decoration, cr, left, top, width, height);
    gtk_render_frame(self->decoration, cr, left, top, width, height);
  }
  // Children only: GtkWindow's draw would paint the window background under
  // Dart's corners.
  GTK_WIDGET_CLASS(g_type_class_peek(GTK_TYPE_CONTAINER))->draw(widget, cr);
  return TRUE;
}

// Returns the corners to libhandy for the rest of the session.
static void give_back(WindowFrame* self) {
  // FlView can't report its old background, so the view stays transparent.
  g_clear_pointer(&self->style, fl_value_unref);
  fl_method_channel_invoke_method(self->channel, "styleChanged", nullptr,
                                  nullptr, nullptr, nullptr);
}

static void style_changed(WindowFrame* self) {
  if (self->style == nullptr) {
    return;
  }
  FlValue* style = compute_style(self);
  if (style == nullptr) {
    give_back(self);
    return;
  }
  if (fl_value_equal(style, self->style)) {
    fl_value_unref(style);
    return;
  }
  fl_value_unref(self->style);
  self->style = style;
  fl_method_channel_invoke_method(self->channel, "styleChanged", style,
                                  nullptr, nullptr, nullptr);
}

static gboolean window_state_event_cb(GtkWidget*,
                                      GdkEventWindowState*,
                                      gpointer user_data) {
  style_changed(static_cast<WindowFrame*>(user_data));
  return FALSE;
}

static void size_allocated(WindowFrame* self) {
  if (self->style != nullptr && !view_fills_window(self)) {
    give_back(self);
  }
}

static void enable(WindowFrame* self) {
  GtkWidget* window = GTK_WIDGET(self->window);
  self->decoration = new_child_context(window);
  self->rim = new_child_context(window);
  // Dart can ask before GTK's first layout, when the view has no size yet.
  gtk_container_check_resize(GTK_CONTAINER(window));
  self->style = compute_style(self);
  if (self->style == nullptr) {
    return;
  }
  // Until first-frame, libhandy could cut Dart's corners twice; there's no
  // frame to lose by not drawing the view.
  g_signal_connect(self->view, "draw", G_CALLBACK(gtk_true), self);
  // Ahead of libhandy's draw, which it skips while Dart paints the corners.
  g_signal_connect(window, "draw", G_CALLBACK(draw_cb), self);
  // These run after GTK's own handlers, which restyle and lay out the window.
  g_signal_connect_swapped(window, "style-updated", G_CALLBACK(style_changed),
                           self);
  g_signal_connect_after(window, "window-state-event",
                         G_CALLBACK(window_state_event_cb), self);
  g_signal_connect_swapped(window, "notify::scale-factor",
                           G_CALLBACK(style_changed), self);
  g_signal_connect_swapped(window, "size-allocate", G_CALLBACK(size_allocated),
                           self);
}

static void method_call_cb(FlMethodChannel*,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  auto* self = static_cast<WindowFrame*>(user_data);
  const gchar* method = fl_method_call_get_name(method_call);
  if (strcmp(method, "enable") == 0) {
    // Only once, and too late after the first frame: it was painted for
    // libhandy's path.
    if (self->decoration == nullptr && !self->had_first_frame) {
      enable(self);
    }
    g_autoptr(FlValue) result = self->style != nullptr
                                    ? fl_value_ref(self->style)
                                    : fl_value_new_null();
    fl_method_call_respond_success(method_call, result, nullptr);
  } else if (strcmp(method, "disable") == 0) {
    give_back(self);
    fl_method_call_respond_success(method_call, nullptr, nullptr);
  } else {
    fl_method_call_respond_not_implemented(method_call, nullptr);
  }
}

static void first_frame_cb(FlView* view, gpointer user_data) {
  auto* self = static_cast<WindowFrame*>(user_data);
  self->had_first_frame = true;
  // This handler and enable()'s draw block, the only ones with this data.
  g_signal_handlers_disconnect_by_data(view, self);
  if (self->style != nullptr) {
    // An opaque view background would fill the corners Dart cuts away.
    const GdkRGBA transparent{};
    fl_view_set_background_color(view, &transparent);
  }
}

static void window_frame_free(gpointer data) {
  auto* self = static_cast<WindowFrame*>(data);
  g_object_unref(self->channel);
  g_clear_object(&self->decoration);
  g_clear_object(&self->rim);
  g_clear_pointer(&self->style, fl_value_unref);
  delete self;
}

void window_frame_attach(GtkWindow* window, FlView* view) {
  auto* self = new WindowFrame();
  self->window = window;
  self->view = view;
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  self->channel = fl_method_channel_new(
      fl_engine_get_binary_messenger(fl_view_get_engine(view)), kChannelName,
      FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(self->channel, method_call_cb,
                                            self, nullptr);
  g_signal_connect(view, "first-frame", G_CALLBACK(first_frame_cb), self);
  g_object_set_data_full(G_OBJECT(window), kDataKey, self, window_frame_free);
}
