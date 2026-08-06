#include "deepfilter_processor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <filesystem>
#include <cstdlib>
#include <stdexcept>

namespace {
constexpr size_t QueueCapacity = 16384;
constexpr size_t SchedulingFrames = 2;

std::filesystem::path resolve_deepfilter_path(const std::string& configured) {
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

    bool push(float value) {
        const size_t write = write_.load(std::memory_order_relaxed);
        if(write - read_.load(std::memory_order_acquire) >= data_.size())
            return false;
        data_[write % data_.size()] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool pop(float& value) {
        const size_t read = read_.load(std::memory_order_relaxed);
        if(read == write_.load(std::memory_order_acquire)) return false;
        value = data_[read % data_.size()];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

private:
    std::vector<float> data_;
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
        // DeepFilterNet v0.5.6 exposes the two-argument C API.
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

        const size_t schedulingDelay = SchedulingFrames * frameLength_;
        for(size_t i = 0; i < schedulingDelay; ++i) outputRing_->push(0.0f);
        dryDelaySamples_ = options.deepFilterDelaySamples + schedulingDelay;
        dryDelay_.assign(std::max<size_t>(dryDelaySamples_, 1), 0.0f);

        available_ = true;
        worker_ = std::thread(&DeepFilterProcessor::worker_loop, this);
    } catch(...) {
        unload();
        throw;
    }
}

void DeepFilterProcessor::unload() {
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
    targetStrength_ = std::clamp(value, 0.0f, 1.0f);
}

void DeepFilterProcessor::process(std::vector<int16_t>& samples, size_t frames,
                                  unsigned int channels, Routing routing) {
    if(!available() || channels == 0) return;
    const float smoothing = 1.0f - std::exp(-1.0f / (0.040f * sampleRate_));

    for(size_t frame = 0; frame < frames; ++frame) {
        const size_t offset = frame * channels;
        float mono = samples[offset] / 32768.0f;
        if(channels > 1 && routing == Routing::Stereo)
            mono = 0.5f * (mono + samples[offset + 1] / 32768.0f);

        if(!inputRing_->push(mono)) ++inputOverruns_;

        const float delayedDry = dryDelay_[dryDelayPosition_];
        dryDelay_[dryDelayPosition_] = mono;
        dryDelayPosition_ = (dryDelayPosition_ + 1) % dryDelay_.size();

        float enhanced = delayedDry;
        if(!outputRing_->pop(enhanced)) ++outputUnderruns_;

        const float target = targetStrength_.load(std::memory_order_relaxed);
        smoothedStrength_ += smoothing * (target - smoothedStrength_);
        float mixed;
        if(target <= 0.0f && smoothedStrength_ < 0.0001f) {
            smoothedStrength_ = 0.0f;
            mixed = mono;
        } else {
            mixed = delayedDry * (1.0f - smoothedStrength_) +
                    enhanced * smoothedStrength_;
        }
        mixed = std::clamp(mixed, -1.0f, 0.999969f);
        const int16_t value = static_cast<int16_t>(std::lrint(mixed * 32768.0f));
        for(size_t channel = 0; channel < channels; ++channel)
            samples[offset + channel] = value;
    }
    workerWake_.notify_one();
}

void DeepFilterProcessor::worker_loop() {
    const uint64_t deadlineNanoseconds =
        static_cast<uint64_t>(frameLength_) * 1000000000ull / sampleRate_;
    while(!stop_.load()) {
        if(inputRing_->size() < frameLength_ ||
           outputRing_->free_space() < frameLength_) {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerWake_.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }

        for(float& sample : workerInput_) inputRing_->pop(sample);
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
        for(float sample : workerOutput_) {
            if(!outputRing_->push(sample)) break;
        }
    }
}

DeepFilterProcessor::Snapshot DeepFilterProcessor::snapshot() const {
    Snapshot result;
    result.available = available();
    result.strength = targetStrength_.load();
    result.frameLength = frameLength_;
    result.framesProcessed = framesProcessed_.load();
    result.deadlineMisses = deadlineMisses_.load();
    result.inputOverruns = inputOverruns_.load();
    result.outputUnderruns = outputUnderruns_.load();
    const uint64_t total = totalNanoseconds_.load();
    result.meanMilliseconds = result.framesProcessed > 0
        ? static_cast<double>(total) / result.framesProcessed / 1e6 : 0.0;
    result.maximumMilliseconds = maximumNanoseconds_.load() / 1e6;
    return result;
}
