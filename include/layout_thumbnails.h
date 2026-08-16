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

    // ============================================================
    // Collision-based deterministic layout
    // ============================================================

    void calculate() {
        if (m_cells.empty())
            return;

        sortCells();

        std::unordered_map<ExpoCell*, Rect> targets;

        for (auto* c : m_cells) {
            targets[c] = {
                c->naturalX(),
                c->naturalY(),
                c->naturalWidth(),
                c->naturalHeight()
            };
        }

        // --------------------------------------------------------
        // Separate overlapping cells.
        //
        // Each pair is processed exactly once.
        // --------------------------------------------------------

        for (int iteration = 0; iteration < 300; ++iteration) {
            bool changed = false;

            for (size_t i = 0; i < m_cells.size(); ++i) {
                for (size_t j = i + 1; j < m_cells.size(); ++j) {

                    ExpoCell* a = m_cells[i];
                    ExpoCell* b = m_cells[j];

                    Rect& ra = targets[a];
                    Rect& rb = targets[b];

                    Rect ea = ra.translated(
                        -spacing / 2,
                        -spacing / 2
                    );

                    Rect eb = rb.translated(
                        -spacing / 2,
                        -spacing / 2
                    );

                    ea.w += spacing;
                    ea.h += spacing;

                    eb.w += spacing;
                    eb.h += spacing;

                    if (!ea.intersects(eb))
                        continue;

                    changed = true;

                    Point ca = ra.center();
                    Point cb = rb.center();

                    int dx = cb.x - ca.x;
                    int dy = cb.y - ca.y;

                    // Deterministic direction when centers coincide.
                    if (dx == 0 && dy == 0) {
                        dx = 1;
                        dy = 0;
                    }

                    int ax = std::abs(dx);
                    int ay = std::abs(dy);

                    int distance = ax + ay;

                    if (distance == 0)
                        distance = 1;

                    int moveX =
                        (dx * accuracy) / distance;

                    int moveY =
                        (dy * accuracy) / distance;

                    // Guarantee movement.
                    if (moveX == 0 && moveY == 0) {
                        if (ax >= ay)
                            moveX = dx > 0 ? 1 : -1;
                        else
                            moveY = dy > 0 ? 1 : -1;
                    }

                    ra.translate(-moveX, -moveY);
                    rb.translate(moveX, moveY);
                }
            }

            if (!changed)
                break;
        }

        // --------------------------------------------------------
        // Recalculate bounds from FINAL positions.
        // --------------------------------------------------------

        Rect bounds;
        bool first = true;

        for (auto* c : m_cells) {
            const Rect& r = targets[c];

            if (first) {
                bounds = r;
                first = false;
            } else {
                bounds = bounds.united(r);
            }
        }

        if (bounds.w <= 0 || bounds.h <= 0)
            return;

        // --------------------------------------------------------
        // Scale to fit the available area.
        // --------------------------------------------------------

        float sx =
            float(m_area.w) / float(bounds.w);

        float sy =
            float(m_area.h) / float(bounds.h);

        float scale =
            std::min({ sx, sy, 1.0f });

        for (auto* c : m_cells) {
            Rect& r = targets[c];

            r.x = int(
                (r.x - bounds.x) * scale
            );

            r.y = int(
                (r.y - bounds.y) * scale
            );

            r.w = int(r.w * scale);
            r.h = int(r.h * scale);
        }

        // --------------------------------------------------------
        // Preserve aspect ratio.
        // --------------------------------------------------------

        for (auto* c : m_cells) {
            Rect& r = targets[c];

            r = centered(c, r);

            c->setRect(r);
        }
    }


    // ============================================================
    // Deterministic grid layout
    // ============================================================
    //
    // This is completely independent of calculate().
    //
    // The grid is based solely on:
    //
    //   - persistentKey()
    //   - number of cells
    //   - area size
    //   - spacing
    //
    // Natural X/Y positions do NOT influence placement.
    //
    // Same cells + same area = same layout.
    //
    // ============================================================

    void calculateGrid() {
        if (m_cells.empty())
            return;

        if (m_area.w <= 0 || m_area.h <= 0)
            return;

        sortCells();

        const int count =
            static_cast<int>(m_cells.size());

        const int columns =
            calculateGridColumns(count);

        const int rows =
            (count + columns - 1) / columns;

        calculateGridLayout(columns, rows);
    }


private:

    Rect m_area;
    std::vector<ExpoCell*> m_cells;


    // ============================================================
    // Deterministic ordering shared by both algorithms.
    // ============================================================

    void sortCells() {
        std::sort(
            m_cells.begin(),
            m_cells.end(),
            [](const ExpoCell* a, const ExpoCell* b) {

                if (a->persistentKey() !=
                    b->persistentKey()) {

                    return a->persistentKey() <
                           b->persistentKey();
                }

                // Tie breakers in case persistentKey isn't unique.

                if (a->naturalY() != b->naturalY())
                    return a->naturalY() < b->naturalY();

                if (a->naturalX() != b->naturalX())
                    return a->naturalX() < b->naturalX();

                if (a->naturalWidth() != b->naturalWidth())
                    return a->naturalWidth() <
                           b->naturalWidth();

                return a->naturalHeight() <
                       b->naturalHeight();
            }
        );
    }


    // ============================================================
    // Grid column selection.
    // ============================================================

    int calculateGridColumns(int count) const {
        if (count <= 1)
            return 1;

        int bestColumns = 1;

        double bestScore =
            std::numeric_limits<double>::max();

        for (int columns = 1;
             columns <= count;
             ++columns) {

            const int rows =
                (count + columns - 1) / columns;

            const int horizontalSpacing =
                std::max(0, columns - 1) * spacing;

            const int verticalSpacing =
                std::max(0, rows - 1) * spacing;

            const int availableWidth =
                m_area.w - horizontalSpacing;

            const int availableHeight =
                m_area.h - verticalSpacing;

            if (availableWidth <= 0 ||
                availableHeight <= 0) {

                continue;
            }

            const double slotWidth =
                double(availableWidth) /
                double(columns);

            const double slotHeight =
                double(availableHeight) /
                double(rows);

            if (slotWidth <= 0 ||
                slotHeight <= 0) {

                continue;
            }

            const double aspect =
                slotWidth / slotHeight;

            // Prefer approximately square slots.
            const double aspectScore =
                std::abs(std::log(aspect));

            // Slightly penalize unused slots.
            const int totalSlots =
                columns * rows;

            const double waste =
                double(totalSlots - count) /
                double(totalSlots);

            const double score =
                aspectScore + waste * 0.15;

            if (score < bestScore) {
                bestScore = score;
                bestColumns = columns;
            }
        }

        return bestColumns;
    }


    // ============================================================
    // Actually position the grid.
    // ============================================================

    void calculateGridLayout(
        int columns,
        int rows)
    {
        const int count =
            static_cast<int>(m_cells.size());

        if (columns <= 0 || rows <= 0)
            return;

        const int horizontalSpacing =
            std::max(0, columns - 1) * spacing;

        const int verticalSpacing =
            std::max(0, rows - 1) * spacing;

        const int availableWidth =
            std::max(
                1,
                m_area.w - horizontalSpacing
            );

        const int availableHeight =
            std::max(
                1,
                m_area.h - verticalSpacing
            );

        const int slotWidth =
            std::max(
                1,
                availableWidth / columns
            );

        const int slotHeight =
            std::max(
                1,
                availableHeight / rows
            );

        const int gridWidth =
            columns * slotWidth +
            horizontalSpacing;

        const int gridHeight =
            rows * slotHeight +
            verticalSpacing;

        // Center the entire grid in the area.
        const int offsetX =
            m_area.x +
            (m_area.w - gridWidth) / 2;

        const int offsetY =
            m_area.y +
            (m_area.h - gridHeight) / 2;

        for (int index = 0;
             index < count;
             ++index) {

            ExpoCell* cell =
                m_cells[index];

            const int row =
                index / columns;

            const int column =
                index % columns;

            Rect slot {
                offsetX +
                    column * (slotWidth + spacing),

                offsetY +
                    row * (slotHeight + spacing),

                slotWidth,
                slotHeight
            };

            Rect result =
                centered(cell, slot);

            cell->setRect(result);
        }
    }


    // ============================================================
    // Aspect-ratio preserving centering.
    // ============================================================

    static Rect centered(
        const ExpoCell* cell,
        const Rect& bounds)
    {
        const int naturalWidth =
            cell->naturalWidth();

        const int naturalHeight =
            cell->naturalHeight();

        if (naturalWidth <= 0 ||
            naturalHeight <= 0) {

            return bounds;
        }

        const double scale =
            std::min(
                double(bounds.w) /
                    double(naturalWidth),

                double(bounds.h) /
                    double(naturalHeight)
            );

        int width =
            int(std::round(
                naturalWidth * scale
            ));

        int height =
            int(std::round(
                naturalHeight * scale
            ));

        width =
            std::min(width, bounds.w);

        height =
            std::min(height, bounds.h);

        Point center =
            bounds.center();

        return {
            center.x - width / 2,
            center.y - height / 2,
            width,
            height
        };
    }
};
   
#endif // layout_thumbnails_h_INCLUDED
