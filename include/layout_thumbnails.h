#ifndef layout_thumbnails_h_INCLUDED
#define layout_thumbnails_h_INCLUDED

#include "container.h"
#include <vector>

struct Item {
    float aspectRatio;   // width / height
    float width;
    float height;
};

struct DensityPreset {
    int maxItemCount;
    int targetHeight;
};

struct LayoutResult {
    std::vector<Bounds> items;
    Bounds bounds;
};

struct LayoutParams {
    int availableWidth;
    int availableHeight;
    int horizontalSpacing;
    int verticalSpacing;
    int margin;
    int maxThumbWidth;

    std::vector<DensityPreset> densityPresets;
};

LayoutResult layoutAltTabThumbnails(const LayoutParams& params, const std::vector<Item>& items); 

#pragma once
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>

/* ============================================================
   Basic geometry (Qt-independent)
   ============================================================ */

struct Size {
    int w = 0, h = 0;
};

struct Point {
    int x = 0, y = 0;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    int left()   const { return x; }
    int right()  const { return x + w; }
    int top()    const { return y; }
    int bottom() const { return y + h; }

    Point center() const {
        return { x + w / 2, y + h / 2 };
    }

    bool intersects(const Rect& o) const {
        return !(right() <= o.left() ||
                 left() >= o.right() ||
                 bottom() <= o.top() ||
                 top() >= o.bottom());
    }

    Rect translated(int dx, int dy) const {
        return { x + dx, y + dy, w, h };
    }

    void translate(int dx, int dy) {
        x += dx; y += dy;
    }

    Rect united(const Rect& o) const {
        int nx = std::min(x, o.x);
        int ny = std::min(y, o.y);
        int nr = std::max(right(), o.right());
        int nb = std::max(bottom(), o.bottom());
        return { nx, ny, nr - nx, nb - ny };
    }
};

/* ============================================================
   Cell interface (user implements this)
   ============================================================ */

struct ExpoCell {
    virtual ~ExpoCell() = default;

    virtual int naturalX() const = 0;
    virtual int naturalY() const = 0;
    virtual int naturalWidth() const = 0;
    virtual int naturalHeight() const = 0;

    virtual int persistentKey() const = 0;

    // Output (you apply these to your renderer / UI)
    virtual void setRect(const Rect& r) = 0;
};

class DemoCell final : public ExpoCell {
public:
    DemoCell(int id, int x, int y, int w, int h)
        : m_id(id), m_x(x), m_y(y), m_w(w), m_h(h) {}

    // Natural geometry
    int naturalX() const override      { return m_x; }
    int naturalY() const override      { return m_y; }
    int naturalWidth() const override  { return m_w; }
    int naturalHeight() const override { return m_h; }

    int persistentKey() const override { return m_id; }

    // Layout output
    void setRect(const Rect& r) override {
        m_result = r;
    }

    const Rect& result() const { return m_result; }

private:
    int  m_id;
    int  m_x, m_y;
    int  m_w, m_h;
    Rect m_result;
};

/* ============================================================
   Expo layout engine
   ============================================================ */

class ExpoLayout {
public:
    int spacing  = 20;
    int accuracy = 15;
    bool fillGaps = true;

    void setAreaSize(int w, int h) {
        m_area = { 0, 0, w, h };
    }

    void setCells(const std::vector<ExpoCell*>& cells) {
        m_cells = cells;
    }

    void calculate() {
        if (m_cells.empty()) return;

        std::sort(m_cells.begin(), m_cells.end(),
            [](const ExpoCell* a, const ExpoCell* b) {
                return a->persistentKey() < b->persistentKey();
            });

        std::unordered_map<ExpoCell*, Rect> targets;
        Rect bounds;
        bool first = true;

        for (auto* c : m_cells) {
            Rect r {
                c->naturalX(),
                c->naturalY(),
                c->naturalWidth(),
                c->naturalHeight()
            };
            targets[c] = r;
            bounds = first ? r : bounds.united(r);
            first = false;
        }

        // Push overlapping windows apart
        bool overlap;
        int times = 0;
        do {
            overlap = false;
            for (auto* a : m_cells) {
                for (auto* b : m_cells) {
                    if (a == b) continue;

                    Rect& ra = targets[a];
                    Rect& rb = targets[b];

                    Rect ea = ra.translated(-spacing/2, -spacing/2);
                    Rect eb = rb.translated(-spacing/2, -spacing/2);

                    ea.w += spacing; ea.h += spacing;
                    eb.w += spacing; eb.h += spacing;

                    if (ea.intersects(eb)) {
                        overlap = true;

                        Point ca = ra.center();
                        Point cb = rb.center();
                        int dx = cb.x - ca.x;
                        int dy = cb.y - ca.y;

                        if (dx == 0 && dy == 0) dx = 1;

                        float len = std::abs(dx) + std::abs(dy);
                        dx = int(dx / len * accuracy);
                        dy = int(dy / len * accuracy);

                        ra.translate(-dx, -dy);
                        rb.translate(dx, dy);

                        bounds = bounds.united(ra).united(rb);
                    }
                }
            }
        } while (overlap && times++ < 300);

        // Scale to fit area
        float sx = float(m_area.w) / bounds.w;
        float sy = float(m_area.h) / bounds.h;
        float scale = std::min({ sx, sy, 1.0f });

        for (auto& it : targets) {
            Rect& r = it.second;
            r.x = int((r.x - bounds.x) * scale);
            r.y = int((r.y - bounds.y) * scale);
            r.w = int(r.w * scale);
            r.h = int(r.h * scale);
        }

        // Center + preserve aspect ratio
        for (auto* c : m_cells) {
            Rect& r = targets[c];
            r = centered(c, r);
            c->setRect(r);
        }
    }

private:
    Rect m_area;
    std::vector<ExpoCell*> m_cells;

    static Rect centered(const ExpoCell* c, const Rect& bounds) {
        float scale = std::min(
            float(bounds.w) / c->naturalWidth(),
            float(bounds.h) / c->naturalHeight()
        );

        int w = int(c->naturalWidth() * scale);
        int h = int(c->naturalHeight() * scale);

        Point center = bounds.center();
        return {
            center.x - w / 2,
            center.y - h / 2,
            w, h
        };
    }
};

 
#endif // layout_thumbnails_h_INCLUDED
