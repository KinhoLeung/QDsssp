#pragma once

#include "spectrum.h"
#include <cstdint>
#include <memory>
#include <string>

namespace qdsssp {

// Captures PCM from PipeWire and publishes a latest-only dBFS spectrum.
// The default target is the current sink monitor, which is the digital signal
// immediately before the board's DAC. It does not read the analog DAC output.
class PipeWireSpectrumSource {
public:
    struct Config {
        // Leave empty to use PipeWire's current/default sink monitor. Set this
        // to a node.name when the board has more than one playback device.
        std::string targetNode;
        int sampleRate = 48000;
        int channels = 2;
        int fftSize = 2048;
        int hopSize = 1024;
        bool captureSinkMonitor = true;
    };

    explicit PipeWireSpectrumSource(std::shared_ptr<SpectrumMailbox> output);
    ~PipeWireSpectrumSource();

    PipeWireSpectrumSource(const PipeWireSpectrumSource &) = delete;
    PipeWireSpectrumSource &operator=(const PipeWireSpectrumSource &) = delete;

    bool start(const Config &config);
    bool start();
    void stop();
    // Pauses FFT only; capture and recent PCM history continue. This controls
    // the shared mailbox demand (also controlled by an attached EqWidget).
    // An in-flight FFT may finish, but its result will be discarded.
    void setPaused(bool paused);
    bool isPaused() const;
    bool isRunning() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reads little-endian signed 16-bit mono PCM and publishes a looping FFT
// spectrum. This is useful for a deterministic demo source when no live
// PipeWire/UAC stream is available.
class PcmFileSpectrumSource {
public:
    struct Stats {
        std::uint64_t inputSamples = 0;
        std::uint64_t droppedSamples = 0;
        std::uint64_t fftFrames = 0;
        std::uint64_t publishedFrames = 0;
        std::uint64_t fftNanoseconds = 0;
        std::uint64_t fftMaxNanoseconds = 0;
    };

    struct Config {
        std::string path = "/opt/qdsssp/assets/DOGFIGHT.pcm";
        int sampleRate = 44100;
        int fftSize = 2048;
        int hopSize = 1024;
        bool loop = true;
    };

    explicit PcmFileSpectrumSource(std::shared_ptr<SpectrumMailbox> output);
    ~PcmFileSpectrumSource();

    PcmFileSpectrumSource(const PcmFileSpectrumSource &) = delete;
    PcmFileSpectrumSource &operator=(const PcmFileSpectrumSource &) = delete;

    bool start(const Config &config);
    void stop();
    // Keeps advancing through the PCM file and retaining the latest window.
    // Controls shared mailbox demand; resume does not restart the file.
    void setPaused(bool paused);
    bool isPaused() const;
    bool isRunning() const;
    std::string error() const;
    Stats stats() const;
    void resetStats();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qdsssp
