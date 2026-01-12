#ifndef layout_thumbnails_h_INCLUDED
#define layout_thumbnails_h_INCLUDED

#include "container.h"
#include <vector>

struct Item {
    float aspectRatio;   // width / height
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
LayoutResult layoutOverview(const LayoutParams& params, const std::vector<Item>& items);
 
#endif // layout_thumbnails_h_INCLUDED
