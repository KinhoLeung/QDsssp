#pragma once
#include "eq_response.h"
#include <QColor>
#include <QIcon>
#include <QRectF>
class QPainter;
namespace qdsssp {
// Uses the original DSSSP font outlines, cached after their first use.
void paintFilterIcon(QPainter &painter, QRectF rect, FilterType type, double gainDb, QColor color);
QIcon filterIcon(FilterType type, double gainDb = 0, QColor color = QColor(226,236,242));
}
