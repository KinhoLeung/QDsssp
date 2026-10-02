#pragma once

#include <array>
#include <vector>

namespace qdsssp {

// Values correspond to the DSSSP filter IDs. Coefficients below use the usual
// DSP convention: numerator b0,b1,b2; denominator 1,a1,a2.
enum class FilterType {
    Bypass = 0, HighShelf1 = 1, HighShelf2 = 2, LowShelf1 = 3,
    LowShelf2 = 4, Notch = 5, Peak = 6, HighPass1 = 7,
    HighPass2 = 8, LowPass1 = 9, LowPass2 = 10, BandPass = 11, Gain = 12
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

struct Band {
    FilterType type = FilterType::Peak;
    double frequency = 1000;
    double gainDb = 0;
    double q = 0.7071067811865476;
    bool enabled = true;
    // Optional exact DSP coefficients (e.g. vendor-specific Q definition).
    bool useCustomCoefficients = false;
    Biquad coefficients;
};

inline bool operator==(const Biquad &a, const Biquad &b) {
    return a.b0 == b.b0 && a.b1 == b.b1 && a.b2 == b.b2 &&
           a.a1 == b.a1 && a.a2 == b.a2;
}
inline bool operator==(const Band &a, const Band &b) {
    return a.type == b.type && a.frequency == b.frequency &&
           a.gainDb == b.gainDb && a.q == b.q && a.enabled == b.enabled &&
           a.useCustomCoefficients == b.useCustomCoefficients &&
           a.coefficients == b.coefficients;
}

bool hasGain(FilterType type);
bool hasQ(FilterType type);
bool hasFrequency(FilterType type);
Band sanitized(Band band, double sampleRate);
Biquad designBiquad(const Band &band, double sampleRate);
double magnitudeDb(const Biquad &coefficients, double frequency, double sampleRate);
const char *filterName(FilterType type);

// All methods belong to the GUI/control thread; there is no audio processing.
// Grid trigonometry is cached; a parameter edit evaluates only the dirty band.
class ResponseCache {
public:
    static constexpr int MaxBands = 32;
    ResponseCache();
    void setGrid(int points, double minHz, double maxHz, double sampleRate);
    void setBands(const std::vector<Band> &bands);
    void setBand(int index, const Band &band);
    bool update();
    const std::vector<double> &frequencies() const { return frequencies_; }
    const std::vector<double> &sumDb() const { return sum_; }
    const std::vector<double> &bandDb(int index) const { return curves_.at(index); }
    const std::vector<Band> &bands() const { return bands_; }
    unsigned long evaluationCount() const { return evaluations_; }
private:
    std::vector<Band> bands_;
    std::array<std::vector<double>, MaxBands> curves_;
    std::array<bool, MaxBands> dirty_{};
    std::vector<double> frequencies_, cos1_, sin1_, cos2_, sin2_, sum_;
    double sampleRate_ = 48000, minHz_ = 20, maxHz_ = 20000;
    bool sumDirty_ = true;
    bool rebuildSum_ = true;
    unsigned int incrementalUpdates_ = 0;
    unsigned long evaluations_ = 0;
};

} // namespace qdsssp
