#include "qdsssp/eq_response.h"
#include <algorithm>
#include <cmath>

namespace qdsssp {
// C++14 needs storage when the constexpr member binds to a reference,
// as it does in std::min() below.
constexpr int ResponseCache::MaxBands;

namespace {
constexpr double pi = 3.14159265358979323846;
double bounded(double v, double lo, double hi, double fallback) {
    return std::isfinite(v) ? std::max(lo, std::min(hi, v)) : fallback;
}
double response(const Biquad &c, double cs, double sn, double cs2, double sn2) {
    // Complex evaluation avoids cancellation in the expanded cos polynomial
    // for low-frequency/high-Q filters. No trig in the per-band hot loop.
    const double nr = c.b0 + c.b1 * cs + c.b2 * cs2;
    const double ni = c.b1 * sn + c.b2 * sn2;
    const double dr = 1 + c.a1 * cs + c.a2 * cs2;
    const double di = c.a1 * sn + c.a2 * sn2;
    const double power = (nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di);
    return bounded(10 * std::log10(std::max(1e-20, power)), -200, 200, -200);
}
}

bool hasGain(FilterType t) {
    return t == FilterType::Peak || t == FilterType::Gain ||
           t == FilterType::LowShelf1 || t == FilterType::LowShelf2 ||
           t == FilterType::HighShelf1 || t == FilterType::HighShelf2;
}
bool hasQ(FilterType t) {
    return t == FilterType::Peak || t == FilterType::Notch ||
           t == FilterType::BandPass || t == FilterType::LowPass2 ||
           t == FilterType::HighPass2 || t == FilterType::LowShelf2 ||
           t == FilterType::HighShelf2;
}
bool hasFrequency(FilterType t) { return t != FilterType::Gain && t != FilterType::Bypass; }

Band sanitized(Band b, double fs) {
    fs = bounded(fs, 100, 768000, 48000);
    if (int(b.type) < 0 || int(b.type) > 12) b.type = FilterType::Bypass;
    b.frequency = bounded(b.frequency, 1, fs * 0.499, std::min(1000.0, fs * 0.25));
    b.gainDb = bounded(b.gainDb, -24, 24, 0);
    b.q = bounded(b.q, 0.1, 30, 0.7071067811865476);
    const auto &c = b.coefficients;
    if (!(std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) &&
          std::isfinite(c.a1) && std::isfinite(c.a2))) b.coefficients = Biquad{};
    return b;
}

Biquad designBiquad(const Band &input, double fs) {
    fs = bounded(fs, 100, 768000, 48000);
    const auto b = sanitized(input, fs);
    if (!b.enabled || b.type == FilterType::Bypass) return {};
    if (b.useCustomCoefficients) return b.coefficients;
    const double k = std::tan(pi * b.frequency / fs), k2 = k * k;
    const double v = std::pow(10.0, std::abs(b.gainDb) / 20.0);
    const double d = k / b.q, s = std::sqrt(v) * d;
    // Bilinear-transform polynomials. Swapping numerator and denominator
    // gives reciprocal boost/cut curves, matching DSSSP's Q convention.
    double n0 = 1, n1 = 0, n2 = 0, d0 = 1, d1 = 0, d2 = 0;
    switch (b.type) {
    case FilterType::Peak:
        n0 = 1 + v*d + k2; n1 = 2*(k2-1); n2 = 1-v*d+k2;
        d0 = 1+d+k2; d1 = n1; d2 = 1-d+k2; break;
    case FilterType::LowShelf1:
        n0 = 1+v*k; n1 = v*k-1; d0 = 1+k; d1 = k-1; break;
    case FilterType::HighShelf1:
        n0 = v+k; n1 = k-v; d0 = 1+k; d1 = k-1; break;
    case FilterType::LowShelf2:
        n0 = 1+s+v*k2; n1 = 2*(v*k2-1); n2 = 1-s+v*k2;
        d0 = 1+d+k2; d1 = 2*(k2-1); d2 = 1-d+k2; break;
    case FilterType::HighShelf2:
        n0 = v+s+k2; n1 = 2*(k2-v); n2 = v-s+k2;
        d0 = 1+d+k2; d1 = 2*(k2-1); d2 = 1-d+k2; break;
    case FilterType::LowPass1:
        n0 = k; n1 = k; d0 = 1+k; d1 = k-1; break;
    case FilterType::HighPass1:
        n0 = 1; n1 = -1; d0 = 1+k; d1 = k-1; break;
    case FilterType::LowPass2:
        n0 = k2; n1 = 2*k2; n2 = k2;
        d0 = 1+d+k2; d1 = 2*(k2-1); d2 = 1-d+k2; break;
    case FilterType::HighPass2:
        n0 = 1; n1 = -2; n2 = 1;
        d0 = 1+d+k2; d1 = 2*(k2-1); d2 = 1-d+k2; break;
    case FilterType::BandPass:
        n0 = d; n1 = 0; n2 = -d;
        d0 = 1+d+k2; d1 = 2*(k2-1); d2 = 1-d+k2; break;
    case FilterType::Notch:
        n0 = 1+k2; n1 = 2*(k2-1); n2 = n0;
        d0 = 1+d+k2; d1 = n1; d2 = 1-d+k2; break;
    case FilterType::Gain: n0 = std::pow(10.0, b.gainDb/20); break;
    case FilterType::Bypass: break;
    }
    if (b.gainDb < 0 && hasGain(b.type) && b.type != FilterType::Gain) {
        std::swap(n0, d0); std::swap(n1, d1); std::swap(n2, d2);
    }
    return {n0/d0, n1/d0, n2/d0, d1/d0, d2/d0};
}

double magnitudeDb(const Biquad &c, double hz, double fs) {
    if (!std::isfinite(fs) || fs <= 0 || !std::isfinite(hz)) return -200;
    const double w = 2*pi*hz/fs;
    return response(c, std::cos(w), std::sin(w), std::cos(2*w), std::sin(2*w));
}

const char *filterName(FilterType t) {
    static const char *names[] = {"Bypass", "High shelf 6", "High shelf 12", "Low shelf 6",
        "Low shelf 12", "Notch", "Peak", "High pass 6", "High pass 12",
        "Low pass 6", "Low pass 12", "Band pass", "Gain"};
    return int(t) >= 0 && int(t) <= 12 ? names[int(t)] : "Unknown";
}

ResponseCache::ResponseCache() { setGrid(800, 20, 20000, 48000); }
void ResponseCache::setGrid(int count, double lo, double hi, double fs) {
    fs = bounded(fs, 100, 768000, 48000);
    hi = bounded(hi, 2, fs*0.499, std::min(20000.0, fs*0.499));
    lo = bounded(lo, 1, hi*0.99, 1);
    count = std::max(2, std::min(4096, count));
    if (count == int(frequencies_.size()) && lo == minHz_ && hi == maxHz_ && fs == sampleRate_) return;
    minHz_ = lo; maxHz_ = hi; sampleRate_ = fs;
    frequencies_.resize(count); cos1_.resize(count); sin1_.resize(count);
    cos2_.resize(count); sin2_.resize(count); sum_.assign(count, 0);
    for (auto &curve : curves_) curve.assign(count, 0);
    for (int i = 0; i < count; ++i) {
        frequencies_[i] = lo * std::exp(std::log(hi/lo)*i/(count-1));
        const double w = 2*pi*frequencies_[i]/fs;
        cos1_[i] = std::cos(w); sin1_[i] = std::sin(w);
        cos2_[i] = std::cos(2*w); sin2_[i] = std::sin(2*w);
    }
    for (auto &band : bands_) band = sanitized(band, fs);
    dirty_.fill(true); sumDirty_ = true; rebuildSum_ = true;
}
void ResponseCache::setBands(const std::vector<Band> &bands) {
    const auto count = std::min(bands.size(), std::size_t(MaxBands));
    if (count == bands_.size()) {
        // Preserve both displayed curves and pending dirty flags when a
        // parameter source sends a complete snapshot with only a few edits.
        for (std::size_t i = 0; i < count; ++i) setBand(int(i), bands[i]);
        return;
    }
    std::vector<Band> replacement(bands.begin(), bands.begin() + count);
    bands_.swap(replacement);
    for (auto &b : bands_) b = sanitized(b, sampleRate_);
    dirty_.fill(true); sumDirty_ = true; rebuildSum_ = true;
}
void ResponseCache::setBand(int i, const Band &b) {
    if (i < 0 || i >= int(bands_.size())) return;
    const auto value = sanitized(b, sampleRate_);
    if (bands_[i] == value) return;
    bands_[i] = value; dirty_[i] = true; sumDirty_ = true;
}
bool ResponseCache::update() {
    if (!sumDirty_) return false;
    // Rebase periodically to prevent roundoff accumulating during long edits.
    const bool rebuild = rebuildSum_ || incrementalUpdates_ >= 256;
    for (int band = 0; band < int(bands_.size()); ++band) {
        if (!dirty_[band]) continue;
        const auto &b = bands_[band];
        auto &curve = curves_[band];
        // Match designBiquad's bypass precedence. Parameter-based shortcuts
        // must not override active external coefficients, even at zero gain.
        const bool bypass = !b.enabled || b.type == FilterType::Bypass;
        const bool constant = bypass || (!b.useCustomCoefficients &&
            (b.type == FilterType::Gain || (hasGain(b.type) && b.gainDb == 0.0)));
        if (constant) {
            // Bands are sanitized on entry. Gain is constant in dB; exactly
            // zero-gain Peak/shelf filters and bypass have unity response.
            const double value = bypass ? 0.0 : b.gainDb;
            if (rebuild) {
                std::fill(curve.begin(), curve.end(), value);
            } else {
                for (int i = 0; i < int(sum_.size()); ++i) {
                    sum_[i] += value - curve[i];
                    curve[i] = value;
                }
            }
        } else {
            const auto c = designBiquad(b, sampleRate_);
            for (int i = 0; i < int(sum_.size()); ++i) {
                const double value = response(c, cos1_[i], sin1_[i], cos2_[i], sin2_[i]);
                if (!rebuild) sum_[i] += value - curve[i];
                curve[i] = value;
            }
        }
        dirty_[band] = false; ++evaluations_;
    }
    if (rebuild) {
        std::fill(sum_.begin(), sum_.end(), 0);
        for (int band = 0; band < int(bands_.size()); ++band)
            for (int i = 0; i < int(sum_.size()); ++i) sum_[i] += curves_[band][i];
        incrementalUpdates_ = 0;
    } else ++incrementalUpdates_;
    rebuildSum_ = false;
    sumDirty_ = false;
    return true;
}

} // namespace qdsssp
