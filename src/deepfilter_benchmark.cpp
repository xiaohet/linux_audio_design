#include "read_wav_file.h"
#include "write_wav_file.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {
constexpr int SampleRate = 48000;

struct Options {
    std::string library;
    std::string model;
    std::string input;
    std::string output;
    std::string csv;
    double durationSeconds = 60.0;
    double warmupSeconds = 5.0;
    float attenuationLimitDb = 100.0f;
};

void usage(const char* name) {
    std::cout
        << "Usage: " << name << " --model MODEL.tar.gz [options]\n\n"
        << "  --model PATH          DeepFilterNet ONNX model archive (required)\n"
        << "  --library PATH        libdf.so or libdeepfilter.so; auto-detected otherwise\n"
        << "  --input WAV           48 kHz, 16-bit PCM test audio; stereo is downmixed\n"
        << "  --output WAV          Write measured enhanced audio as mono WAV\n"
        << "  --csv PATH            Write one timing row per measured model frame\n"
        << "  --duration SECONDS    Measured audio duration (default 60)\n"
        << "  --warmup SECONDS      Untimed model warm-up (default 5)\n"
        << "  --atten-limit DB      Maximum suppression attenuation (default 100)\n"
        << "  --help                Show this help\n\n"
        << "Without --input, a deterministic tone-and-noise signal is generated.\n";
}

double parse_number(const char* value, const std::string& option) {
    try {
        size_t used = 0;
        const double result = std::stod(value, &used);
        if(used != std::string(value).size() || !std::isfinite(result))
            throw std::invalid_argument("number");
        return result;
    } catch(...) {
        throw std::runtime_error("Invalid value for " + option);
    }
}

Options parse_options(int argc, char** argv) {
    Options options;
    for(int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if(argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if(i + 1 >= argc)
            throw std::runtime_error("Missing value for " + argument);
        const char* value = argv[++i];
        if(argument == "--model") options.model = value;
        else if(argument == "--library") options.library = value;
        else if(argument == "--input") options.input = value;
        else if(argument == "--output") options.output = value;
        else if(argument == "--csv") options.csv = value;
        else if(argument == "--duration")
            options.durationSeconds = parse_number(value, argument);
        else if(argument == "--warmup")
            options.warmupSeconds = parse_number(value, argument);
        else if(argument == "--atten-limit")
            options.attenuationLimitDb = static_cast<float>(parse_number(value, argument));
        else throw std::runtime_error("Unknown option: " + argument);
    }
    if(options.model.empty()) throw std::runtime_error("--model is required");
    if(options.durationSeconds <= 0.0)
        throw std::runtime_error("--duration must be greater than zero");
    if(options.warmupSeconds < 0.0)
        throw std::runtime_error("--warmup cannot be negative");
    if(options.attenuationLimitDb < 0.0f || options.attenuationLimitDb > 100.0f)
        throw std::runtime_error("--atten-limit must be between 0 and 100 dB");
    return options;
}

class DeepFilterLibrary {
public:
    using Create = void* (*)(const char*, float, const char*);
    using FrameLength = size_t (*)(void*);
    using ProcessFrame = float (*)(void*, float*, float*);
    using Free = void (*)(void*);

    explicit DeepFilterLibrary(const std::string& requested) {
        if(!requested.empty()) {
            open(requested);
        } else {
            for(const char* candidate : {"libdeepfilter.so", "libdf.so"}) {
                handle_ = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
                if(handle_) {
                    path_ = candidate;
                    break;
                }
            }
            if(!handle_)
                throw std::runtime_error(
                    "Cannot load libdeepfilter.so or libdf.so; use --library PATH");
        }
        create = symbol<Create>("df_create");
        frameLength = symbol<FrameLength>("df_get_frame_length");
        processFrame = symbol<ProcessFrame>("df_process_frame");
        freeState = symbol<Free>("df_free");
    }

    ~DeepFilterLibrary() {
        if(handle_) dlclose(handle_);
    }

    DeepFilterLibrary(const DeepFilterLibrary&) = delete;
    DeepFilterLibrary& operator=(const DeepFilterLibrary&) = delete;

    const std::string& path() const { return path_; }
    Create create = nullptr;
    FrameLength frameLength = nullptr;
    ProcessFrame processFrame = nullptr;
    Free freeState = nullptr;

private:
    void open(const std::string& path) {
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if(!handle_) {
            const char* error = dlerror();
            throw std::runtime_error("Cannot load " + path + ": " +
                                     (error ? error : "unknown error"));
        }
        path_ = path;
    }

    template<typename T>
    T symbol(const char* name) {
        dlerror();
        void* address = dlsym(handle_, name);
        const char* error = dlerror();
        if(error) throw std::runtime_error(std::string("Missing symbol ") + name +
                                           ": " + error);
        return reinterpret_cast<T>(address);
    }

    void* handle_ = nullptr;
    std::string path_;
};

class DeepFilterState {
public:
    DeepFilterState(DeepFilterLibrary& library, const Options& options)
        : library_(library) {
        state_ = library_.create(options.model.c_str(), options.attenuationLimitDb,
                                 nullptr);
        if(!state_) throw std::runtime_error("DeepFilterNet model creation failed");
    }
    ~DeepFilterState() {
        if(state_) library_.freeState(state_);
    }
    void* get() const { return state_; }
private:
    DeepFilterLibrary& library_;
    void* state_ = nullptr;
};

std::vector<float> load_input(const Options& options) {
    if(options.input.empty()) {
        const size_t count = static_cast<size_t>(SampleRate * 10);
        std::vector<float> samples(count);
        uint32_t random = 0x13579bdu;
        constexpr double pi = 3.14159265358979323846;
        for(size_t i = 0; i < count; ++i) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            const float noise = (static_cast<float>(random) /
                                 static_cast<float>(UINT32_MAX) * 2.0f - 1.0f);
            const double time = static_cast<double>(i) / SampleRate;
            samples[i] = 0.18f * static_cast<float>(std::sin(2.0 * pi * 220.0 * time)) +
                         0.08f * static_cast<float>(std::sin(2.0 * pi * 440.0 * time)) +
                         0.06f * noise;
        }
        return samples;
    }

    const WavData wav = read_wav_file(options.input);
    if(wav.sampleRate != SampleRate)
        throw std::runtime_error("Input WAV must use a 48000 Hz sample rate");
    if(wav.numChannels < 1)
        throw std::runtime_error("Input WAV has no audio channels");
    const size_t frames = wav.samples.size() / static_cast<size_t>(wav.numChannels);
    std::vector<float> mono(frames);
    for(size_t frame = 0; frame < frames; ++frame) {
        float sum = 0.0f;
        for(int channel = 0; channel < wav.numChannels; ++channel)
            sum += wav.samples[frame * wav.numChannels + channel];
        mono[frame] = sum / static_cast<float>(wav.numChannels);
    }
    if(mono.empty()) throw std::runtime_error("Input WAV contains no samples");
    return mono;
}

float source_sample(const std::vector<float>& source, size_t index) {
    return source[index % source.size()];
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if(sorted.empty()) return 0.0;
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(position);
    const size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double blend = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - blend) + sorted[upper] * blend;
}

double temperature_celsius() {
    std::ifstream input("/sys/class/thermal/thermal_zone0/temp");
    double millidegrees = 0.0;
    return input >> millidegrees ? millidegrees / 1000.0 : -1.0;
}

double process_cpu_seconds(const rusage& usage) {
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

void write_csv(const std::string& path, const std::vector<double>& milliseconds,
               double deadlineMs) {
    if(path.empty()) return;
    std::ofstream output(path);
    if(!output) throw std::runtime_error("Cannot open CSV output: " + path);
    output << "frame,processing_ms,deadline_ms,deadline_missed\n";
    output << std::fixed << std::setprecision(6);
    for(size_t i = 0; i < milliseconds.size(); ++i)
        output << i << ',' << milliseconds[i] << ',' << deadlineMs << ','
               << (milliseconds[i] > deadlineMs ? 1 : 0) << '\n';
}

void print_metric(const char* label, double value, const char* unit) {
    std::cout << std::left << std::setw(27) << label << std::right
              << std::fixed << std::setprecision(3) << std::setw(10) << value
              << ' ' << unit << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const std::vector<float> source = load_input(options);
        DeepFilterLibrary library(options.library);

        const double temperatureBefore = temperature_celsius();
        const auto initializationStart = std::chrono::steady_clock::now();
        DeepFilterState state(library, options);
        const double initializationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - initializationStart).count();

        const size_t frameLength = library.frameLength(state.get());
        if(frameLength == 0) throw std::runtime_error("Model returned a zero frame length");
        const double deadlineMs = frameLength * 1000.0 / SampleRate;
        const size_t warmupFrames = static_cast<size_t>(
            std::ceil(options.warmupSeconds * SampleRate / frameLength));
        const size_t measuredFrames = static_cast<size_t>(
            std::ceil(options.durationSeconds * SampleRate / frameLength));
        std::vector<float> input(frameLength);
        std::vector<float> output(frameLength);
        size_t warmupIndex = 0;

        for(size_t frame = 0; frame < warmupFrames; ++frame) {
            for(size_t i = 0; i < frameLength; ++i)
                input[i] = source_sample(source, warmupIndex++);
            library.processFrame(state.get(), input.data(), output.data());
        }

        size_t sourceIndex = 0;
        std::vector<double> frameMilliseconds;
        frameMilliseconds.reserve(measuredFrames);
        std::vector<float> enhanced;
        if(!options.output.empty()) enhanced.reserve(measuredFrames * frameLength);
        rusage usageBefore{};
        rusage usageAfter{};
        getrusage(RUSAGE_SELF, &usageBefore);
        const auto benchmarkStart = std::chrono::steady_clock::now();
        for(size_t frame = 0; frame < measuredFrames; ++frame) {
            for(size_t i = 0; i < frameLength; ++i)
                input[i] = source_sample(source, sourceIndex++);
            const auto frameStart = std::chrono::steady_clock::now();
            library.processFrame(state.get(), input.data(), output.data());
            frameMilliseconds.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - frameStart).count());
            if(!options.output.empty())
                enhanced.insert(enhanced.end(), output.begin(), output.end());
        }
        const double wallSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - benchmarkStart).count();
        getrusage(RUSAGE_SELF, &usageAfter);
        const double temperatureAfter = temperature_celsius();

        std::vector<double> sorted = frameMilliseconds;
        std::sort(sorted.begin(), sorted.end());
        const double audioSeconds = measuredFrames * frameLength /
                                    static_cast<double>(SampleRate);
        const double sumMs = std::accumulate(frameMilliseconds.begin(),
                                             frameMilliseconds.end(), 0.0);
        const size_t misses = static_cast<size_t>(std::count_if(
            frameMilliseconds.begin(), frameMilliseconds.end(),
            [deadlineMs](double value) { return value > deadlineMs; }));
        const double cpuSeconds = process_cpu_seconds(usageAfter) -
                                  process_cpu_seconds(usageBefore);

        std::cout << "\nDeepFilterNet standalone benchmark\n"
                  << "----------------------------------\n"
                  << "Library:                    " << library.path() << '\n'
                  << "Model:                      " << options.model << '\n'
                  << "Input:                      "
                  << (options.input.empty() ? "synthetic tone + noise" : options.input) << '\n'
                  << "Sample rate:                " << SampleRate << " Hz\n"
                  << "Frame length:               " << frameLength << " samples\n"
                  << "Frames measured:            " << measuredFrames << '\n';
        print_metric("Model initialization", initializationMs, "ms");
        print_metric("Audio duration", audioSeconds, "s");
        print_metric("Wall time", wallSeconds, "s");
        print_metric("Real-time factor", wallSeconds / audioSeconds, "RTF");
        print_metric("Mean frame time", sumMs / measuredFrames, "ms");
        print_metric("Median frame time", percentile(sorted, 0.50), "ms");
        print_metric("95th percentile", percentile(sorted, 0.95), "ms");
        print_metric("99th percentile", percentile(sorted, 0.99), "ms");
        print_metric("Maximum frame time", sorted.back(), "ms");
        print_metric("Frame deadline", deadlineMs, "ms");
        std::cout << std::left << std::setw(27) << "Deadline misses" << std::right
                  << std::setw(10) << misses << " frames ("
                  << std::fixed << std::setprecision(4)
                  << 100.0 * misses / measuredFrames << "%)\n";
        print_metric("Process CPU / wall", cpuSeconds / wallSeconds * 100.0, "% one core");
        print_metric("Peak resident memory", usageAfter.ru_maxrss / 1024.0, "MiB");
        if(temperatureBefore >= 0.0) print_metric("Temperature before", temperatureBefore, "C");
        if(temperatureAfter >= 0.0) print_metric("Temperature after", temperatureAfter, "C");

        const double p99 = percentile(sorted, 0.99);
        const bool pass = wallSeconds / audioSeconds < 0.60 &&
                          p99 < deadlineMs * 0.80 && misses == 0;
        std::cout << "Conservative real-time result: "
                  << (pass ? "PASS" : "NEEDS REVIEW") << '\n';

        write_csv(options.csv, frameMilliseconds, deadlineMs);
        if(!options.output.empty()) {
            WavData wav{SampleRate, 1, 16, std::move(enhanced)};
            write_wav_file(wav, options.output);
        }
        return pass ? 0 : 2;
    } catch(const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
