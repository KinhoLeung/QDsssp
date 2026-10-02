#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace qdsssp {

// Real FFT, single-sided, uniform bins: bin[k] = k*sampleRate/(2*(count-1)).
// Values are amplitude dBFS, including DC and Nyquist; no implicit FFT scaling.
struct SpectrumFrame {
    static constexpr int MaxBins = 8193;
    std::array<float, MaxBins> db{};
    int count = 0;
    double sampleRate = 48000;
    std::uint64_t sequence = 0;
};

// Latest-only bounded mailbox, share independently of QWidget's lifetime.
// publish/take use try_lock: contention drops/defer frames instead of waiting.
// No heap allocation here. Intended for an FFT worker, not a hard-RT guarantee.
class SpectrumMailbox {
public:
    struct AnalysisState {
        std::uint64_t generation;
        bool enabled;
    };
    // One consumer controls demand. Standalone mailboxes default to enabled;
    // EqWidget updates demand from its visibility and spectrum switch.
    void setAnalysisEnabled(bool enabled);
    AnalysisState analysisState() const;
    bool publish(const float *db, int count, double sampleRate);
    // Built-in analyzers capture state BEFORE computing. Reject results from
    // an earlier pause/resume generation, including an FFT already in flight.
    bool publish(const float *db, int count, double sampleRate, AnalysisState state);
    bool takeLatest(SpectrumFrame &destination);
private:
    std::mutex mutex_;
    // Low bit is demand; remaining bits distinguish successive transitions.
    std::atomic<std::uint64_t> analysisState_{1};
    SpectrumFrame frame_;
    bool pending_ = false;
};

// Peak-preserving logarithmic display buckets. Empty low-frequency buckets
// interpolate neighbouring FFT bins; that does not improve FFT resolution.
class SpectrumProjector {
public:
    void configure(int points, double minHz, double maxHz, int bins, double sampleRate);
    void project(const SpectrumFrame &frame, std::vector<float> &db) const;
    const std::vector<double> &frequencies() const { return frequencies_; }
private:
    struct Bucket { int first, last, near; float fraction; };
    std::vector<Bucket> buckets_;
    std::vector<double> frequencies_;
    int bins_ = 0;
    double sampleRate_ = 0, minHz_ = 0, maxHz_ = 0;
};

} // namespace qdsssp
