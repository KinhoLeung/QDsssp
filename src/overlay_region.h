#pragma once

#include <QPainterPath>

namespace qdsssp { namespace detail {

// One conservative rectangle is cheaper to compose than a fragmented clip
// region on the software paint engine. Skip transparent margins without
// changing curve sampling or antialiasing.
class OverlayCoverage {
public:
    explicit OverlayCoverage(QRect bounds) : bounds_(bounds) {}
    void add(QRectF rect) {
        if (rect.isEmpty()) return;
        // Image and clip transforms can round opposite ways at fractional
        // DPR. Keep an extra logical pixel beyond the conservative bounds.
        const QRect r=rect.toAlignedRect().adjusted(-1,-1,1,1).intersected(bounds_);
        if (r.isEmpty()) return;
        ink_=ink_.united(r);
    }
    void addPath(const QPainterPath &path,QRectF clip,double margin) {
        if (!path.isEmpty())
            add(path.controlPointRect().adjusted(-margin,-margin,margin,margin).intersected(clip));
    }
    QRect rect() const { return ink_; }
private:
    QRect bounds_,ink_;
};

}} // namespace qdsssp::detail
