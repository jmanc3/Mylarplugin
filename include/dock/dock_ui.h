#pragma once

#include "dock/dock_types.h"
#include "hypriso.h"

#include <cairo.h>
#include <functional>
#include <string>

// Shared dock colours. Popups and dock controls must agree on these.
extern RGBA accent;
extern RGBA border_color;

float get_icon_size(float dpi);

RawWindowSettings make_icon_anchored_popup_settings(Container *icon,
                                                    float dpi,
                                                    int popup_w,
                                                    int popup_h);

Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size, bool draw, std::string font,
                 int wrap, int h, RGBA color, bool bold, int align = 0);
Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size = 10, bool draw = true,
                 std::string font = set->font, int wrap = -1, int h = -1, RGBA color = {1, 1, 1, 1});
Bounds draw_text(cairo_t *cr, Container *c, std::string text, int size = 10, bool draw = true,
                 std::string font = set->font, int wrap = -1, int h = -1);

void drawRoundedRect(cairo_t *cr, double x, double y, double width, double height, double radius,
                     double stroke_width);

void paint_popup_background(cairo_t *cr, const Bounds &bounds, double dpi,
                            RGBA background = {1, 1, 1, 1}, RGBA outline = border_color);
// Intersects the current clip with the inside of the popup's rounded outline.
// Callers wrap it in cairo_save / cairo_restore.
void clip_to_popup_background(cairo_t *cr, const Bounds &bounds, double dpi);

// `meter` (0..1) draws a live level inside the filled part of the track, up to the knob.
void paint_slider(Container *root, Container *c, cairo_t *cr, float dpi, float value, float meter = 0);

Container *make_self_sizing_slider(Container *root,
                                   std::function<std::string(Container *)> left_text,
                                   std::function<std::string(Container *)> right_text,
                                   std::function<void(Container *, float)> on_value_change,
                                   std::function<float(Container *)> get_value,
                                   std::function<MylarWindow *(Dock *)> get_window);

Container *make_self_sizing_label(Container *root, std::string text, int size,
                                  std::function<MylarWindow *(Dock *)> get_window);
