#pragma once

#include "audio_config.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DeepFilterProcessor {
public:
    struct Snapshot {
        bool available = false;
        float strength = 0.0f;
        size_t frameLength = 0;
        double meanMilliseconds = 0.0;
        double maximumMilliseconds = 0.0;
        uint64_t framesProcessed = 0;
        uint64_t deadlineMisses = 0;
        uint64_t inputOverruns = 0;
        uint64_t outputUnderruns = 0;
    };

    explicit DeepFilterProcessor(const Options& options);
    ~DeepFilterProcessor();

    DeepFilterProcessor(const DeepFilterProcessor&) = delete;
    DeepFilterProcessor& operator=(const DeepFilterProcessor&) = delete;

    bool available() const;
    void set_strength(float value);
    void process(std::vector<int16_t>& samples, size_t frames,
                 unsigned int channels, Routing routing);
    Snapshot snapshot() const;

private:
    class SampleRing;
    void worker_loop();
    void load(const Options& options);
    void unload();

    void* libraryHandle_ = nullptr;
    void* state_ = nullptr;
    using ProcessFrame = float (*)(void*, float*, float*);
    using FreeState = void (*)(void*);
    ProcessFrame processFrame_ = nullptr;
    FreeState freeState_ = nullptr;

    size_t frameLength_ = 0;
    unsigned int sampleRate_ = 48000;
    size_t dryDelaySamples_ = 0;
    std::unique_ptr<SampleRing> inputRing_;
    std::unique_ptr<SampleRing> outputRing_;
    std::vector<float> dryDelay_;
    size_t dryDelayPosition_ = 0;
    std::vector<float> workerInput_;
    std::vector<float> workerOutput_;

    std::atomic<bool> available_{false};
    std::atomic<bool> stop_{false};
    std::atomic<float> targetStrength_{0.0f};
    float smoothedStrength_ = 0.0f;
    std::condition_variable workerWake_;
    std::mutex workerMutex_;
    std::thread worker_;

    std::atomic<uint64_t> framesProcessed_{0};
    std::atomic<uint64_t> totalNanoseconds_{0};
    std::atomic<uint64_t> maximumNanoseconds_{0};
    std::atomic<uint64_t> deadlineMisses_{0};
    std::atomic<uint64_t> inputOverruns_{0};
    std::atomic<uint64_t> outputUnderruns_{0};
};
