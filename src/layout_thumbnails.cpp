
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


