#pragma once

#include <QImage>
#include <QPointF>
#include <algorithm>

namespace qdsssp { namespace detail {
struct MarkerSprite { QImage image; QPointF offset; };

// Cached markers have a large transparent margin. Trim it once, retaining
// the original physical-pixel alignment and centre-relative placement.
inline MarkerSprite markerSprite(const QImage &image) {
    // Scaling can change image-edge sampling after cropping, especially with
    // a nonrectangular clip. Keep the original sprite on those devices.
    if (image.devicePixelRatioF()!=1)
        return {image,QPointF(-18,-18)};
    int left=image.width(),top=image.height(),right=-1,bottom=-1;
    for (int y=0;y<image.height();++y) {
        const auto *row=reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x=0;x<image.width();++x) if (qAlpha(row[x])) {
            left=std::min(left,x); right=std::max(right,x);
            top=std::min(top,y); bottom=y;
        }
    }
    if (right<0) return {image,QPointF(-18,-18)};
    const QRect bounds(QPoint(left,top),QPoint(right,bottom));
    auto cropped=image.copy(bounds); cropped.setDevicePixelRatio(image.devicePixelRatioF());
    return {cropped,QPointF(-18,-18)+QPointF(left,top)/image.devicePixelRatioF()};
}
}} // namespace qdsssp::detail
