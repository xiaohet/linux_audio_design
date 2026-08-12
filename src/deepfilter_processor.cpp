#include "deepfilter_processor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <filesystem>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace {
// The audio thread and inference worker exchange individual mono samples through
// bounded lock-free SPSC queues.  A generous fixed capacity absorbs short OS
// scheduling stalls without allocating memory in the real-time path.
constexpr size_t QueueCapacity = 16384;
// ALSA commonly hands us 512 samples at once while DeepFilterNet works in
// 480-sample frames.  Four frames give the worker enough look-ahead to cross
// two ALSA period boundaries without making an on-time frame look late.
constexpr size_t SchedulingFrames = 4;

struct TaggedSample {
    // sequence identifies the original captured sample.  It lets the audio
    // thread match delayed model output with the corresponding dry signal.
    float value = 0.0f;
    uint64_t sequence = 0;
};

std::filesystem::path resolve_deepfilter_path(const std::string& configured) {
    // Relative defaults work both when DeepFilterNet is inside this repository
    // and when the same checkout has been moved to the user's home directory.
    std::filesystem::path path(configured);
    if(std::filesystem::exists(path) || path.is_absolute()) return path;
    const char* home = std::getenv("HOME");
    if(home && configured.rfind("DeepFilterNet/", 0) == 0) {
        const std::filesystem::path homePath =
            std::filesystem::path(home) / configured;
        if(std::filesystem::exists(homePath)) return homePath;
    }
    return path;
}

template<typename T>
T load_symbol(void* handle, const char* name) {
    // Resolve the C API at runtime so realtime_audio can still start when the
    // optional DeepFilterNet library was not linked at build time.
    dlerror();
    void* address = dlsym(handle, name);
    if(const char* error = dlerror())
        throw std::runtime_error(std::string("Missing DeepFilterNet symbol ") +
                                 name + ": " + error);
    return reinterpret_cast<T>(address);
}
}

class DeepFilterProcessor::SampleRing {
public:
    explicit SampleRing(size_t capacity) : data_(capacity) {}

    size_t size() const {
        return write_.load(std::memory_order_acquire) -
               read_.load(std::memory_order_acquire);
    }

    size_t free_space() const { return data_.size() - size(); }

    bool push(const TaggedSample& value) {
        // Only the audio thread writes the input ring and only the worker writes
        // the output ring, so acquire/release indices are sufficient here.
        const size_t write = write_.load(std::memory_order_relaxed);
        if(write - read_.load(std::memory_order_acquire) >= data_.size())
            return false;
        data_[write % data_.size()] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool pop(TaggedSample& value) {
        const size_t read = read_.load(std::memory_order_relaxed);
        if(read == write_.load(std::memory_order_acquire)) return false;
        value = data_[read % data_.size()];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    bool peek(TaggedSample& value) const {
        const size_t read = read_.load(std::memory_order_relaxed);
        if(read == write_.load(std::memory_order_acquire)) return false;
        value = data_[read % data_.size()];
        return true;
    }

private:
    std::vector<TaggedSample> data_;
    std::atomic<size_t> read_{0};
    std::atomic<size_t> write_{0};
};

DeepFilterProcessor::DeepFilterProcessor(const Options& options)
    : sampleRate_(options.rate), targetStrength_(options.deepFilterStrength) {
    load(options);
}

DeepFilterProcessor::~DeepFilterProcessor() {
    unload();
}

void DeepFilterProcessor::load(const Options& options) {
    // Empty or missing paths mean that suppression is optional and unavailable;
    // malformed libraries and models, in contrast, are reported as errors.
    if(options.deepFilterLibrary.empty() || options.deepFilterModel.empty()) return;
    const std::filesystem::path libraryPath =
        resolve_deepfilter_path(options.deepFilterLibrary);
    const std::filesystem::path modelPath =
        resolve_deepfilter_path(options.deepFilterModel);
    if(!std::filesystem::exists(libraryPath) ||
       !std::filesystem::exists(modelPath)) return;
    if(sampleRate_ != 48000)
        throw std::runtime_error("DeepFilterNet requires a 48000 Hz sample rate");

    libraryHandle_ = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if(!libraryHandle_) {
        const char* error = dlerror();
        throw std::runtime_error("Cannot load DeepFilterNet library: " +
                                 std::string(error ? error : "unknown error"));
    }

    try {
        // DeepFilterNet v0.5.6 exposes this two-argument model constructor plus
        // frame processing, frame-length discovery, and destruction functions.
        using Create = void* (*)(const char*, float);
        using FrameLength = size_t (*)(void*);
        const Create create = load_symbol<Create>(libraryHandle_, "df_create");
        const FrameLength getFrameLength =
            load_symbol<FrameLength>(libraryHandle_, "df_get_frame_length");
        processFrame_ = load_symbol<ProcessFrame>(libraryHandle_, "df_process_frame");
        freeState_ = load_symbol<FreeState>(libraryHandle_, "df_free");

        const std::string model = modelPath.string();
        state_ = create(model.c_str(),
                        options.deepFilterAttenuationLimitDb);
        if(!state_) throw std::runtime_error("DeepFilterNet model creation failed");
        frameLength_ = getFrameLength(state_);
        if(frameLength_ == 0 || frameLength_ >= QueueCapacity / 4)
            throw std::runtime_error("DeepFilterNet returned an invalid frame length");

        inputRing_ = std::make_unique<SampleRing>(QueueCapacity);
        outputRing_ = std::make_unique<SampleRing>(QueueCapacity);
        workerInput_.resize(frameLength_);
        workerOutput_.resize(frameLength_);
        workerSequences_.resize(frameLength_);

        // Total dry delay contains two parts: the model's inherent algorithmic
        // delay and a scheduling reserve for the asynchronous inference worker.
        schedulingDelaySamples_ = SchedulingFrames * frameLength_;
        modelDelaySamples_ = options.deepFilterDelaySamples;
        dryDelaySamples_ = modelDelaySamples_ + schedulingDelaySamples_;
        dryDelay_.assign(std::max<size_t>(dryDelaySamples_, 1), 0.0f);
        // The fixed delay is the ordinary fallback path.  The tagged history
        // supports exact dry/wet alignment when a processed frame arrives late.
        dryHistory_.assign(QueueCapacity * 2, 0.0f);
        dryHistorySequences_.assign(QueueCapacity * 2,
                                    std::numeric_limits<uint64_t>::max());

        available_ = true;
        worker_ = std::thread(&DeepFilterProcessor::worker_loop, this);
    } catch(...) {
        unload();
        throw;
    }
}

void DeepFilterProcessor::unload() {
    // Stop and join before releasing the model or shared library: the worker may
    // currently be executing code or accessing state owned by either one.
    stop_ = true;
    workerWake_.notify_all();
    if(worker_.joinable()) worker_.join();
    available_ = false;
    if(state_ && freeState_) freeState_(state_);
    state_ = nullptr;
    if(libraryHandle_) dlclose(libraryHandle_);
    libraryHandle_ = nullptr;
}

bool DeepFilterProcessor::available() const { return available_.load(); }

void DeepFilterProcessor::set_strength(float value) {
    // Store only a target here; process() smooths changes to avoid zipper noise.
    targetStrength_ = std::clamp(value, 0.0f, 1.0f);
}

void DeepFilterProcessor::process(std::vector<int16_t>& samples, size_t frames,
                                  unsigned int channels, Routing routing) {
    // This function runs on the ALSA audio thread.  It performs bounded queue
    // operations only; neural-network inference remains on worker_loop().
    if(!available() || channels == 0) return;
    const float smoothing = 1.0f - std::exp(-1.0f / (0.040f * sampleRate_));
    bool outputUnderrunThisPeriod = false;

    for(size_t frame = 0; frame < frames; ++frame) {
        const size_t offset = frame * channels;
        // DeepFilterNet is mono.  Routing has already duplicated mono inputs;
        // preserve both stereo sources by averaging only in stereo mode.
        float mono = samples[offset] / 32768.0f;
        if(channels > 1 && routing == Routing::Stereo)
            mono = 0.5f * (mono + samples[offset + 1] / 32768.0f);

        // Send the new sample to inference and retain a tagged dry copy.  If the
        // input queue is full, audio continues and the diagnostic is incremented.
        const uint64_t sequence = audioSequence_++;
        if(!inputRing_->push({mono, sequence})) ++inputOverruns_;

        const size_t historyPosition = sequence % dryHistory_.size();
        dryHistory_[historyPosition] = mono;
        dryHistorySequences_[historyPosition] = sequence;

        // This constant-latency dry signal is safe to play whenever inference
        // has not produced a usable enhanced sample.
        const float delayedDry = dryDelay_[dryDelayPosition_];
        dryDelay_[dryDelayPosition_] = mono;
        dryDelayPosition_ = (dryDelayPosition_ + 1) % dryDelay_.size();

        float enhanced = delayedDry;
        float alignedDry = delayedDry;
        bool haveEnhanced = false;
        if(sequence >= schedulingDelaySamples_) {
            // 'expected' is the newest model-output tag that is due for playout
            // after allowing the worker its configured scheduling reserve.
            const uint64_t expected = sequence - schedulingDelaySamples_;
            TaggedSample candidate;
            // Do not demand sample-exact arrival here.  The audio callback
            // advances a whole ALSA period at a time, so a valid DF frame can
            // arrive a little behind the nominal sequence.  Only discard it
            // when it is more than the complete scheduling reserve behind.
            while(outputRing_->peek(candidate) &&
                  candidate.sequence + schedulingDelaySamples_ < expected) {
                outputRing_->pop(candidate);
                ++staleOutputSamples_;
            }
            if(outputRing_->peek(candidate) && candidate.sequence <= expected) {
                outputRing_->pop(candidate);
                enhanced = candidate.value;
                // DeepFilterNet output already contains its algorithmic delay.
                // Retrieve dry audio from that same acoustic instant before
                // blending; mixing with current dry audio would sound like echo.
                if(candidate.sequence >= modelDelaySamples_) {
                    const uint64_t drySequence =
                        candidate.sequence - modelDelaySamples_;
                    const size_t dryPosition =
                        drySequence % dryHistory_.size();
                    if(dryHistorySequences_[dryPosition] == drySequence) {
                        alignedDry = dryHistory_[dryPosition];
                        haveEnhanced = true;
                    }
                } else {
                    alignedDry = 0.0f;
                    haveEnhanced = true;
                }
                if(!haveEnhanced) outputUnderrunThisPeriod = true;
            } else {
                outputUnderrunThisPeriod = true;
            }
        }
        // Missing output falls back to delayed dry rather than silence or stale
        // model data, keeping the stream continuous during a scheduling spike.
        if(!haveEnhanced) enhanced = delayedDry;

        // A 40 ms one-pole ramp makes suppression changes click-free.  At true
        // zero, bypass the delayed path so "off" has no DeepFilter latency.
        const float target = targetStrength_.load(std::memory_order_relaxed);
        smoothedStrength_ += smoothing * (target - smoothedStrength_);
        float mixed;
        if(target <= 0.0f && smoothedStrength_ < 0.0001f) {
            smoothedStrength_ = 0.0f;
            mixed = mono;
        } else {
            mixed = alignedDry * (1.0f - smoothedStrength_) +
                    enhanced * smoothedStrength_;
        }
        mixed = std::clamp(mixed, -1.0f, 0.999969f);
        const int16_t value = static_cast<int16_t>(std::lrint(mixed * 32768.0f));
        for(size_t channel = 0; channel < channels; ++channel)
            samples[offset + channel] = value;
    }
    if(outputUnderrunThisPeriod) ++outputUnderruns_;
    workerWake_.notify_one();
}

void DeepFilterProcessor::worker_loop() {
    // One model frame represents its real-time inference deadline (10 ms for a
    // 480-sample frame at 48 kHz).  Timing statistics compare work against it.
    const uint64_t deadlineNanoseconds =
        static_cast<uint64_t>(frameLength_) * 1000000000ull / sampleRate_;
    while(!stop_.load()) {
        // Wait until a complete input frame and a complete output slot exist;
        // DeepFilterNet's C API cannot process partial frames.
        if(inputRing_->size() < frameLength_ ||
           outputRing_->free_space() < frameLength_) {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerWake_.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }

        // Preserve every input tag while copying the frame into contiguous model
        // buffers.  The same tags accompany enhanced samples back to audio.
        for(size_t index = 0; index < frameLength_; ++index) {
            TaggedSample sample;
            inputRing_->pop(sample);
            workerInput_[index] = sample.value;
            workerSequences_[index] = sample.sequence;
        }
        // Measure inference only, excluding FIFO waits and sample copies.
        const auto start = std::chrono::steady_clock::now();
        processFrame_(state_, workerInput_.data(), workerOutput_.data());
        const uint64_t elapsed = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
        ++framesProcessed_;
        totalNanoseconds_.fetch_add(elapsed, std::memory_order_relaxed);
        uint64_t currentMaximum = maximumNanoseconds_.load();
        while(elapsed > currentMaximum &&
              !maximumNanoseconds_.compare_exchange_weak(currentMaximum, elapsed)) {}
        if(elapsed > deadlineNanoseconds) ++deadlineMisses_;
        // Publish a complete enhanced frame.  A failed push can only follow an
        // unexpected queue-state change and is handled by audio-side fallback.
        for(size_t index = 0; index < frameLength_; ++index) {
            if(!outputRing_->push({workerOutput_[index], workerSequences_[index]}))
                break;
        }
    }
}

DeepFilterProcessor::Snapshot DeepFilterProcessor::snapshot() const {
    // Copy atomics into a coherent, read-only view for the terminal and webpage.
    Snapshot result;
    result.available = available();
    result.strength = targetStrength_.load();
    result.frameLength = frameLength_;
    result.framesProcessed = framesProcessed_.load();
    result.deadlineMisses = deadlineMisses_.load();
    result.inputOverruns = inputOverruns_.load();
    result.outputUnderruns = outputUnderruns_.load();
    result.staleOutputSamples = staleOutputSamples_.load();
    const uint64_t total = totalNanoseconds_.load();
    result.meanMilliseconds = result.framesProcessed > 0
        ? static_cast<double>(total) / result.framesProcessed / 1e6 : 0.0;
    result.maximumMilliseconds = maximumNanoseconds_.load() / 1e6;
    return result;
}
