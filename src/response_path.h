#pragma once

#include <QPainterPath>
#include <algorithm>
#include <limits>
#include <vector>

namespace qdsssp {
namespace detail {

// Independent streaming simplifier for the uniformly spaced response grid.
// Every accepted endpoint must fit ALL intervening sample constraints, not
// just its neighbours. Vertical deviation from the dense polyline is bounded
// by tolerance (logical pixels), including between samples. O(N), no scratch
// allocation. Frequency response evaluation itself remains unchanged.
inline QPainterPath responsePath(const std::vector<double> &db, const QRectF &rect,
                                 double tolerance) {
    QPainterPath path;
    if (db.empty()) return path;
    const double dx = db.size() > 1 ? rect.width() / (db.size() - 1) : 0;
    const double scale = rect.height() / 48;
    auto yAt = [&](int i) { return rect.center().y() - std::max(-72.0, std::min(72.0, db[i])) * scale; };
    auto point = [&](int i, double y) { return QPointF(rect.left() + i * dx, y); };
    int anchor = 0;
    double anchorY = yAt(0), previousY = anchorY;
    double lower = -std::numeric_limits<double>::infinity(), upper = -lower;
    path.moveTo(point(0, anchorY));
    tolerance = std::max(0.0, tolerance);
    for (int i = 1; i < int(db.size()); ++i) {
        const double y = yAt(i);
        double slope = (y - anchorY) / (i - anchor);
        if (slope < lower || slope > upper) {
            path.lineTo(point(i - 1, previousY));
            anchor = i - 1; anchorY = previousY;
            lower = -std::numeric_limits<double>::infinity(); upper = -lower;
            slope = y - anchorY;
        }
        const double margin = tolerance / (i - anchor);
        lower = std::max(lower, slope - margin);
        upper = std::min(upper, slope + margin);
        previousY = y;
    }
    if (db.size() > 1) path.lineTo(point(int(db.size()) - 1, previousY));
    return path;
}

} // namespace detail
} // namespace qdsssp
