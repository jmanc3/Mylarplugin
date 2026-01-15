
#include "layout_thumbnails.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <climits>
#include <algorithm>

static int chooseThumbnailHeight(
    int itemCount,
    const std::vector<DensityPreset>& presets
) {
    for (const auto& p : presets) {
        if (itemCount <= p.maxItemCount)
            return p.targetHeight;
    }
    return presets.back().targetHeight;
}

LayoutResult layoutAltTabThumbnails(
    const LayoutParams& params,
    const std::vector<Item>& items
) {
    LayoutResult out;
    out.items.resize(items.size());

    if (items.empty()) {
        out.bounds = { 0, 0, 0, 0 };
        return out;
    }

    const int usableWidth =
        params.availableWidth - params.margin * 2;

    const int thumbHeight_ =
        chooseThumbnailHeight((int)items.size(), params.densityPresets);

    struct RowItem {
        int index;
        int width;
        int height;
    };

    std::vector<std::vector<RowItem>> rows;
    std::vector<RowItem> currentRow;
    int rowWidth = 0;

    for (int i = 0; i < (int)items.size(); ++i) {
        int w = (int)std::round(items[i].aspectRatio * thumbHeight_);
        int h = thumbHeight_;
        if (w > params.maxThumbWidth) {
            w = params.maxThumbWidth;
            h = w / items[i].aspectRatio;
        }
        int extra = currentRow.empty() ? 0 : params.horizontalSpacing;

        if (!currentRow.empty() &&
            rowWidth + extra + w > usableWidth)
        {
            rows.push_back(currentRow);
            currentRow.clear();
            rowWidth = 0;
            extra = 0;
        }

        currentRow.push_back({ i, w, h });
        rowWidth += extra + w;
    }

    if (!currentRow.empty())
        rows.push_back(currentRow);

    // Compute total content height
    int totalHeight = thumbHeight_ * (int)rows.size()
        + params.verticalSpacing * ((int)rows.size() - 1);

    // Vertical placement: center, but never above top edge
    int y = (params.availableHeight - totalHeight) / 2;
    if (y < params.margin)
        y = params.margin;

    // Track bounds
    int minX = INT_MAX, minY = INT_MAX;
    int maxX = INT_MIN, maxY = INT_MIN;

    // Emit rectangles
    for (const auto& row : rows) {
        int rowPixelWidth = 0;
        for (const auto& r : row)
            rowPixelWidth += r.width;
        rowPixelWidth += params.horizontalSpacing * ((int)row.size() - 1);

        int x = (params.availableWidth - rowPixelWidth) / 2;

        for (const auto& r : row) {
            Bounds rect(
                x,
                y,
                r.width,
                r.height
            );

            out.items[r.index] = rect;

            minX = std::min(minX, (int) rect.x);
            minY = std::min(minY, (int) rect.y);
            maxX = std::max(maxX, (int) (rect.x + rect.w));
            maxY = std::max(maxY, (int) (rect.y + rect.h));

            x += r.width + params.horizontalSpacing;
        }

        y += thumbHeight_ + params.verticalSpacing;
    }

    out.bounds = {
        (double) minX,
        (double) minY,
        (double) (maxX - minX),
        (double) (maxY - minY)
    };

    return out;
}


static inline double clamp(double v, double lo, double hi) {
    return std::max(lo, std::min(v, hi));
}

LayoutResult layoutOverview(
    const LayoutParams& params,
    const std::vector<Item>& items
) {
    LayoutResult out;
    out.items.resize(items.size());

    if (items.empty()) {
        out.bounds = {0, 0, 0, 0};
        return out;
    }

    const double edgeMargin = params.margin;
    const double minSpacing = std::max(
        params.horizontalSpacing,
        params.verticalSpacing
    );

    const double usableW =
        params.availableWidth  - 2.0 * edgeMargin;
    const double usableH =
        params.availableHeight - 2.0 * edgeMargin;

    const double cx = params.availableWidth  * 0.5;
    const double cy = params.availableHeight * 0.5;

    // ---------------------------------------------------------------------
    // 1. Compute uniform global scale
    // ---------------------------------------------------------------------

    double scale = std::numeric_limits<double>::infinity();

    for (const auto& it : items) {
        if (it.width <= 0.0 || it.height <= 0.0)
            continue;

        scale = std::min(scale, usableW / it.width);
        scale = std::min(scale, usableH / it.height);
    }

    if (!std::isfinite(scale))
        scale = 1.0;

    // Leave headroom for spacing and relaxation
    scale *= 0.9;

    struct Tmp {
        double tx, ty, tw, th;
    };

    std::vector<Tmp> tmp(items.size());

    // ---------------------------------------------------------------------
    // 2. Initial radial placement (macOS-style)
    // ---------------------------------------------------------------------

    const double radius =
        std::min(usableW, usableH) * 0.25;

    for (size_t i = 0; i < items.size(); ++i) {
        const double w = items[i].width  * scale;
        const double h = items[i].height * scale;

        const double angle =
            (double)i / (double)items.size() * 2.0 * M_PI;

        const double ox = std::cos(angle) * radius;
        const double oy = std::sin(angle) * radius;

        tmp[i].tw = w;
        tmp[i].th = h;
        tmp[i].tx = cx + ox - w * 0.5;
        tmp[i].ty = cy + oy - h * 0.5;
    }

    // ---------------------------------------------------------------------
    // 3. Bounded overlap relaxation (deterministic)
    // ---------------------------------------------------------------------

    constexpr int RELAX_PASSES = 30;

    for (int pass = 0; pass < RELAX_PASSES; ++pass) {
        for (size_t i = 0; i < tmp.size(); ++i) {
            for (size_t j = i + 1; j < tmp.size(); ++j) {
                auto& a = tmp[i];
                auto& b = tmp[j];

                const double ax = a.tx + a.tw * 0.5;
                const double ay = a.ty + a.th * 0.5;
                const double bx = b.tx + b.tw * 0.5;
                const double by = b.ty + b.th * 0.5;

                const double dx = ax - bx;
                const double dy = ay - by;

                const double minX =
                    (a.tw + b.tw) * 0.5 + minSpacing;
                const double minY =
                    (a.th + b.th) * 0.5 + minSpacing;

                const double ox = minX - std::abs(dx);
                const double oy = minY - std::abs(dy);

                if (ox > 0.0 && oy > 0.0) {
                    if (ox < oy) {
                        const double push = ox * 0.5;
                        const double dir = (dx >= 0.0) ? 1.0 : -1.0;
                        a.tx += push * dir;
                        b.tx -= push * dir;
                    } else {
                        const double push = oy * 0.5;
                        const double dir = (dy >= 0.0) ? 1.0 : -1.0;
                        a.ty += push * dir;
                        b.ty -= push * dir;
                    }
                }
            }
        }

        // Clamp every pass (critical for stability)
        for (auto& w : tmp) {
            w.tx = clamp(
                w.tx,
                edgeMargin,
                params.availableWidth - edgeMargin - w.tw
            );
            w.ty = clamp(
                w.ty,
                edgeMargin,
                params.availableHeight - edgeMargin - w.th
            );
        }
    }

    // ---------------------------------------------------------------------
    // 4. Emit results + bounds
    // ---------------------------------------------------------------------

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    for (size_t i = 0; i < tmp.size(); ++i) {
        out.items[i] = {
            tmp[i].tx,
            tmp[i].ty,
            tmp[i].tw,
            tmp[i].th
        };

        minX = std::min(minX, tmp[i].tx);
        minY = std::min(minY, tmp[i].ty);
        maxX = std::max(maxX, tmp[i].tx + tmp[i].tw);
        maxY = std::max(maxY, tmp[i].ty + tmp[i].th);
    }

    out.bounds = {
        minX,
        minY,
        maxX - minX,
        maxY - minY
    };

    return out;
}
