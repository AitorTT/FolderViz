#pragma once

#include <vector>

#include "fsnode.h"

struct TmRect {
    Node* node = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Squarified treemap layout. `items` is consumed but not modified; only entries
// with a positive size produce rectangles. Rectangles are appended to `out`.
void TreemapLayout(const std::vector<Node*>& items,
                   float x,
                   float y,
                   float w,
                   float h,
                   std::vector<TmRect>& out);
