#pragma once

#include <QFontMetrics>
#include <QGlyphRun>
#include <QPainter>
#include <QRawFont>
#include <QTextLayout>
#include <array>
#include <vector>

namespace qdsssp { namespace detail {

// Keep the exact positions produced by QTextLayout when only tabular digits
// change. Template count is bounded; unusual fonts/shaping use the normal path.
class FooterTextCache {
public:
    bool draw(QPainter &painter, const QRectF &rect, const QString &text) {
        const QFont font=painter.font();
        auto *device=painter.device();
        const bool spaced=font.letterSpacingType()==QFont::PercentageSpacing?
            (font.letterSpacing()!=0 && font.letterSpacing()!=100):font.letterSpacing()!=0;
        if (font.underline() || font.overline() || font.strikeOut() || spaced || font.wordSpacing()!=0 ||
            (font.stretch()!=0 && font.stretch()!=QFont::Unstretched) || font.capitalization()!=QFont::MixedCase ||
            painter.layoutDirection()==Qt::RightToLeft) return false;
        if (font!=font_ || dpiX_!=device->logicalDpiX() || dpiY_!=device->logicalDpiY() ||
            dpr_!=device->devicePixelRatioF() || direction_!=painter.layoutDirection()) {
            entries_.clear(); font_=font; dpiX_=device->logicalDpiX(); dpiY_=device->logicalDpiY();
            dpr_=device->devicePixelRatioF(); direction_=painter.layoutDirection();
            digitsChecked_=false; digitsValid_=false;
        }
        if (digitsChecked_ && !digitsValid_) return false;
        QString key=text;
        for (int i=0;i<key.size();++i) {
            const auto ch=key.at(i).unicode();
            if (ch<32 || ch>126) return false;
            if (ch>='0' && ch<='9') {
                auto numericContext=[](QChar c) {
                    return (c>=QLatin1Char('0') && c<=QLatin1Char('9')) ||
                        c==QLatin1Char(' ') || c==QLatin1Char('.') || c==QLatin1Char('-') || c==QLatin1Char('+');
                };
                if ((i>0 && !numericContext(text.at(i-1))) ||
                    (i+1<text.size() && !numericContext(text.at(i+1)))) return false;
                key[i]=QLatin1Char('0');
            }
        }
        Entry *entry=nullptr;
        for (auto &item:entries_) if (item.key==key) { entry=&item; break; }
        if (!entry) {
            QTextLayout layout(text,font,device);
            QTextOption option; option.setWrapMode(QTextOption::NoWrap);
            option.setTextDirection(direction_); option.setAlignment(Qt::AlignLeft);
            layout.setTextOption(option); layout.beginLayout();
            auto line=layout.createLine();
            if (line.isValid()) line.setLineWidth(0x01000000);
            layout.endLayout();
            if (!line.isValid() || line.naturalTextWidth()+8>rect.width() || line.height()>rect.height()) return false;
            const auto runs=line.glyphRuns();
            if (runs.size()!=1 || runs[0].glyphIndexes().size()!=text.size() ||
                runs[0].rawFont().glyphIndexesForString(text)!=runs[0].glyphIndexes()) return false;
            if (!digitsChecked_) checkDigits(runs[0].rawFont(),font.kerning());
            if (!digitsValid_) return false;
            if (entries_.size()==32) entries_.erase(entries_.begin());
            entries_.push_back({key,runs[0],runs[0].glyphIndexes(),line.naturalTextWidth(),line.height()});
            entry=&entries_.back();
        }
        if (entry->width+8>rect.width() || entry->height>rect.height()) return false;
        for (int i=0;i<text.size();++i) {
            const auto ch=text.at(i).unicode();
            if (ch>='0' && ch<='9') entry->glyphs[i]=digitGlyphs_[ch-'0'];
        }
        auto run=entry->run; run.setGlyphIndexes(entry->glyphs);
        painter.drawGlyphRun(QPointF(rect.left(),rect.top()+(rect.height()-entry->height)/2),run);
        return true;
    }
private:
    void checkDigits(const QRawFont &raw, bool kerning) {
        digitsChecked_=true;
        const auto ids=raw.glyphIndexesForString(QStringLiteral("0123456789"));
        if (ids.size()!=10) return;
        const auto advances=raw.advancesForGlyphIndexes(ids);
        for (int i=0;i<10;++i) {
            if (ids[i]==0 || advances[i]!=advances[0]) return;
            digitGlyphs_[i]=ids[i];
        }
        if (kerning) {
            const auto neighbours=raw.glyphIndexesForString(QStringLiteral("0123456789 .-+"));
            for (const auto first:neighbours) for (const auto second:neighbours) {
                const QVector<quint32> pair{first,second};
                if (raw.advancesForGlyphIndexes(pair,QRawFont::KernedAdvances)!=
                    raw.advancesForGlyphIndexes(pair,QRawFont::SeparateAdvances)) return;
            }
        }
        digitsValid_=true;
    }
    struct Entry { QString key; QGlyphRun run; QVector<quint32> glyphs; qreal width,height; };
    std::vector<Entry> entries_;
    std::array<quint32,10> digitGlyphs_{};
    QFont font_;
    int dpiX_=0,dpiY_=0;
    qreal dpr_=0;
    Qt::LayoutDirection direction_=Qt::LayoutDirectionAuto;
    bool digitsChecked_=false,digitsValid_=false;
};

// Reuse one shaped line for both the width check and drawing. The usual
// elidedText()+drawText() path shapes the same fitting text twice. Preserve
// the original elision font and alignment for narrower/custom-font layouts.
inline void drawFooterText(QPainter &painter, QRectF rect, const QString &text,
                           const QFont &measurementFont, FooterTextCache *cache=nullptr) {
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
    if (cache && cache->draw(painter,rect,text)) return;
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
