#pragma once

#include <QFontMetrics>
#include <QPainter>
#include <QTextLayout>

namespace qdsssp { namespace detail {

// Reuse one shaped line for both the width check and drawing. The usual
// elidedText()+drawText() path shapes the same fitting text twice. Preserve
// the original elision font and alignment for narrower/custom-font layouts.
inline void drawFooterText(QPainter &painter, QRectF rect, const QString &text,
                           const QFont &measurementFont) {
    auto original=[&] {
        painter.drawText(rect,Qt::AlignVCenter,
            QFontMetrics(measurementFont).elidedText(text,Qt::ElideRight,int(rect.width())));
    };
    // A cheap estimate selects the original path for likely-elided text;
    // fitting text is still checked using its exact shaped width below.
    if (measurementFont!=painter.font() || painter.layoutDirection()==Qt::RightToLeft ||
        rect.width()<text.size()*QFontMetrics(measurementFont).averageCharWidth()) {
        original(); return;
    }
    QTextLayout layout(text,painter.font(),painter.device());
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTextDirection(painter.layoutDirection());
    option.setAlignment(Qt::AlignLeft);
    layout.setTextOption(option);
    layout.setCacheEnabled(true);
    layout.beginLayout();
    auto line=layout.createLine();
    if (line.isValid()) line.setLineWidth(0x01000000);
    layout.endLayout();
    if (line.isValid() &&
        line.naturalTextWidth()<=rect.width() && line.height()<=rect.height()) {
        line.draw(&painter,QPointF(rect.left(),rect.top()+(rect.height()-line.height())/2));
    } else {
        original();
    }
}

}} // namespace qdsssp::detail
