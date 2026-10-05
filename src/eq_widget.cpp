#include "qdsssp/eq_widget.h"
#include "qdsssp/filter_icon.h"
#include "response_path.h"
#include "overlay_region.h"
#include "footer_text.h"
#include "marker_sprite.h"
#include <QHideEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QTouchEvent>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace qdsssp {
namespace {
const QColor background(17, 24, 32), grid(38, 49, 60), muted(133, 151, 166);
const QColor ink(226, 236, 242), accent(243, 202, 105), spectrumColor(58, 149, 149);
QColor bandColor(int i) {
    static const QColor colors[] = {QColor(96,197,178), QColor(116,172,242), QColor(195,145,231),
        QColor(238,161,118), QColor(228,126,160), QColor(177,200,118)};
    return colors[std::max(0, i)%6];
}
double clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
QString frequencyLabel(double hz) {
    return hz >= 1000 ? QString::number(hz/1000, 'f', 2)+" kHz" : QString::number(hz, 'f', 0)+" Hz";
}
}

EqWidget::EqWidget(QWidget *parent) : QWidget(parent), mailbox_(std::make_shared<SpectrumMailbox>()) {
    footerTextCache_.reset(new detail::FooterTextCache);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_AcceptTouchEvents);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(minimumSizeHint());
    qRegisterMetaType<qdsssp::Band>();
    frameTimer_.setSingleShot(true);
    frameTimer_.setTimerType(Qt::PreciseTimer);
    connect(&frameTimer_, &QTimer::timeout, this, &EqWidget::frameTick);
    clock_.start();
    mailbox_->setAnalysisEnabled(false); // no visible consumer until showEvent
    spectrumGeneration_=mailbox_->analysisState().generation;
    setAccessibleName(tr("Equalizer frequency response and spectrum"));
}

EqWidget::~EqWidget() {
    frameTimer_.stop();
    mailbox_->setAnalysisEnabled(false);
}

QRectF EqWidget::plotRect() const { return QRectF(48, 26, std::max(1, width()-104), std::max(1, height()-134)); }
QRectF EqWidget::qControlRect() const { return QRectF(48, height()-52, std::max(1, width()-104), 48); }
qreal EqWidget::frequencyX(double hz) const {
    const auto r = plotRect();
    return r.left()+std::log(clamp(hz,minHz_,maxHz_)/minHz_)/std::log(maxHz_/minHz_)*r.width();
}
qreal EqWidget::gainY(double db) const { const auto r=plotRect(); return r.center().y()-clamp(db,-24,24)*r.height()/48; }
double EqWidget::frequencyAt(qreal x) const {
    const auto r=plotRect();
    return minHz_*std::exp(clamp((x-r.left())/r.width(),0,1)*std::log(maxHz_/minHz_));
}
double EqWidget::gainAt(qreal y) const { const auto r=plotRect(); return clamp((r.center().y()-y)*48/r.height(),-24,24); }
QPointF EqWidget::bandPosition(int i) const {
    if (i<0 || i>=int(bands().size())) return {};
    const auto &b=bands()[i];
    return {frequencyX(b.frequency), gainY(hasGain(b.type)?b.gainDb:0)};
}

void EqWidget::setBands(const std::vector<Band> &b) {
    // An explicit full snapshot still ends an active edit, even if all
    // parameter values match.
    finishDrag(true);
    const int count=int(std::min(b.size(),std::size_t(ResponseCache::MaxBands)));
    const bool structureChanged=count!=int(bands().size());
    bool changed=structureChanged, selectedChanged=structureChanged;
    for (int i=0;i<count && !structureChanged;++i) {
        const auto value=sanitized(b[i],sampleRate_);
        if (!(value==bands()[i])) {
            changed=true;
            if (i==selected_) selectedChanged=true;
            invalidateBand(i,bands()[i],value);
        }
    }
    // Preserve an explicit no-selection state across data snapshots.
    // Clamp only an existing selection when the band list shrinks.
    const int selection=count==0 || selected_<0 ? -1 : std::min(selected_,count-1);
    const bool selectionChanged=selection!=selected_;
    if (!changed && !selectionChanged) return;
    if (structureChanged) {
        responseDirty_=true; sumPathDirty_=true; bandPathDirty_=true;
        markerDirty_.fill(true); overlayDirty_=true;
    }
    if (changed) response_.setBands(b);
    if (selectionChanged) invalidateSelection(selected_,selection);
    selected_=selection;
    footerDirty_=footerDirty_ || selectedChanged || selectionChanged;
    requestFrame();
    if (selectionChanged) emit selectedBandChanged(selected_);
}
void EqWidget::setBand(int i, const Band &b) {
    if (i<0 || i>=int(bands().size())) return;
    const auto value=sanitized(b,sampleRate_);
    if (value==bands()[i]) return;
    invalidateBand(i,bands()[i],value);
    response_.setBand(i,value);
    requestFrame();
}
void EqWidget::setSelectedBand(int i) {
    if (i < -1 || i>=int(bands().size()) || i==selected_) return;
    finishDrag(true); invalidateSelection(selected_,i); selected_=i;
    emit selectedBandChanged(i); requestFrame();
}
void EqWidget::setSelectedBandFillEnabled(bool enabled) {
    if (fillSelectedBand_==enabled) return;
    fillSelectedBand_=enabled; bandFillDirty_=true; overlayDirty_=true; requestFrame();
}
void EqWidget::setSelectedBandFillOpacity(double opacity) {
    if (!std::isfinite(opacity)) return;
    opacity=clamp(opacity,0,1);
    if (selectedBandFillOpacity_==opacity) return;
    selectedBandFillOpacity_=opacity; overlayDirty_=true; requestFrame();
}
void EqWidget::setSampleRate(double fs) {
    if (!std::isfinite(fs) || fs<100 || fs>768000 || fs==sampleRate_) return;
    finishDrag(true); sampleRate_=fs; maxHz_=std::min(20000.0, fs*0.499);
    invalidateGeometry();
}
void EqWidget::setFrameRate(int fps) {
    fps_=std::max(10,std::min(60,fps)); frameTimer_.stop();
    nextFrameNs_=clock_.nsecsElapsed(); requestFrame();
}
void EqWidget::setSpectrumEnabled(bool enabled) {
    if (spectrumEnabled_==enabled) return;
    spectrumEnabled_=enabled;
    updateSpectrumDemand();
    requestFrame();
}
void EqWidget::resetSpectrum() {
    // Clear both pixels and smoothing targets. The first resumed FFT frame
    // must not be blended with peaks from before pause or a hidden page.
    spectrumPath_=QPainterPath();
    spectrumTarget_.clear(); spectrumDisplay_.clear();
    spectrumFrame_.count=0;
    spectrumGeometryDirty_=true;
    lastSpectrumMs_=-1000;
    lastTickMs_=clock_.elapsed();
    pendingUpdate_+=plotRect().adjusted(-2,-2,2,2).toAlignedRect();
}
void EqWidget::updateSpectrumDemand() {
    const bool active=spectrumEnabled_ && isVisible();
    mailbox_->setAnalysisEnabled(active);
    spectrumGeneration_=mailbox_->analysisState().generation;
    resetSpectrum();
    if (active) {
        // Schedule the first poll immediately instead of retaining a hidden
        // page's deadline. Subsequent polls still honor the configured FPS.
        nextFrameNs_=clock_.nsecsElapsed();
        frameTimer_.stop();
    }
}
void EqWidget::setSpectrumFillEnabled(bool enabled) {
    if (fillSpectrum_==enabled) return;
    fillSpectrum_=enabled;
    pendingUpdate_+=plotRect().adjusted(-2,-2,2,2).toAlignedRect(); requestFrame();
}
void EqWidget::setCurveAntialiasing(bool enabled) {
    if (antialiasing_==enabled) return;
    antialiasing_=enabled; overlayDirty_=true; requestFrame();
}
void EqWidget::setTouchTargetSize(int pixels) { touchTarget_=std::max(32,std::min(100,pixels)); }

void EqWidget::requestFrame() {
    // Scheduling alone doesn't dirty pixels. Preparation accumulates the
    // affected regions, allowing no-op requests to finish without repainting.
    if (!isVisible() || frameTimer_.isActive()) return;
    const int remaining=int((std::max<qint64>(0,nextFrameNs_-clock_.nsecsElapsed())+999999)/1000000);
    frameTimer_.start(remaining);
}
void EqWidget::invalidateBand(int index, const Band &previous, const Band &next) {
    responseDirty_=true;
    if (index==selected_) footerDirty_=true;
    if (index==selected_ && previous.useCustomCoefficients!=next.useCustomCoefficients) overlayDirty_=true;
    const double oldGain=hasGain(previous.type)?previous.gainDb:0;
    const double newGain=hasGain(next.type)?next.gainDb:0;
    if (previous.frequency!=next.frequency || oldGain!=newGain || previous.enabled!=next.enabled) {
        markerDirty_[index]=true;
        overlayDirty_=true;
    }
}
void EqWidget::invalidateSelection(int previous, int next) {
    if (previous>=0) markerDirty_[previous]=true;
    if (next>=0) markerDirty_[next]=true;
    bandPathDirty_=true; overlayDirty_=true; footerDirty_=true;
}
void EqWidget::invalidateGeometry() {
    backgroundDirty_=true; responseDirty_=true; spectrumGeometryDirty_=true;
    sumPathDirty_=true; bandPathDirty_=true; markerDirty_.fill(true);
    response_.setGrid(std::min(2048,std::max(2,int(plotRect().width()*2))), minHz_,maxHz_,sampleRate_);
    requestFrame();
}

void EqWidget::startProfiling() {
    profile_.reset(new FrameProfile);
    for (auto &phase : profile_->phases) phase.samples.reserve(4096);
    previousProfilePaintNs_ = -1;
    profiling_ = true;
}

void EqWidget::frameTick() {
    if (!isVisible()) return;
    QElapsedTimer timing; timing.start();
    const auto analysis=mailbox_->analysisState();
    if (spectrumGeneration_!=analysis.generation) {
        // Also handles explicit source.setPaused() calls while still visible.
        spectrumGeneration_=analysis.generation;
        resetSpectrum();
    }
    const qint64 nowNs=clock_.nsecsElapsed(), intervalNs=1000000000LL/fps_;
    // Advance an absolute deadline to retain 33.333 ms at 30 Hz. Restarting
    // a rounded 34 ms timer each frame accumulates drift, even with spare CPU.
    if (nextFrameNs_<nowNs-intervalNs) nextFrameNs_=nowNs;
    nextFrameNs_+=intervalNs;
    const auto now=clock_.elapsed();
    const float release=float(48.0*clamp((now-lastTickMs_)/1000.0,0,0.25));
    lastTickMs_=now;
    const bool geometryChanged=spectrumGeometryDirty_; spectrumGeometryDirty_=false;
    if (backgroundDirty_) rebuildBackground();
    if (responseDirty_ || sumPathDirty_ || bandPathDirty_) rebuildResponse();
    if (overlayDirty_) prepareOverlay();
    // Avoid rebuilding a bitmap that continuous edits would use only once.
    // A short quiet period also tolerates source/display timer phase jitter.
    if (!overlayCacheValid_ && nowNs-overlayChangedNs_>=3*intervalNs)
        rebuildOverlay();
    if (footerDirty_) rebuildFooter();
    QElapsedTimer spectrumTiming; if (profiling_) spectrumTiming.start();
    QRect spectrumDamage;
    if (spectrumEnabled_ && analysis.enabled) {
        const bool fresh=mailbox_->takeLatest(spectrumFrame_);
        if (fresh) lastSpectrumMs_=now;
        if ((fresh || geometryChanged) && spectrumFrame_.count>0) {
            projector_.configure(std::min(512,std::max(2,int(plotRect().width()))),minHz_,maxHz_,
                                 spectrumFrame_.count,spectrumFrame_.sampleRate);
            projector_.project(spectrumFrame_,spectrumTarget_);
        }
        if (spectrumDisplay_.size()!=spectrumTarget_.size()) spectrumDisplay_.assign(spectrumTarget_.size(),-120);
        bool changed=geometryChanged;
        for (int i=0;i<int(spectrumDisplay_.size());++i) {
            const float target=now-lastSpectrumMs_>250 ? -120.0f : spectrumTarget_[i];
            const float value=std::max(target,spectrumDisplay_[i]-release);
            changed=changed || value!=spectrumDisplay_[i]; spectrumDisplay_[i]=value;
        }
        if (changed) {
            // Restore old pixels as well as drawing the new trace. Filling
            // extends down to the dBFS floor, unlike the stroke alone.
            spectrumDamage=spectrumBounds(spectrumPath_);
            rebuildSpectrum();
            spectrumDamage=spectrumDamage.united(spectrumBounds(spectrumPath_));
        }
    }
    if (profiling_) profile_->phases[FrameProfile::Spectrum].add(spectrumTiming.nsecsElapsed());
    pendingUpdate_+=spectrumDamage;
    // Keep graph and footer damage separate instead of repainting the axes
    // and the space between them on every parameter edit.
    const QRegion repaintRegion=pendingUpdate_.intersected(QRegion(rect()));
    pendingUpdate_=QRegion();
    if (!repaintRegion.isEmpty()) update(repaintRegion);
    // Preparation consumes part of the frame budget; don't add its duration
    // to every timer interval during continuous editing.
    // Finish the transition to a static cache even with spectrum disabled,
    // then stop waking until another update is requested.
    if (spectrumEnabled_ || !overlayCacheValid_)
        frameTimer_.start(int((std::max<qint64>(0,nextFrameNs_-clock_.nsecsElapsed())+999999)/1000000));
    const auto ns=timing.nsecsElapsed(); totalPreparationNs_+=ns;
    maxPreparationNs_=std::max(maxPreparationNs_,ns); ++preparedFrames_;
    if (profiling_) profile_->phases[FrameProfile::Preparation].add(ns);
}

void EqWidget::rebuildBackground() {
    const qreal dpr=devicePixelRatioF();
    background_=QImage(QSize(qCeil(width()*dpr),qCeil(height()*dpr)),QImage::Format_RGB32);
    background_.setDevicePixelRatio(dpr); background_.fill(background);
    QPainter p(&background_);
    QFont f=font(); f.setPixelSize(11); p.setFont(f);
    const auto r=plotRect();
    p.setPen(muted); p.drawText(QRectF(8,3,40,18),Qt::AlignCenter,"dB");
    p.drawText(QRectF(width()-53,3,48,18),Qt::AlignCenter,"dBFS");
    for (int db=-24;db<=24;db+=6) {
        const auto y=gainY(db); p.setPen(db==0?QColor(72,86,99):grid);
        p.drawLine(QPointF(r.left(),y),QPointF(r.right(),y)); p.setPen(muted);
        p.drawText(QRectF(0,y-8,39,16),Qt::AlignRight|Qt::AlignVCenter,QString::number(db));
    }
    for (int db=0;db>=-120;db-=24) {
        const auto y=r.top()-db*r.height()/120;
        p.drawText(QRectF(r.right()+9,y-8,42,16),Qt::AlignLeft|Qt::AlignVCenter,QString::number(db));
    }
    for (int decade=10;decade<=10000;decade*=10) for (int n=1;n<10;++n) {
        const double hz=decade*n; if (hz<minHz_ || hz>maxHz_) continue;
        p.setPen(grid); const auto x=frequencyX(hz);
        p.drawLine(QPointF(x,r.top()),QPointF(x,r.bottom()));
    }
    const double labels[]={20,50,100,200,500,1000,2000,5000,10000,20000};
    qreal last=-100;
    for (double hz:labels) {
        if (hz>maxHz_) continue;
        const auto x=frequencyX(hz); if (x-last<39) continue; last=x;
        const QString text=hz>=1000?QString::number(hz/1000)+"k":QString::number(hz);
        p.setPen(muted); p.drawText(QRectF(x-20,r.bottom()+9,40,16),Qt::AlignCenter,text);
    }
    p.setPen(grid); p.drawLine(20,height()-77,width()-20,height()-77);
    backgroundDirty_=false;
    pendingUpdate_=QRegion(rect());
    markerDirty_.fill(true);
    overlayDirty_=true;
    footerValid_=false; footerDirty_=true;
}

QPainterPath EqWidget::makeResponsePath(const std::vector<double> &db) const {
    // 0.2 physical-pixel maximum vertical deviation from the dense path.
    return detail::responsePath(db, plotRect(), 0.2 / devicePixelRatioF());
}
QRect EqWidget::responseBounds(const QPainterPath &path) const {
    detail::OverlayCoverage coverage(rect());
    coverage.addPath(path,plotRect().adjusted(-1,-1,1,1),2+2/devicePixelRatioF());
    return coverage.rect();
}
QRect EqWidget::spectrumBounds(const QPainterPath &path) const {
    if (path.isEmpty()) return {};
    auto bounds=path.controlPointRect();
    if (fillSpectrum_) bounds.setBottom(std::max(bounds.bottom(),plotRect().bottom()));
    detail::OverlayCoverage coverage(rect());
    const qreal margin=2+2/devicePixelRatioF();
    coverage.add(bounds.adjusted(-margin,-margin,margin,margin)
                 .intersected(plotRect().adjusted(-1,-1,1,1)));
    return coverage.rect();
}
void EqWidget::rebuildResponse() {
    QElapsedTimer timing; if (profiling_) timing.start();
    if (responseDirty_) {
        const auto selectedRevision=selected_>=0?response_.bandRevision(selected_):0;
        sumPathDirty_=response_.update() || sumPathDirty_;
        if (selected_>=0 && selectedRevision!=response_.bandRevision(selected_)) bandPathDirty_=true;
        if (profiling_) profile_->phases[FrameProfile::Response].add(timing.nsecsElapsed());
    }
    if (profiling_) timing.restart();
    const bool pathsChanged=sumPathDirty_ || bandPathDirty_;
    // Include both snapshots so partial updates also erase the old curves.
    QRect damage;
    if (sumPathDirty_) {
        damage=damage.united(responseBounds(sumPath_));
        sumPath_=makeResponsePath(response_.sumDb());
        damage=damage.united(responseBounds(sumPath_));
    }
    if (bandPathDirty_) {
        damage=damage.united(responseBounds(bandPath_));
        bandPath_=selected_>=0?makeResponsePath(response_.bandDb(selected_)):QPainterPath();
        damage=damage.united(responseBounds(bandPath_));
        bandFillDirty_=true;
    }
    pendingUpdate_+=damage;
    if (profiling_ && pathsChanged) profile_->phases[FrameProfile::Paths].add(timing.nsecsElapsed());
    responseDirty_=false;
    sumPathDirty_=false; bandPathDirty_=false;
    overlayDirty_=overlayDirty_ || pathsChanged;
}
void EqWidget::prepareOverlay() {
    QElapsedTimer timing; if (profiling_) timing.start();
    // Freeze every overlay input alongside the prepared response paths.
    // Setters may run before an unrelated expose, so painting must not read
    // live band positions, selection or marker styles from the model.
    overlayRect_=plotRect().adjusted(-18,-18,18,18).toAlignedRect().intersected(rect());
    overlayClip_=plotRect().adjusted(-1,-1,1,1);
    overlayFont_=font();
    overlaySelection_=selected_;
    if (bandFillDirty_) {
        // Close against the EQ 0 dB line, including for shelves and pass
        // filters. Reuse the simplified stroke instead of sampling again.
        // Both filled areas must be erased, not just their curve outlines.
        pendingUpdate_+=responseBounds(bandFillPath_);
        bandFillPath_=QPainterPath();
        if (fillSelectedBand_ && !bandPath_.isEmpty()) {
            bandFillPath_=bandPath_;
            const auto first=bandPath_.elementAt(0);
            const qreal baseline=plotRect().center().y();
            bandFillPath_.lineTo(bandPath_.currentPosition().x(),baseline);
            bandFillPath_.lineTo(first.x,baseline);
            bandFillPath_.closeSubpath();
        }
        pendingUpdate_+=responseBounds(bandFillPath_);
        bandFillDirty_=false;
    }
    auto fillColor=bandColor(selected_);
    fillColor.setAlphaF(selectedBandFillOpacity_);
    if (overlayBandFillColor_!=fillColor)
        pendingUpdate_+=responseBounds(bandFillPath_);
    overlayBandFillColor_=fillColor;
    if (overlayAntialiasing_!=antialiasing_)
        pendingUpdate_+=responseBounds(sumPath_).united(responseBounds(bandPath_))
            .united(responseBounds(bandFillPath_));
    overlayAntialiasing_=antialiasing_;
    const bool external=selected_>=0 && bands()[selected_].useCustomCoefficients;
    if (overlayExternalCoefficients_!=external)
        pendingUpdate_+=plotRect().toAlignedRect();
    overlayExternalCoefficients_=external;
    const int count=int(bands().size());
    auto markerBounds=[](QPointF position) {
        return QRectF(position-QPointF(18,18),QSizeF(36,36)).toAlignedRect().adjusted(-1,-1,1,1);
    };
    QRect markerDamage;
    for (int i=0;i<count;++i) {
        if (!markerDirty_[i] && i<overlayMarkerCount_) continue;
        if (i<overlayMarkerCount_) markerDamage=markerDamage.united(markerBounds(overlayMarkers_[i].position));
        overlayMarkers_[i].position=bandPosition(i);
        overlayMarkers_[i].image=markerImage(i);
        overlayMarkers_[i].offset=markerOffsets_[i];
        markerDamage=markerDamage.united(markerBounds(overlayMarkers_[i].position));
    }
    for (int i=count;i<overlayMarkerCount_;++i) {
        markerDamage=markerDamage.united(markerBounds(overlayMarkers_[i].position));
        overlayMarkers_[i].image=QImage();
    }
    pendingUpdate_+=markerDamage;
    markerDirty_.fill(false);
    overlayMarkerCount_=count;
    overlayDirty_=false;
    overlayCacheValid_=false;
    overlayChangedNs_=clock_.nsecsElapsed();
    if (profiling_) profile_->phases[FrameProfile::Overlay].add(timing.nsecsElapsed());
}

void EqWidget::rebuildOverlay() {
    QElapsedTimer timing; if (profiling_) timing.start();
    const qreal dpr=devicePixelRatioF();
    const QSize pixels(qCeil(overlayRect_.width()*dpr),qCeil(overlayRect_.height()*dpr));
    if (overlay_.size()!=pixels) overlay_=QImage(pixels,QImage::Format_ARGB32_Premultiplied);
    overlay_.setDevicePixelRatio(dpr); overlay_.fill(Qt::transparent);
    QPainter p(&overlay_); p.translate(-overlayRect_.topLeft()); paintOverlay(p); p.end();
    detail::OverlayCoverage coverage(overlayRect_);
    coverage.addPath(sumPath_,overlayClip_,2+2/dpr);
    coverage.addPath(bandPath_,overlayClip_,2+2/dpr);
    if (overlayBandFillColor_.alpha()>0) coverage.addPath(bandFillPath_,overlayClip_,2/dpr);
    for (int i=0;i<overlayMarkerCount_;++i) coverage.add(QRectF(overlayMarkers_[i].position-QPointF(18,18),QSizeF(36,36)));
    if (overlayExternalCoefficients_) coverage.add(plotRect());
    overlayInk_=coverage.rect();
    overlayCacheValid_=true;
    if (profiling_) profile_->phases[FrameProfile::Overlay].add(timing.nsecsElapsed());
}
void EqWidget::rebuildSpectrum() {
    spectrumPath_=QPainterPath();
    const auto r=plotRect();
    for (int i=0;i<int(spectrumDisplay_.size());++i) {
        const QPointF pt(r.left()+r.width()*i/(spectrumDisplay_.size()-1),
                         r.top()-clamp(spectrumDisplay_[i],-120,0)*r.height()/120);
        if (i==0) spectrumPath_.moveTo(pt); else spectrumPath_.lineTo(pt);
    }
}

void EqWidget::paintEvent(QPaintEvent *event) {
    QElapsedTimer timing; timing.start();
    if (profiling_) {
        const auto now = clock_.nsecsElapsed();
        if (previousProfilePaintNs_ >= 0) profile_->phases[FrameProfile::Interval].add(now - previousProfilePaintNs_);
        previousProfilePaintNs_ = now;
    }
    // Normal exposes reuse the prepared frame. Geometry/DPR/font changes
    // need a matching response and overlay before drawing the new background.
    const bool dprChanged=background_.devicePixelRatioF()!=devicePixelRatioF();
    if (backgroundDirty_ || dprChanged) {
        rebuildBackground();
        // Path simplification uses a physical-pixel error budget.
        if (dprChanged) { sumPathDirty_=true; bandPathDirty_=true; }
        if (responseDirty_ || sumPathDirty_ || bandPathDirty_) rebuildResponse();
        prepareOverlay();
        requestFrame();
    }
    // Parameter edits are prepared together in frameTick(). An unrelated
    // expose must not update the readout ahead of the cached EQ response.
    if (!footerValid_) rebuildFooter();
    QPainter p(this); p.drawImage(QPoint(0,0),background_);
    p.save(); p.setClipRect(plotRect().adjusted(-1,-1,1,1));
    if (spectrumEnabled_ && !spectrumPath_.isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing,false);
        if (fillSpectrum_) {
            auto fill=spectrumPath_;
            fill.lineTo(plotRect().bottomRight()); fill.lineTo(plotRect().bottomLeft()); fill.closeSubpath();
            p.fillPath(fill,QColor(32,58,64));
        }
        p.setPen(QPen(spectrumColor,1)); p.drawPath(spectrumPath_);
    }
    p.restore();
    if (!overlay_.isNull() && overlayCacheValid_) {
        p.save(); p.setClipRect(overlayInk_,Qt::IntersectClip);
        p.drawImage(overlayRect_.topLeft(),overlay_); p.restore();
    }
    else paintOverlay(p,&event->region());
    p.end();
    ++paintedFrames_;
    const auto ns=timing.nsecsElapsed(); totalPaintNs_+=ns; maxPaintNs_=std::max(maxPaintNs_,ns);
    if (profiling_) profile_->phases[FrameProfile::Paint].add(ns);
}

const QImage &EqWidget::markerImage(int i) {
    const bool active=i==selected_;
    const int style=(active?1:0) | (bands()[i].enabled?2:0);
    const qreal dpr=devicePixelRatioF();
    auto &image=markers_[i];
    if (!image.isNull() && image.devicePixelRatioF()==dpr && markerStyles_[i]==style) return image;
    image=QImage(QSize(qCeil(36*dpr),qCeil(36*dpr)),QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr); image.fill(Qt::transparent); markerStyles_[i]=style;
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing,true);
    const QPointF pos(18,18); const auto color=bands()[i].enabled?bandColor(i):muted;
    QFont f=font(); f.setPixelSize(11); f.setBold(true); p.setFont(f);
    if (active) { p.setPen(QPen(color,1)); p.setBrush(Qt::NoBrush); p.drawEllipse(pos,15,15); }
    p.setPen(QPen(color,1.5)); p.setBrush(active?color:background); p.drawEllipse(pos,10,10);
    p.setPen(active?background:color); p.drawText(QRectF(8,8,20,20),Qt::AlignCenter,QString::number(i+1));
    p.end();
    const auto sprite=detail::markerSprite(image);
    image=sprite.image; markerOffsets_[i]=sprite.offset;
    return image;
}

void EqWidget::paintOverlay(QPainter &p,const QRegion *damage) {
    QElapsedTimer timing; if (profiling_) timing.start();
    p.save(); p.setClipRect(overlayRect_,Qt::IntersectClip);
    p.save(); p.setClipRect(overlayClip_,Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing,overlayAntialiasing_);
    if (!bandFillPath_.isEmpty() && overlayBandFillColor_.alpha()>0)
        p.fillPath(bandFillPath_,overlayBandFillColor_);
    if (overlaySelection_>=0) { p.setPen(QPen(bandColor(overlaySelection_).darker(135),1)); p.drawPath(bandPath_); }
    p.setPen(QPen(accent,1.5)); p.drawPath(sumPath_); p.restore();
    if (profiling_) { profile_->phases[FrameProfile::Curves].add(timing.nsecsElapsed()); timing.restart(); }
    p.setRenderHint(QPainter::Antialiasing,true);
    for (int i=0;i<overlayMarkerCount_;++i) {
        const auto &marker=overlayMarkers_[i];
        if (damage && !damage->intersects(QRectF(marker.position-QPointF(18,18),QSizeF(36,36))
                                          .toAlignedRect().adjusted(-1,-1,1,1))) continue;
        p.drawImage(marker.position+marker.offset,marker.image);
    }
    if (overlayExternalCoefficients_) {
        QFont f=overlayFont_;
        f.setBold(false); f.setPixelSize(13); p.setFont(f); p.setPen(muted);
        p.drawText(plotRect().adjusted(10,6,-10,-6),Qt::AlignTop|Qt::AlignRight,tr("External coefficients"));
    }
    p.restore();
    if (profiling_) profile_->phases[FrameProfile::Controls].add(timing.nsecsElapsed());
}

void EqWidget::rebuildFooter() {
    QElapsedTimer timing; if (profiling_) timing.start();
    // Footer pixels are opaque. Cache them in the background instead of
    // repeatedly clearing and alpha-blending a full-widget overlay.
    QPainter p(&background_); p.setRenderHint(QPainter::Antialiasing,true);
    p.setClipRect(QRect(0,height()-76,width(),76));
    if (!footerValid_) {
        // Reuse the adjusted font and its resolved engine throughout edits.
        footerFont_=font(); footerFont_.setBold(false); footerFont_.setPixelSize(13);
        const QRectF label(12,qControlRect().center().y()-12,28,24);
        footerQLabelBounds_=QFontMetricsF(footerFont_,&background_)
            .boundingRect(label,Qt::AlignCenter,QStringLiteral("Q")).toAlignedRect().adjusted(-1,-1,1,1);
    }
    p.setFont(footerFont_); p.setPen(ink);
    const bool reset=!footerValid_ || footerSelection_!=selected_;
    QRegion damage;
    if (reset) {
        const QRect area(0,height()-76,width(),76);
        p.fillRect(area,background); damage+=area;
    }
    if (selected_>=0) {
        const auto &b=bands()[selected_];
        const QString info=QString("%1   %2     %3     %4 dB     Q %5")
            .arg(selected_+1,2,10,QLatin1Char('0')).arg(filterName(b.type))
            .arg(frequencyLabel(b.frequency)).arg(b.gainDb,0,'f',1).arg(b.q,0,'f',2);
        if (reset || info!=footerText_) {
            const QRect area(48,height()-76,width()-48,24);
            p.fillRect(area,background); damage+=area;
            detail::drawFooterText(p,QRectF(48,height()-75,width()-96,22),info,font(),footerTextCache_.get());
        }
        footerText_=info;
        const int sign=(b.gainDb>0)-(b.gainDb<0);
        if (reset || footerType_!=int(b.type) || footerSign_!=sign) {
            const QRect area(0,height()-76,48,24);
            p.fillRect(area,background); damage+=area;
            paintFilterIcon(p,QRectF(15,height()-72,24,18),b.type,b.gainDb,bandColor(selected_));
        }
        footerType_=int(b.type); footerSign_=sign;
        const auto qr=qControlRect(); const qreal y=qr.center().y();
        const bool editable=canEdit() && hasQ(b.type);
        if (reset || footerQ_!=b.q || footerEditable_!=editable) {
            const qreal x=qr.left()+std::log(b.q/0.1)/std::log(300.0)*qr.width();
            QRect area(0,height()-52,width(),52);
            if (!reset && footerEditable_==editable) {
                const qreal oldX=qr.left()+std::log(footerQ_/0.1)/std::log(300.0)*qr.width();
                area=QRectF(std::min(x,oldX)-12,y-12,std::abs(x-oldX)+24,24).toAlignedRect();
            }
            damage+=area;
            p.save(); p.setClipRect(area,Qt::IntersectClip); p.fillRect(area,background);
            const QRectF label(12,y-12,28,24);
            if (area.intersects(footerQLabelBounds_)) {
                p.setPen(editable?ink:muted); p.drawText(label,Qt::AlignCenter,"Q");
            }
            p.setPen(QPen(grid,4,Qt::SolidLine,Qt::RoundCap)); p.drawLine(QPointF(qr.left(),y),QPointF(qr.right(),y));
            p.setPen(QPen(editable?bandColor(selected_):muted,4,Qt::SolidLine,Qt::RoundCap));
            p.drawLine(QPointF(qr.left(),y),QPointF(x,y)); p.setBrush(editable?bandColor(selected_):muted);
            p.setPen(Qt::NoPen); p.drawEllipse(QPointF(x,y),9,9);
            p.restore();
        }
        footerQ_=b.q; footerEditable_=editable;
    } else if (reset) {
        p.setPen(muted); p.drawText(QRectF(30,height()-74,width()-60,56),Qt::AlignCenter,tr("Select an EQ point to edit frequency, gain and Q"));
    }
    p.end(); footerSelection_=selected_; footerValid_=true; footerDirty_=false;
    pendingUpdate_+=damage;
    if (profiling_) profile_->phases[FrameProfile::Footer].add(timing.nsecsElapsed());
}

void EqWidget::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); invalidateGeometry(); }
void EqWidget::showEvent(QShowEvent *e) {
    QWidget::showEvent(e); updateSpectrumDemand(); invalidateGeometry();
}
void EqWidget::hideEvent(QHideEvent *e) {
    frameTimer_.stop(); finishDrag(true); touchSequence_=false; touchId_=pinchId_=-1;
    mailbox_->setAnalysisEnabled(false);
    spectrumGeneration_=mailbox_->analysisState().generation;
    resetSpectrum();
    QWidget::hideEvent(e);
}
void EqWidget::changeEvent(QEvent *e) {
    if (e->type()==QEvent::FontChange || e->type()==QEvent::PaletteChange) {
        for (auto &marker : markers_) marker=QImage();
        backgroundDirty_=true; overlayDirty_=true; requestFrame();
    }
    if (e->type()==QEvent::EnabledChange) {
        if (!isEnabled()) finishDrag(true);
        overlayDirty_=true; footerDirty_=true; requestFrame();
    }
    QWidget::changeEvent(e);
}

int EqWidget::hitBand(QPointF pos) const {
    int best=-1; double distance=touchTarget_*touchTarget_/4.0;
    // Prefer selected band for exact ties; other overlapping bands remain
    // selectable through setSelectedBand() (demo provides a band selector).
    for (int i=0;i<int(bands().size());++i) {
        const auto delta=pos-bandPosition(i); const auto d=QPointF::dotProduct(delta,delta);
        if (d<distance || (d==distance && i==selected_)) { best=i; distance=d; }
    }
    return best;
}
bool EqWidget::canEdit() const {
    return selected_>=0 && isEnabled() && !bands()[selected_].useCustomCoefficients && bands()[selected_].type!=FilterType::Bypass;
}
bool EqWidget::beginDrag(QPointF pos) {
    setFocus(Qt::MouseFocusReason);
    if (qControlRect().contains(pos) && canEdit() && hasQ(bands()[selected_].type)) {
        drag_=Drag::Q; emit bandEditStarted(selected_); setQFromPosition(pos.x()); return true;
    }
    const int index=hitBand(pos);
    if (index<0) {
        // A tap on empty plot space clears the active band, including its fill.
        if (selected_>=0 && plotRect().contains(pos)) {
            setSelectedBand(-1);
            return true;
        }
        return false;
    }
    setSelectedBand(index);
    if (!canEdit()) return true;
    drag_=Drag::Point; dragStart_=pos; pointStart_=bandPosition(index);
    emit bandEditStarted(index); requestFrame(); return true;
}
void EqWidget::editBand(Band b) {
    if (!canEdit()) return;
    const int index=selected_; b=sanitized(b,sampleRate_);
    const auto &old=bands()[index];
    if (b.frequency==old.frequency && b.gainDb==old.gainDb && b.q==old.q) return;
    invalidateBand(index,old,b);
    response_.setBand(index,b); requestFrame();
    emit bandEdited(index,b);
}
void EqWidget::setQFromPosition(qreal x) {
    if (!canEdit() || !hasQ(bands()[selected_].type)) return;
    auto b=bands()[selected_]; const auto r=qControlRect();
    b.q=0.1*std::exp(clamp((x-r.left())/r.width(),0,1)*std::log(300.0)); editBand(b);
}
void EqWidget::moveDrag(QPointF pos) {
    if (!canEdit()) return;
    if (drag_==Drag::Q) { setQFromPosition(pos.x()); return; }
    if (drag_!=Drag::Point) return;
    auto b=bands()[selected_]; const auto target=pointStart_+pos-dragStart_;
    if (hasFrequency(b.type)) b.frequency=frequencyAt(target.x());
    if (hasGain(b.type)) { b.gainDb=gainAt(target.y()); if (std::abs(b.gainDb)<0.15) b.gainDb=0; }
    editBand(b);
}
void EqWidget::finishDrag(bool cancelled) {
    const bool editing=drag_!=Drag::None; drag_=Drag::None; pinchId_=-1;
    if (editing && selected_>=0) emit bandEditFinished(selected_,bands()[selected_],cancelled);
}
void EqWidget::mousePressEvent(QMouseEvent *e) {
    if (touchSequence_ || e->source()==Qt::MouseEventSynthesizedByQt) { e->accept(); return; }
    if (e->button()==Qt::LeftButton && beginDrag(e->localPos())) e->accept(); else e->ignore();
}
void EqWidget::mouseMoveEvent(QMouseEvent *e) {
    if (!touchSequence_ && e->source()!=Qt::MouseEventSynthesizedByQt && drag_!=Drag::None) moveDrag(e->localPos());
}
void EqWidget::mouseReleaseEvent(QMouseEvent *e) {
    if (!touchSequence_ && e->source()!=Qt::MouseEventSynthesizedByQt && e->button()==Qt::LeftButton) {
        moveDrag(e->localPos()); finishDrag();
    }
}
void EqWidget::wheelEvent(QWheelEvent *e) {
    if (!canEdit() || !hasQ(bands()[selected_].type)) { e->ignore(); return; }
    auto b=bands()[selected_]; emit bandEditStarted(selected_);
    b.q*=std::exp(e->angleDelta().y()/120.0*0.12); editBand(b);
    emit bandEditFinished(selected_,bands()[selected_],false); e->accept();
}
void EqWidget::keyPressEvent(QKeyEvent *e) {
    if (!canEdit()) { QWidget::keyPressEvent(e); return; }
    auto b=bands()[selected_]; const double step=e->modifiers().testFlag(Qt::ShiftModifier)?0.1:1;
    switch (e->key()) {
    case Qt::Key_Left: if(hasFrequency(b.type)) b.frequency*=std::pow(2.0,-step/12); break;
    case Qt::Key_Right: if(hasFrequency(b.type)) b.frequency*=std::pow(2.0,step/12); break;
    case Qt::Key_Up: if(hasGain(b.type)) b.gainDb+=step*0.5; break;
    case Qt::Key_Down: if(hasGain(b.type)) b.gainDb-=step*0.5; break;
    case Qt::Key_Plus: case Qt::Key_Equal: if(hasQ(b.type)) b.q*=std::exp(step*0.12); break;
    case Qt::Key_Minus: if(hasQ(b.type)) b.q*=std::exp(-step*0.12); break;
    default: QWidget::keyPressEvent(e); return;
    }
    b.frequency=clamp(b.frequency,minHz_,maxHz_);
    emit bandEditStarted(selected_); editBand(b); emit bandEditFinished(selected_,bands()[selected_],false);
}

bool EqWidget::event(QEvent *e) {
    if (e->type()==QEvent::ScreenChangeInternal) invalidateGeometry();
    if (e->type()==QEvent::TouchCancel) {
        finishDrag(true); touchSequence_=false; touchId_=pinchId_=-1; e->accept(); return true;
    }
    if (e->type()==QEvent::TouchBegin || e->type()==QEvent::TouchUpdate || e->type()==QEvent::TouchEnd) {
        auto *te=static_cast<QTouchEvent *>(e);
        QList<QTouchEvent::TouchPoint> active;
        for (const auto &pt:te->touchPoints()) if (pt.state()!=Qt::TouchPointReleased) active.append(pt);
        if (e->type()==QEvent::TouchBegin) {
            touchSequence_=true;
            if (!active.isEmpty()) { touchId_=active.first().id(); beginDrag(active.first().pos()); }
        }
        if (e->type()==QEvent::TouchEnd || active.isEmpty()) {
            // Qt may put the last movement only in the released point.
            if (pinchId_<0) for (const auto &pt:te->touchPoints()) if (pt.id()==touchId_) moveDrag(pt.pos());
            finishDrag(); touchSequence_=false; touchId_=pinchId_=-1;
        } else if (active.size()>=2 && drag_==Drag::Point && canEdit() && hasQ(bands()[selected_].type)) {
            const QTouchEvent::TouchPoint *primary=nullptr, *secondary=nullptr;
            for (const auto &pt:active) if (pt.id()==touchId_) primary=&pt;
            if (!primary) { primary=&active.first(); touchId_=primary->id(); pinchId_=-1; }
            for (const auto &pt:active) if (pt.id()==pinchId_ && pt.id()!=touchId_) secondary=&pt;
            if (!secondary) for (const auto &pt:active) if(pt.id()!=touchId_) { secondary=&pt; break; }
            const double distance=QLineF(primary->pos(),secondary->pos()).length();
            if (pinchId_!=secondary->id() || pinchDistance_<8) {
                pinchId_=secondary->id(); pinchDistance_=std::max(8.0,distance); pinchQ_=bands()[selected_].q;
            } else {
                auto b=bands()[selected_]; b.q=pinchQ_*pinchDistance_/std::max(8.0,distance); editBand(b);
            }
        } else {
            const QTouchEvent::TouchPoint *primary=nullptr;
            for (const auto &pt:active) if (pt.id()==touchId_) primary=&pt;
            if (!primary) primary=&active.first();
            if (pinchId_>=0 || primary->id()!=touchId_) {
                touchId_=primary->id(); pinchId_=-1; dragStart_=primary->pos();
                if (selected_>=0) pointStart_=bandPosition(selected_);
            } else if (e->type()!=QEvent::TouchBegin) moveDrag(primary->pos());
        }
        e->accept(); return true;
    }
    if (e->type()==QEvent::WindowDeactivate || e->type()==QEvent::UngrabMouse) finishDrag(true);
    return QWidget::event(e);
}

} // namespace qdsssp
