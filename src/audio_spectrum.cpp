#include "qdsssp/audio_spectrum.h"
#include "spectrum_fft.h"

#include <QFile>
#include <QString>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(QDSSSP_HAS_PIPEWIRE)
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

namespace qdsssp {

namespace {
bool validConfig(const PipeWireSpectrumSource::Config &config) {
    return config.sampleRate >= 100 && config.sampleRate <= 768000 &&
           config.channels >= 1 && config.channels <= 32 &&
           config.fftSize >= 256 && config.fftSize <= 16384 &&
           (config.fftSize & (config.fftSize - 1)) == 0 &&
           config.hopSize >= 1 && config.hopSize <= config.fftSize;
}

class FloatRing {
public:
    // Single producer / single consumer. Both sources use fftSize * 8;
    // validated FFT sizes make capacity a nonzero power of two.
    explicit FloatRing(std::size_t capacity) : data_(capacity), mask_(capacity - 1) {
        assert(capacity != 0 && (capacity & (capacity - 1)) == 0);
    }

    // Accept the available prefix without waiting or overwriting unread data.
    // The caller drops any remainder, matching the existing overflow policy.
    std::size_t push(const float *values, std::size_t count) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        const auto available = data_.size() - std::size_t(write - read);
        const auto accepted = std::min(count, available);
        if (accepted == 0) return 0;
        const auto offset = std::size_t(write) & mask_;
        const auto first = std::min(accepted, data_.size() - offset);
        std::copy_n(values, first, data_.data() + offset);
        std::copy_n(values + first, accepted - first, data_.data());
        // Publish only after both spans have been copied.
        write_.store(write + accepted, std::memory_order_release);
        return accepted;
    }

    // Return immediately with whatever is ready, up to count. No extra
    // batching delay is introduced when only part of a window is available.
    std::size_t pop(float *values, std::size_t count) {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        const auto taken = std::min(count, std::size_t(write - read));
        if (taken == 0) return 0;
        const auto offset = std::size_t(read) & mask_;
        const auto first = std::min(taken, data_.size() - offset);
        std::copy_n(data_.data() + offset, first, values);
        std::copy_n(data_.data(), taken - first, values + first);
        // Release slots only after the consumer has finished copying them.
        read_.store(read + taken, std::memory_order_release);
        return taken;
    }

    // Consumer-only snapshot, used to bound a catch-up pass. Samples arriving
    // after this snapshot stay queued for the next pass.
    std::size_t readable() const {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        return std::size_t(write - read);
    }

private:
    std::vector<float> data_;
    const std::size_t mask_;
    std::atomic<std::uint64_t> read_{0};
    std::atomic<std::uint64_t> write_{0};
};

// Owned exclusively by the analyzer thread. Capture keeps using the SPSC
// ring while this rolling history retains a complete recent window even when
// FFT is paused. Unlike shifting n-hop samples, it also retains the last hop.
class AnalysisWindow {
public:
    AnalysisWindow(int size, int hop)
        : history_(std::size_t(size)), scratch_(std::size_t(size)), hop_(std::size_t(hop)) {}

    bool prepare(FloatRing &ring, SpectrumMailbox::AnalysisState state) {
        if (state.generation != generation_) {
            generation_ = state.generation;
            immediate_ = true;
        }

        std::size_t received = 0;
        if (!state.enabled || immediate_) {
            // Drain only the backlog present at entry. Resume computes the
            // latest complete window instead of replaying queued old windows.
            auto remaining = ring.readable();
            while (remaining != 0) {
                const auto taken = ring.pop(scratch_.data(), std::min(remaining, scratch_.size()));
                if (taken == 0) break;
                append(taken);
                received += taken;
                remaining -= taken;
            }
        } else {
            const auto needed = filled_ < history_.size() ? history_.size() - filled_ : hop_ - sinceFrame_;
            const auto taken = ring.pop(scratch_.data(), needed);
            append(taken);
            received = taken;
        }
        if (received != 0) lastInput_ = std::chrono::steady_clock::now();
        if (!state.enabled || filled_ < history_.size()) return false;
        // Do not revive a long-expired window if capture stopped during pause.
        if (immediate_ && std::chrono::steady_clock::now() - lastInput_ > std::chrono::milliseconds(250))
            return false;
        if (!immediate_ && sinceFrame_ < hop_) return false;
        immediate_ = false;
        sinceFrame_ = 0;
        return true;
    }

    const float *data() const { return history_.data(); }
    // position_ points to the oldest sample once the history is full.
    std::size_t oldest() const { return position_; }

private:
    void append(std::size_t count) {
        if (count == 0) return;
        const auto first = std::min(count, history_.size() - position_);
        std::copy_n(scratch_.data(), first, history_.data() + position_);
        std::copy_n(scratch_.data() + first, count - first, history_.data());
        position_ = (position_ + count) & (history_.size() - 1);
        filled_ = std::min(history_.size(), filled_ + count);
        sinceFrame_ = std::min(hop_, sinceFrame_ + count);
    }

    std::vector<float> history_, scratch_;
    const std::size_t hop_;
    std::size_t position_ = 0, filled_ = 0, sinceFrame_ = 0;
    std::uint64_t generation_ = 0;
    bool immediate_ = true;
    std::chrono::steady_clock::time_point lastInput_{};
};

} // namespace

struct PipeWireSpectrumSource::Impl {
    explicit Impl(std::shared_ptr<SpectrumMailbox> outputIn)
        : output(std::move(outputIn)) {}

    std::shared_ptr<SpectrumMailbox> output;
    Config config;
    std::unique_ptr<FloatRing> ring;
    std::thread thread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    std::atomic<int> actualRate{48000};
    mutable std::mutex errorMutex;
    std::string errorText;

    void setError(const std::string &message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        errorText = message;
    }

    std::string error() const {
        std::lock_guard<std::mutex> lock(errorMutex);
        return errorText;
    }

    void analyze() {
        const int n = config.fftSize;
        const int hop = config.hopSize;
        AnalysisWindow window(n, hop);
        detail::SpectrumFft fft(n);
        std::vector<float> db(std::size_t(n / 2 + 1));

        while (!stopRequested.load(std::memory_order_acquire)) {
            const auto state = output->analysisState();
            if (!window.prepare(*ring, state)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            fft.analyze(window.data(), window.oldest(), db);
            output->publish(db.data(), int(db.size()), actualRate.load(std::memory_order_relaxed), state);
        }
    }

#if defined(QDSSSP_HAS_PIPEWIRE)
    struct PipeWireState {
        std::atomic<pw_main_loop *> loop{nullptr};
        pw_stream *stream = nullptr;
        spa_audio_info format{};
        Impl *owner = nullptr;
    } pw;

    static void onParamChanged(void *userdata, std::uint32_t id, const spa_pod *param) {
        auto *state = static_cast<PipeWireState *>(userdata);
        if (!param || id != SPA_PARAM_Format) return;
        if (spa_format_parse(param, &state->format.media_type, &state->format.media_subtype) < 0) return;
        if (state->format.media_type != SPA_MEDIA_TYPE_audio ||
            state->format.media_subtype != SPA_MEDIA_SUBTYPE_raw) return;
        spa_format_audio_raw_parse(param, &state->format.info.raw);
        state->owner->actualRate.store(int(state->format.info.raw.rate), std::memory_order_relaxed);
    }

    static void onProcess(void *userdata) {
        auto *state = static_cast<PipeWireState *>(userdata);
        auto *buffer = pw_stream_dequeue_buffer(state->stream);
        if (!buffer) return;
        auto *spaBuffer = buffer->buffer;
        auto *chunk = spaBuffer->datas[0].chunk;
        auto *bytes = static_cast<std::uint8_t *>(spaBuffer->datas[0].data);
        const int channels = std::max(1, int(state->format.info.raw.channels));
        const int stride = chunk && chunk->stride > 0 ? chunk->stride : int(sizeof(float) * channels);
        const std::size_t offset = chunk ? chunk->offset : 0;
        const std::size_t size = chunk ? chunk->size : 0;
        if (bytes && size >= std::size_t(stride) && stride >= int(sizeof(float) * channels)) {
            const auto frames = size / std::size_t(stride);
            // Fixed scratch space: no allocation or waiting in the callback.
            std::array<float, 256> mono;
            for (std::size_t first = 0; first < frames; first += mono.size()) {
                const auto count = std::min(mono.size(), frames - first);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto *samples = reinterpret_cast<const float *>(
                        bytes + offset + (first + i) * std::size_t(stride));
                    float value = 0.0f;
                    for (int channel = 0; channel < channels; ++channel)
                        value += samples[channel];
                    mono[i] = value / channels;
                }
                state->owner->ring->push(mono.data(), count);
            }
        }
        pw_stream_queue_buffer(state->stream, buffer);
    }

    static void onStateChanged(void *userdata, pw_stream_state oldState,
                               pw_stream_state state, const char *error) {
        auto *pwState = static_cast<PipeWireState *>(userdata);
        if (state == PW_STREAM_STATE_ERROR && error) pwState->owner->setError(error);
        (void)oldState;
    }

    static const pw_stream_events &events() {
        static const pw_stream_events value = [] {
            pw_stream_events result{};
            result.version = PW_VERSION_STREAM_EVENTS;
            result.state_changed = onStateChanged;
            result.param_changed = onParamChanged;
            result.process = onProcess;
            return result;
        }();
        return value;
    }

    void runPipeWire() {
        int argc = 0;
        char **argv = nullptr;
        pw_init(&argc, &argv);
        auto *loop = pw_main_loop_new(nullptr);
        pw.loop.store(loop, std::memory_order_release);
        if (!loop) {
            setError("pw_main_loop_new failed");
            running.store(false, std::memory_order_release);
            pw_deinit();
            return;
        }
        pw.owner = this;

        auto *properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                              PW_KEY_MEDIA_CATEGORY, "Capture",
                                              PW_KEY_MEDIA_ROLE, "Music", nullptr);
        if (!properties) {
            setError("pw_properties_new failed");
            pw_main_loop_destroy(loop);
            pw.loop.store(nullptr, std::memory_order_release);
            running.store(false, std::memory_order_release);
            pw_deinit();
            return;
        }
        if (!config.targetNode.empty())
            pw_properties_set(properties, PW_KEY_TARGET_OBJECT, config.targetNode.c_str());
        if (config.captureSinkMonitor)
            pw_properties_set(properties, PW_KEY_STREAM_CAPTURE_SINK, "true");
        pw.stream = pw_stream_new_simple(pw_main_loop_get_loop(loop),
                                         "qdsssp-spectrum", properties, &events(), &pw);
        if (!pw.stream) {
            setError("pw_stream_new_simple failed");
            pw_main_loop_destroy(loop);
            pw.loop.store(nullptr, std::memory_order_release);
            running.store(false, std::memory_order_release);
            pw_deinit();
            return;
        }

        std::uint8_t podBuffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
        spa_audio_info_raw raw{};
        raw.format = SPA_AUDIO_FORMAT_F32;
        raw.channels = std::uint32_t(config.channels);
        raw.rate = std::uint32_t(config.sampleRate);
        const spa_pod *params[] = {
            spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw)
        };
        const int result = pw_stream_connect(pw.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                             pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT |
                                                             PW_STREAM_FLAG_MAP_BUFFERS |
                                                             PW_STREAM_FLAG_RT_PROCESS),
                                             params, 1);
        if (result < 0) {
            setError("pw_stream_connect failed: " + std::to_string(result));
            pw_stream_destroy(pw.stream);
            pw.stream = nullptr;
            pw_main_loop_destroy(loop);
            pw.loop.store(nullptr, std::memory_order_release);
            running.store(false, std::memory_order_release);
            pw_deinit();
            return;
        }
        pw_main_loop_run(loop);
        if (pw.stream) pw_stream_destroy(pw.stream);
        pw.stream = nullptr;
        pw_main_loop_destroy(loop);
        pw.loop.store(nullptr, std::memory_order_release);
        pw_deinit();
    }
#endif

    void run() {
        ring.reset(new FloatRing(std::size_t(config.fftSize) * 8));
        running.store(true, std::memory_order_release);
#if defined(QDSSSP_HAS_PIPEWIRE)
        std::thread analyzer([this] { analyze(); });
        runPipeWire();
        stopRequested.store(true, std::memory_order_release);
        analyzer.join();
#else
        setError("QDsssp was built without PipeWire support");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
        running.store(false, std::memory_order_release);
    }

    void stop() {
        stopRequested.store(true, std::memory_order_release);
#if defined(QDSSSP_HAS_PIPEWIRE)
        auto *loop = pw.loop.load(std::memory_order_acquire);
        if (loop) pw_main_loop_quit(loop);
#endif
        if (thread.joinable()) thread.join();
        ring.reset();
        stopRequested.store(false, std::memory_order_release);
    }
};

PipeWireSpectrumSource::PipeWireSpectrumSource(std::shared_ptr<SpectrumMailbox> output)
    : impl_(new Impl(std::move(output))) {}

PipeWireSpectrumSource::~PipeWireSpectrumSource() { stop(); }

bool PipeWireSpectrumSource::start(const Config &config) {
    if (!impl_->output || !validConfig(config)) {
        impl_->setError("invalid PipeWire spectrum configuration");
        return false;
    }
#if !defined(QDSSSP_HAS_PIPEWIRE)
    impl_->setError("QDsssp was built without PipeWire support");
    return false;
#else
    stop();
    impl_->config = config;
    impl_->setError({});
    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
#endif
}

bool PipeWireSpectrumSource::start() { return start(Config{}); }

void PipeWireSpectrumSource::stop() { impl_->stop(); }

void PipeWireSpectrumSource::setPaused(bool paused) {
    if (impl_->output) impl_->output->setAnalysisEnabled(!paused);
}

bool PipeWireSpectrumSource::isPaused() const {
    return impl_->output && !impl_->output->analysisState().enabled;
}

bool PipeWireSpectrumSource::isRunning() const {
    return impl_->running.load(std::memory_order_acquire);
}

std::string PipeWireSpectrumSource::error() const { return impl_->error(); }

struct PcmFileSpectrumSource::Impl {
    explicit Impl(std::shared_ptr<SpectrumMailbox> outputIn)
        : output(std::move(outputIn)) {}

    std::shared_ptr<SpectrumMailbox> output;
    Config config;
    std::unique_ptr<FloatRing> ring;
    std::thread thread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> inputSamples{0};
    std::atomic<std::uint64_t> droppedSamples{0};
    std::atomic<std::uint64_t> fftFrames{0};
    std::atomic<std::uint64_t> publishedFrames{0};
    std::atomic<std::uint64_t> fftNanoseconds{0};
    std::atomic<std::uint64_t> fftMaxNanoseconds{0};
    mutable std::mutex errorMutex;
    std::string errorText;

    void setError(const std::string &message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        errorText = message;
    }

    std::string error() const {
        std::lock_guard<std::mutex> lock(errorMutex);
        return errorText;
    }

    void resetStats() {
        inputSamples.store(0, std::memory_order_relaxed);
        droppedSamples.store(0, std::memory_order_relaxed);
        fftFrames.store(0, std::memory_order_relaxed);
        publishedFrames.store(0, std::memory_order_relaxed);
        fftNanoseconds.store(0, std::memory_order_relaxed);
        fftMaxNanoseconds.store(0, std::memory_order_relaxed);
    }

    void recordMax(std::uint64_t value) {
        auto current = fftMaxNanoseconds.load(std::memory_order_relaxed);
        while (current < value && !fftMaxNanoseconds.compare_exchange_weak(
                   current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    void analyze() {
        const int n = config.fftSize;
        const int hop = config.hopSize;
        AnalysisWindow window(n, hop);
        detail::SpectrumFft fft(n);
        std::vector<float> db(std::size_t(n / 2 + 1));

        while (!stopRequested.load(std::memory_order_acquire)) {
            const auto state = output->analysisState();
            if (!window.prepare(*ring, state)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const auto fftStart = std::chrono::steady_clock::now();
            fft.analyze(window.data(), window.oldest(), db);
            const auto fftNs = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - fftStart).count());
            fftNanoseconds.fetch_add(fftNs, std::memory_order_relaxed);
            recordMax(fftNs);
            fftFrames.fetch_add(1, std::memory_order_relaxed);
            if (output->publish(db.data(), int(db.size()), config.sampleRate, state))
                publishedFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void run() {
        ring.reset(new FloatRing(std::size_t(config.fftSize) * 8));
        running.store(true, std::memory_order_release);
        std::thread analyzer([this] { analyze(); });

        QFile file(QString::fromStdString(config.path));
        if (!file.open(QIODevice::ReadOnly)) {
            setError("cannot open PCM file: " + config.path);
        } else if (file.size() < 2) {
            setError("PCM file is empty: " + config.path);
        } else {
            const int chunkBytes = 4096;
            std::array<float, 256> mono;
            while (!stopRequested.load(std::memory_order_acquire)) {
                if (file.atEnd()) {
                    if (!config.loop || !file.seek(0)) break;
                }
                const QByteArray bytes = file.read(chunkBytes);
                const int frames = bytes.size() / int(sizeof(std::int16_t));
                if (frames <= 0) break;
                const auto *raw = reinterpret_cast<const std::uint8_t *>(bytes.constData());
                std::size_t accepted = 0;
                for (std::size_t first = 0; first < std::size_t(frames); first += mono.size()) {
                    const auto count = std::min(mono.size(), std::size_t(frames) - first);
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto offset = (first + i) * 2;
                        const std::uint16_t word = std::uint16_t(raw[offset]) |
                                                   (std::uint16_t(raw[offset + 1]) << 8);
                        const auto sample = static_cast<std::int16_t>(word);
                        mono[i] = float(sample) / 32768.0f;
                    }
                    accepted += ring->push(mono.data(), count);
                }
                inputSamples.fetch_add(std::uint64_t(frames), std::memory_order_relaxed);
                const auto dropped = std::size_t(frames) - accepted;
                if (dropped != 0)
                    droppedSamples.fetch_add(dropped, std::memory_order_relaxed);
                const auto milliseconds = std::chrono::milliseconds(
                    std::max(1, int(1000.0 * frames / config.sampleRate)));
                std::this_thread::sleep_for(milliseconds);
            }
        }

        stopRequested.store(true, std::memory_order_release);
        analyzer.join();
        running.store(false, std::memory_order_release);
    }

    void stop() {
        stopRequested.store(true, std::memory_order_release);
        if (thread.joinable()) thread.join();
        ring.reset();
        stopRequested.store(false, std::memory_order_release);
    }
};

PcmFileSpectrumSource::PcmFileSpectrumSource(std::shared_ptr<SpectrumMailbox> output)
    : impl_(new Impl(std::move(output))) {}

PcmFileSpectrumSource::~PcmFileSpectrumSource() { stop(); }

bool PcmFileSpectrumSource::start(const Config &config) {
    if (!impl_->output || config.sampleRate < 100 || config.fftSize < 256 ||
        config.fftSize > 16384 || (config.fftSize & (config.fftSize - 1)) != 0 ||
        config.hopSize < 1 || config.hopSize > config.fftSize) {
        impl_->setError("invalid PCM file spectrum configuration");
        return false;
    }
    stop();
    impl_->config = config;
    impl_->setError({});
    impl_->resetStats();
    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
}

void PcmFileSpectrumSource::stop() { impl_->stop(); }

void PcmFileSpectrumSource::setPaused(bool paused) {
    if (impl_->output) impl_->output->setAnalysisEnabled(!paused);
}

bool PcmFileSpectrumSource::isPaused() const {
    return impl_->output && !impl_->output->analysisState().enabled;
}

bool PcmFileSpectrumSource::isRunning() const {
    return impl_->running.load(std::memory_order_acquire);
}

std::string PcmFileSpectrumSource::error() const { return impl_->error(); }

PcmFileSpectrumSource::Stats PcmFileSpectrumSource::stats() const {
    Stats result;
    result.inputSamples = impl_->inputSamples.load(std::memory_order_relaxed);
    result.droppedSamples = impl_->droppedSamples.load(std::memory_order_relaxed);
    result.fftFrames = impl_->fftFrames.load(std::memory_order_relaxed);
    result.publishedFrames = impl_->publishedFrames.load(std::memory_order_relaxed);
    result.fftNanoseconds = impl_->fftNanoseconds.load(std::memory_order_relaxed);
    result.fftMaxNanoseconds = impl_->fftMaxNanoseconds.load(std::memory_order_relaxed);
    return result;
}

void PcmFileSpectrumSource::resetStats() { impl_->resetStats(); }

} // namespace qdsssp
