#include "qdsssp/filter_icon.h"
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QResource>
#include <array>

static void initDssspResources() { Q_INIT_RESOURCE(dsssp); }
namespace qdsssp {
void paintFilterIcon(QPainter &p, QRectF rect, FilterType type, double gain, QColor color) {
    static const auto paths=[] {
        initDssspResources();
        QRawFont font(QStringLiteral(":/qdsssp/dsssp.ttf"),24);
        const ushort codes[]={0xe901,0xe903,0xe903,0xe905,0xe905,0xe907,0xe908,
                              0xe906,0xe906,0xe904,0xe904,0xe900,0xe902};
        std::array<QPainterPath,13> result;
        for (int i=0;i<13;++i) {
            const auto glyphs=font.glyphIndexesForString(QString(QChar(codes[i])));
            if (!glyphs.isEmpty()) result[i]=font.pathForGlyph(glyphs.first());
        }
        return result;
    }();
    const int index=int(type);
    if (index<0 || index>12) return;
    const auto &path=paths[index]; const auto bounds=path.boundingRect();
    if (bounds.isEmpty()) return;
    const bool shelf=type==FilterType::LowShelf1 || type==FilterType::LowShelf2 ||
                     type==FilterType::HighShelf1 || type==FilterType::HighShelf2;
    const bool flip=(shelf && gain>0) || ((type==FilterType::Peak || type==FilterType::Gain) && gain<0);
    const auto scale=std::min(rect.width()/bounds.width(),rect.height()/bounds.height());
    p.save(); p.translate(rect.center()); p.scale(scale,flip?-scale:scale); p.translate(-bounds.center());
    p.setRenderHint(QPainter::Antialiasing); p.fillPath(path,color); p.restore();
}
QIcon filterIcon(FilterType type, double gain, QColor color) {
    QPixmap pixmap(28,28); pixmap.fill(Qt::transparent); QPainter p(&pixmap);
    paintFilterIcon(p,QRectF(3,3,22,22),type,gain,color); p.end(); return QIcon(pixmap);
}
}
