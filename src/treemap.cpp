#include "treemap.h"

#include <algorithm>

namespace {

double WorstRatio(const std::vector<double>& row, double side) {
    double sum = 0.0;
    double maxVal = 0.0;
    double minVal = 1e300;
    for (double v : row) {
        sum += v;
        if (v > maxVal) maxVal = v;
        if (v < minVal) minVal = v;
    }
    if (sum <= 0.0) {
        return 1e300;
    }
    const double sum2 = sum * sum;
    const double side2 = side * side;
    const double a = side2 * maxVal / sum2;
    const double b = sum2 / (side2 * minVal);
    return a > b ? a : b;
}

// Places the accumulated row into the free rectangle and shrinks it.
void EmitRow(const std::vector<double>& row,
             double& x, double& y, double& w, double& h,
             const std::vector<Node*>& nodes, size_t start,
             std::vector<TmRect>& out) {
    double sum = 0.0;
    for (double v : row) sum += v;
    if (sum <= 0.0) return;

    if (w >= h) {
        // Stack vertically along the left edge.
        const double stripW = sum / h;
        double cy = y;
        for (size_t k = 0; k < row.size(); ++k) {
            const double itemH = row[k] / stripW;
            out.push_back({nodes[start + k],
                           static_cast<float>(x), static_cast<float>(cy),
                           static_cast<float>(stripW), static_cast<float>(itemH)});
            cy += itemH;
        }
        x += stripW;
        w -= stripW;
    } else {
        // Stack horizontally along the top edge.
        const double stripH = sum / w;
        double cx = x;
        for (size_t k = 0; k < row.size(); ++k) {
            const double itemW = row[k] / stripH;
            out.push_back({nodes[start + k],
                           static_cast<float>(cx), static_cast<float>(y),
                           static_cast<float>(itemW), static_cast<float>(stripH)});
            cx += itemW;
        }
        y += stripH;
        h -= stripH;
    }
}

}  // namespace

void TreemapLayout(const std::vector<Node*>& items,
                   float x, float y, float w, float h,
                   std::vector<TmRect>& out) {
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }

    std::vector<Node*> sorted;
    sorted.reserve(items.size());
    double total = 0.0;
    for (Node* n : items) {
        if (n->size > 0) {
            sorted.push_back(n);
            total += static_cast<double>(n->size);
        }
    }
    if (total <= 0.0) {
        return;
    }

    std::sort(sorted.begin(), sorted.end(), [](const Node* a, const Node* b) {
        return a->size > b->size;
    });

    const double scale = (static_cast<double>(w) * static_cast<double>(h)) / total;
    std::vector<double> areas(sorted.size());
    for (size_t i = 0; i < sorted.size(); ++i) {
        areas[i] = static_cast<double>(sorted[i]->size) * scale;
    }

    double rx = x;
    double ry = y;
    double rw = w;
    double rh = h;

    std::vector<double> row;
    size_t rowStart = 0;
    size_t i = 0;
    while (i < sorted.size()) {
        const double side = (rw >= rh) ? rh : rw;
        if (row.empty()) {
            row.push_back(areas[i]);
            ++i;
            continue;
        }
        std::vector<double> candidate = row;
        candidate.push_back(areas[i]);
        if (WorstRatio(candidate, side) <= WorstRatio(row, side)) {
            row = std::move(candidate);
            ++i;
        } else {
            EmitRow(row, rx, ry, rw, rh, sorted, rowStart, out);
            rowStart = i;
            row.clear();
        }
    }
    if (!row.empty()) {
        EmitRow(row, rx, ry, rw, rh, sorted, rowStart, out);
    }
}
