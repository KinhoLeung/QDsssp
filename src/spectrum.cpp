#include "qdsssp/spectrum.h"
#include <algorithm>
#include <cmath>

namespace qdsssp {
// Provide storage for C++14 callers that use this public constant by reference.
constexpr int SpectrumFrame::MaxBins;

namespace {
float clean(float db) {
    return std::isfinite(db) ? std::max(-120.0f, std::min(24.0f, db)) : -120.0f;
}
}
void SpectrumMailbox::setAnalysisEnabled(bool enabled) {
    // Serialize demand changes with publication, but never wait for an FFT.
    std::lock_guard<std::mutex> lock(mutex_);
    const auto previous = analysisState_.load(std::memory_order_relaxed);
    if (bool(previous & 1) == enabled) return;
    pending_ = false;
    const auto next = ((previous + 2) & ~std::uint64_t(1)) | std::uint64_t(enabled);
    analysisState_.store(next, std::memory_order_release);
}
SpectrumMailbox::AnalysisState SpectrumMailbox::analysisState() const {
    const auto value = analysisState_.load(std::memory_order_acquire);
    return {value, bool(value & 1)};
}
bool SpectrumMailbox::publish(const float *db, int count, double fs) {
    return publish(db, count, fs, analysisState());
}
bool SpectrumMailbox::publish(const float *db, int count, double fs, AnalysisState state) {
    if (!db || count < 2 || count > SpectrumFrame::MaxBins || !std::isfinite(fs) || fs < 100 || fs > 768000)
        return false;
    if (!state.enabled) return false;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const auto current = analysisState_.load(std::memory_order_relaxed);
    if (!(current & 1) || state.generation != current) return false;
    std::copy_n(db, count, frame_.db.begin());
    frame_.count = count; frame_.sampleRate = fs; ++frame_.sequence;
    pending_ = true;
    return true;
}
bool SpectrumMailbox::takeLatest(SpectrumFrame &destination) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !pending_) return false;
    destination.count = frame_.count; destination.sampleRate = frame_.sampleRate;
    destination.sequence = frame_.sequence;
    std::copy_n(frame_.db.begin(), frame_.count, destination.db.begin());
    pending_ = false;
    return true;
}

void SpectrumProjector::configure(int count, double lo, double hi, int bins, double fs) {
    if (count < 2 || count > 4096 || bins < 2 || bins > SpectrumFrame::MaxBins ||
        !std::isfinite(fs) || fs < 100 || !std::isfinite(lo) || !std::isfinite(hi) || lo <= 0 || hi <= lo) {
        buckets_.clear(); frequencies_.clear(); bins_ = 0; return;
    }
    if (count == int(buckets_.size()) && bins == bins_ && fs == sampleRate_ && lo == minHz_ && hi == maxHz_) return;
    bins_ = bins; sampleRate_ = fs; minHz_ = lo; maxHz_ = hi;
    buckets_.resize(count); frequencies_.resize(count);
    const double step = std::log(hi/lo)/(count-1), binHz = fs/(2*(bins-1));
    for (int i = 0; i < count; ++i) {
        const double hz = lo * std::exp(i*step);
        frequencies_[i] = hz;
        const double left = i == 0 ? lo : hz*std::exp(-step/2);
        const double right = i == count-1 ? hi : hz*std::exp(step/2);
        const int first = std::max(0, int(std::ceil(left/binHz)));
        // Half-open intervals, except the last bin, cover every visible peak.
        const int last = std::min(bins-1, i == count-1 ? int(std::floor(right/binHz)) : int(std::ceil(right/binHz))-1);
        const double pos = std::min(double(bins-1), hz/binHz);
        const int near = std::min(bins-2, int(pos));
        buckets_[i] = {first, last, near, float(pos-near)};
    }
}
void SpectrumProjector::project(const SpectrumFrame &frame, std::vector<float> &db) const {
    db.assign(buckets_.size(), -120);
    if (frame.count != bins_ || frame.sampleRate != sampleRate_) return;
    for (int i = 0; i < int(buckets_.size()); ++i) {
        if (frequencies_[i] > sampleRate_/2) continue;
        const auto &b = buckets_[i];
        if (b.first <= b.last) {
            float peak = -120;
            for (int j = b.first; j <= b.last; ++j) peak = std::max(peak, clean(frame.db[j]));
            db[i] = peak;
        } else {
            const float a = clean(frame.db[b.near]), next = clean(frame.db[b.near+1]);
            db[i] = a + (next-a)*b.fraction;
        }
    }
}

} // namespace qdsssp
