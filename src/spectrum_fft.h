#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <vector>

namespace qdsssp { namespace detail {

// A reusable real-input FFT plan. Two real samples share one complex value,
// so the butterfly transform is only N/2 points. Hann coefficients, input
// permutation and twiddles are computed once, outside the analysis hot loop.
class SpectrumFft {
public:
    explicit SpectrumFft(int size)
        : size_(std::size_t(size)), hann_(size_), permutation_(size_ / 2),
          twiddles_(size_ / 2), values_(size_ / 2) {
        assert(size >= 256 && size <= 16384 && (size & (size - 1)) == 0);
        constexpr double pi = 3.14159265358979323846;
        double windowSum = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            hann_[i] = float(0.5 - 0.5 * std::cos(2 * pi * i / size_));
            windowSum += hann_[i];
        }
        scaleSquared_ = float(1 / (windowSum * windowSum));
        for (std::size_t i = 0, reversed = 0; i < values_.size(); ++i) {
            permutation_[i] = reversed;
            std::size_t bit = values_.size() >> 1;
            for (; reversed & bit; bit >>= 1) reversed ^= bit;
            reversed ^= bit;
            const double angle = -2 * pi * i / size_;
            twiddles_[i] = {float(std::cos(angle)), float(std::sin(angle))};
        }
    }

    // samples contains a full circular window; oldest is its first sample in
    // chronological order. Reuse output capacity; no per-frame scratch alloc.
    void analyze(const float *samples, std::size_t oldest, std::vector<float> &db) {
        const std::size_t half = values_.size(), mask = size_ - 1;
        for (std::size_t i = 0; i < half; ++i) {
            const std::size_t even = 2 * i;
            values_[permutation_[i]] = {samples[(oldest + even) & mask] * hann_[even],
                                       samples[(oldest + even + 1) & mask] * hann_[even + 1]};
        }
        // The first stage has only unity twiddles.
        for (std::size_t i = 0; i < half; i += 2) {
            const auto a = values_[i], b = values_[i + 1];
            values_[i] = {a.real + b.real, a.imag + b.imag};
            values_[i + 1] = {a.real - b.real, a.imag - b.imag};
        }
        for (std::size_t length = 4; length <= half; length <<= 1) {
            const std::size_t stride = size_ / length;
            for (std::size_t first = 0; first < half; first += length) {
                for (std::size_t i = 0; i < length / 2; ++i) {
                    const auto a = values_[first + i], b = values_[first + i + length / 2];
                    const auto w = twiddles_[i * stride];
                    const float real = w.real * b.real - w.imag * b.imag;
                    const float imag = w.real * b.imag + w.imag * b.real;
                    values_[first + i] = {a.real + real, a.imag + imag};
                    values_[first + i + length / 2] = {a.real - real, a.imag - imag};
                }
            }
        }
        db.resize(half + 1);
        const auto zero = values_[0];
        db[0] = toDb((zero.real + zero.imag) * (zero.real + zero.imag), scaleSquared_);
        db[half] = toDb((zero.real - zero.imag) * (zero.real - zero.imag), scaleSquared_);
        for (std::size_t k = 1; k < half; ++k) {
            const auto a = values_[k], b = values_[half - k], w = twiddles_[k];
            // X[k] = (Z[k] + conj(Z[M-k]))/2
            //        - i*W_N[k]*(Z[k] - conj(Z[M-k]))/2.
            const float differenceReal = a.real - b.real, sumImag = a.imag + b.imag;
            const float real = 0.5f * (a.real + b.real + w.imag * differenceReal + w.real * sumImag);
            const float imag = 0.5f * (a.imag - b.imag - w.real * differenceReal + w.imag * sumImag);
            db[k] = toDb(real * real + imag * imag, 4 * scaleSquared_);
        }
    }

private:
    struct Complex { float real, imag; };
    static float toDb(float power, float scale) {
        return 10.0f * std::log10(std::max(power * scale, 1.0e-12f));
    }
    const std::size_t size_;
    std::vector<float> hann_;
    std::vector<std::size_t> permutation_;
    std::vector<Complex> twiddles_, values_;
    float scaleSquared_ = 1;
};

}} // namespace qdsssp::detail
