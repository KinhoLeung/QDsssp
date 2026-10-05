#pragma once

#include "eq_response.h"
#include "spectrum.h"
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QPainterPath>
#include <QRegion>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <memory>

namespace qdsssp {
namespace detail { class FooterTextCache; }

// Opt-in diagnostics; samples are bounded and allocated only when profiling.
struct TimingSamples {
    quint64 count = 0;
    qint64 totalNs = 0, maxNs = 0;
    std::vector<qint64> samples;
    void add(qint64 ns) {
        ++count; totalNs += ns; maxNs = std::max(maxNs, ns);
        if (samples.size() < 16384) samples.push_back(ns);
    }
};
struct FrameProfile {
    // Curves/Controls run inside Paint for dynamic EQ frames, or inside
    // Overlay when building the static cache; these timings are nested.
    enum Phase { Preparation, Response, Paths, Overlay, Curves, Controls, Spectrum, Paint, Interval, Footer, PhaseCount };
    std::array<TimingSamples, PhaseCount> phases;
};

class EqWidget : public QWidget {
    Q_OBJECT
public:
    explicit EqWidget(QWidget *parent = nullptr);
    ~EqWidget() override;
    QSize sizeHint() const override { return {800, 480}; }
    QSize minimumSizeHint() const override { return {320, 240}; }
    void setBands(const std::vector<Band> &bands);
    void setBand(int index, const Band &band); // silent programmatic update
    const std::vector<Band> &bands() const { return response_.bands(); }
    int selectedBand() const { return selected_; }
    void setSelectedBand(int index); // -1 clears the selection
    void setSelectedBandFillEnabled(bool enabled); // fill between response and 0 dB
    bool selectedBandFillEnabled() const { return fillSelectedBand_; }
    void setSelectedBandFillOpacity(double opacity); // 0..1; default 0.25
    double selectedBandFillOpacity() const { return selectedBandFillOpacity_; }
    void setSampleRate(double sampleRate);
    double sampleRate() const { return sampleRate_; }
    void setFrameRate(int fps); // 10..60; applies to graph, not input delivery
    void setSpectrumEnabled(bool enabled); // also pauses built-in FFT when false / hidden
    bool spectrumEnabled() const { return spectrumEnabled_; }
    void setSpectrumFillEnabled(bool enabled);
    void setCurveAntialiasing(bool enabled);
    void setTouchTargetSize(int logicalPixels); // default 48, minimum 32
    std::shared_ptr<SpectrumMailbox> spectrumInput() const { return mailbox_; }
    QRectF plotRect() const;
    QRectF qControlRect() const;
    QPointF bandPosition(int index) const;
    double frequencyAt(qreal x) const;
    double gainAt(qreal y) const;
    quint64 paintedFrames() const { return paintedFrames_; }
    unsigned long responseEvaluations() const { return response_.evaluationCount(); }
    qint64 totalPaintNs() const { return totalPaintNs_; }
    qint64 maxPaintNs() const { return maxPaintNs_; }
    qint64 totalPreparationNs() const { return totalPreparationNs_; }
    qint64 maxPreparationNs() const { return maxPreparationNs_; }
    quint64 preparedFrames() const { return preparedFrames_; }
    void startProfiling(); // resets samples; GUI thread only
    void stopProfiling() { profiling_ = false; }
    const FrameProfile *profile() const { return profile_.get(); }
signals:
    void selectedBandChanged(int index);
    void bandEditStarted(int index);
    void bandEdited(int index, qdsssp::Band band);
    // Cancel keeps the last value and ends the transaction; it does not roll back.
    void bandEditFinished(int index, qdsssp::Band band, bool cancelled);
protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void changeEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    enum class Drag { None, Point, Q };
    void requestFrame();
    void frameTick();
    void rebuildBackground();
    void rebuildResponse();
    void prepareOverlay();
    void rebuildOverlay();
    void paintOverlay(QPainter &painter, const QRegion *damage = nullptr);
    void rebuildFooter();
    const QImage &markerImage(int index);
    void rebuildSpectrum();
    void resetSpectrum();
    void updateSpectrumDemand();
    void invalidateBand(int index, const Band &previous, const Band &next);
    void invalidateSelection(int previous, int next);
    void invalidateGeometry();
    int hitBand(QPointF position) const;
    bool beginDrag(QPointF position);
    void moveDrag(QPointF position);
    void finishDrag(bool cancelled = false);
    void editBand(Band band);
    void setQFromPosition(qreal x);
    bool canEdit() const;
    qreal frequencyX(double hz) const;
    qreal gainY(double db) const;
    QPainterPath makeResponsePath(const std::vector<double> &db) const;
    QRect responseBounds(const QPainterPath &path) const;
    QRect spectrumBounds(const QPainterPath &path) const;

    ResponseCache response_;
    std::shared_ptr<SpectrumMailbox> mailbox_;
    SpectrumFrame spectrumFrame_;
    std::uint64_t spectrumGeneration_ = 0;
    SpectrumProjector projector_;
    std::vector<float> spectrumTarget_, spectrumDisplay_;
    QImage background_, overlay_;
    QRect overlayRect_;
    QRect overlayInk_;
    QRectF overlayClip_;
    QFont overlayFont_, footerFont_;
    QRect footerQLabelBounds_;
    std::unique_ptr<detail::FooterTextCache> footerTextCache_;
    QColor overlayBandFillColor_;
    struct OverlayMarker { QPointF position,offset; QImage image; };
    std::array<OverlayMarker, ResponseCache::MaxBands> overlayMarkers_;
    int overlayMarkerCount_ = 0, overlaySelection_ = -1;
    bool overlayAntialiasing_ = true, overlayExternalCoefficients_ = false;
    // The bitmap belongs to the last prepared snapshot, even if a setter
    // has already dirtied the model for the next frame.
    bool overlayCacheValid_ = false;
    qint64 overlayChangedNs_ = 0;
    bool footerDirty_ = true, footerValid_ = false;
    QString footerText_;
    int footerSelection_ = -2, footerType_ = -1, footerSign_ = 0;
    double footerQ_ = -1;
    bool footerEditable_ = false;
    std::array<QImage, ResponseCache::MaxBands> markers_;
    std::array<QPointF, ResponseCache::MaxBands> markerOffsets_;
    std::array<int, ResponseCache::MaxBands> markerStyles_{};
    std::array<bool, ResponseCache::MaxBands> markerDirty_{};
    QPainterPath sumPath_, bandPath_, bandFillPath_, spectrumPath_;
    // Damage belongs to prepared snapshots, including changes prepared during
    // an expose. It is accumulated until frameTick queues a widget update.
    QRegion pendingUpdate_;
    QTimer frameTimer_;
    QElapsedTimer clock_;
    qint64 nextFrameNs_ = 0, lastSpectrumMs_ = -1000, lastTickMs_ = 0;
    double sampleRate_ = 48000, minHz_ = 20, maxHz_ = 20000;
    double selectedBandFillOpacity_ = 0.25;
    int fps_ = 30, selected_ = -1, touchTarget_ = 48;
    bool backgroundDirty_ = true, responseDirty_ = true;
    // Selection changes only the selected path, not numerical responses or
    // the total path. Edits to other bands preserve the selected path.
    bool sumPathDirty_ = true, bandPathDirty_ = true;
    bool bandFillDirty_ = true, fillSelectedBand_ = true;
    bool spectrumGeometryDirty_ = true;
    bool overlayDirty_ = true;
    bool spectrumEnabled_ = true, fillSpectrum_ = false, antialiasing_ = true;
    quint64 paintedFrames_ = 0;
    quint64 preparedFrames_ = 0;
    qint64 totalPaintNs_ = 0, maxPaintNs_ = 0, totalPreparationNs_ = 0, maxPreparationNs_ = 0;
    std::unique_ptr<FrameProfile> profile_;
    bool profiling_ = false;
    qint64 previousProfilePaintNs_ = -1;
    Drag drag_ = Drag::None;
    QPointF dragStart_, pointStart_;
    int touchId_ = -1, pinchId_ = -1;
    bool touchSequence_ = false;
    double pinchDistance_ = 0, pinchQ_ = 1;
};

} // namespace qdsssp
Q_DECLARE_METATYPE(qdsssp::Band)
