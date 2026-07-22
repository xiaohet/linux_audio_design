#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic<bool> running{true};

void stop(int) { running = false; }

struct Options {
    std::string captureDevice = "plughw:CARD=Device";
    std::string playbackDevice = "plughw:CARD=Device";
    unsigned int rate = 48000;
    unsigned int channels = 2;
    snd_pcm_uframes_t periodFrames = 128;
    snd_pcm_uframes_t bufferFrames = 512;
    float gain = 0.8f;
    float lowPassHz = 0.0f;
    float highPassHz = 0.0f;
};

void usage(const char* name) {
    std::cout
        << "Usage: " << name << " [options]\n"
        << "  --capture DEVICE       ALSA capture PCM (default plughw:CARD=Device)\n"
        << "  --playback DEVICE      ALSA playback PCM (default plughw:CARD=Device)\n"
        << "  --rate HZ              Sample rate (default 48000)\n"
        << "  --channels N           Channel count (default 2)\n"
        << "  --period FRAMES        Frames per processing period (default 128)\n"
        << "  --buffer FRAMES        ALSA buffer frames (default 512)\n"
        << "  --gain VALUE           Linear gain (default 0.8)\n"
        << "  --lowpass HZ           Low-pass cutoff; 0 disables (default 0)\n"
        << "  --highpass HZ          High-pass cutoff; 0 disables (default 0)\n"
        << "  --list-devices         Print ALSA PCM device names\n"
        << "  --help                 Show this help\n";
}

void listDevices() {
    void** hints = nullptr;
    const int result = snd_device_name_hint(-1, "pcm", &hints);
    if(result < 0) throw std::runtime_error(snd_strerror(result));
    for(void** hint = hints; *hint != nullptr; ++hint) {
        char* name = snd_device_name_get_hint(*hint, "NAME");
        char* description = snd_device_name_get_hint(*hint, "DESC");
        if(name) std::cout << name << (description ? "\t" : "\n");
        if(description) {
            for(char* p = description; *p; ++p) if(*p == '\n') *p = ' ';
            std::cout << description << '\n';
        }
        std::free(name);
        std::free(description);
    }
    snd_device_name_free_hint(hints);
}

template<typename T>
T number(const char* text, const std::string& option);

template<>
unsigned int number<unsigned int>(const char* text, const std::string& option) {
    try {
        size_t used = 0;
        const auto value = std::stoul(text, &used);
        if(used != std::strlen(text) || value == 0) throw std::invalid_argument("range");
        return static_cast<unsigned int>(value);
    } catch(...) { throw std::runtime_error("Invalid value for " + option); }
}

template<>
float number<float>(const char* text, const std::string& option) {
    try {
        size_t used = 0;
        const float value = std::stof(text, &used);
        if(used != std::strlen(text) || !std::isfinite(value)) throw std::invalid_argument("range");
        return value;
    } catch(...) { throw std::runtime_error("Invalid value for " + option); }
}

Options parse(int argc, char** argv) {
    Options options;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if(arg == "--help") { usage(argv[0]); std::exit(0); }
        if(arg == "--list-devices") { listDevices(); std::exit(0); }
        if(i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
        const char* value = argv[++i];
        if(arg == "--capture") options.captureDevice = value;
        else if(arg == "--playback") options.playbackDevice = value;
        else if(arg == "--rate") options.rate = number<unsigned int>(value, arg);
        else if(arg == "--channels") options.channels = number<unsigned int>(value, arg);
        else if(arg == "--period") options.periodFrames = number<unsigned int>(value, arg);
        else if(arg == "--buffer") options.bufferFrames = number<unsigned int>(value, arg);
        else if(arg == "--gain") options.gain = number<float>(value, arg);
        else if(arg == "--lowpass") options.lowPassHz = number<float>(value, arg);
        else if(arg == "--highpass") options.highPassHz = number<float>(value, arg);
        else throw std::runtime_error("Unknown option: " + arg);
    }
    if(options.bufferFrames < options.periodFrames * 2)
        throw std::runtime_error("--buffer must be at least twice --period");
    if(options.lowPassHz < 0 || options.highPassHz < 0)
        throw std::runtime_error("Filter cutoffs cannot be negative");
    return options;
}

class Pcm {
public:
    Pcm(const std::string& device, snd_pcm_stream_t stream, const Options& options) {
        int error = snd_pcm_open(&handle_, device.c_str(), stream, 0);
        if(error < 0) throw std::runtime_error("Cannot open " + device + ": " + snd_strerror(error));

        snd_pcm_hw_params_t* params;
        snd_pcm_hw_params_alloca(&params);
        check(snd_pcm_hw_params_any(handle_, params), "initialize hardware parameters");
        check(snd_pcm_hw_params_set_access(handle_, params, SND_PCM_ACCESS_RW_INTERLEAVED), "set interleaved access");
        check(snd_pcm_hw_params_set_format(handle_, params, SND_PCM_FORMAT_S16_LE), "set S16_LE format");
        check(snd_pcm_hw_params_set_channels(handle_, params, options.channels), "set channel count");

        unsigned int rate = options.rate;
        int direction = 0;
        check(snd_pcm_hw_params_set_rate_near(handle_, params, &rate, &direction), "set sample rate");
        if(rate != options.rate) throw std::runtime_error(device + " does not support requested sample rate exactly");
        snd_pcm_uframes_t period = options.periodFrames;
        check(snd_pcm_hw_params_set_period_size_near(handle_, params, &period, &direction), "set period size");
        snd_pcm_uframes_t buffer = options.bufferFrames;
        check(snd_pcm_hw_params_set_buffer_size_near(handle_, params, &buffer), "set buffer size");
        check(snd_pcm_hw_params(handle_, params), "apply hardware parameters");
        check(snd_pcm_prepare(handle_), "prepare PCM");
    }

    ~Pcm() { if(handle_) snd_pcm_close(handle_); }
    snd_pcm_t* get() const { return handle_; }

private:
    void check(int error, const char* operation) {
        if(error < 0) throw std::runtime_error(std::string(operation) + ": " + snd_strerror(error));
    }
    snd_pcm_t* handle_ = nullptr;
};

class Processor {
public:
    explicit Processor(const Options& o)
        : options_(o), lp_(o.channels), hpInput_(o.channels), hpOutput_(o.channels) {
        constexpr float pi = 3.14159265358979323846f;
        const float dt = 1.0f / static_cast<float>(o.rate);
        if(o.lowPassHz > 0) {
            const float cutoff = std::min(o.lowPassHz, o.rate * 0.499f);
            const float rc = 1.0f / (2.0f * pi * cutoff);
            lpAlpha_ = dt / (rc + dt);
        }
        if(o.highPassHz > 0) {
            const float cutoff = std::min(o.highPassHz, o.rate * 0.499f);
            const float rc = 1.0f / (2.0f * pi * cutoff);
            hpAlpha_ = rc / (rc + dt);
        }
    }

    void process(std::vector<int16_t>& samples, snd_pcm_sframes_t frames) {
        const size_t count = static_cast<size_t>(frames) * options_.channels;
        for(size_t i = 0; i < count; ++i) {
            const size_t channel = i % options_.channels;
            float value = samples[i] / 32768.0f * options_.gain;
            if(options_.highPassHz > 0) {
                const float output = hpAlpha_ * (hpOutput_[channel] + value - hpInput_[channel]);
                hpInput_[channel] = value;
                hpOutput_[channel] = output;
                value = output;
            }
            if(options_.lowPassHz > 0) {
                lp_[channel] += lpAlpha_ * (value - lp_[channel]);
                value = lp_[channel];
            }
            value = std::clamp(value, -1.0f, 0.999969f);
            samples[i] = static_cast<int16_t>(std::lrint(value * 32768.0f));
        }
    }

private:
    Options options_;
    float lpAlpha_ = 0, hpAlpha_ = 0;
    std::vector<float> lp_, hpInput_, hpOutput_;
};

snd_pcm_sframes_t recover(snd_pcm_t* pcm, snd_pcm_sframes_t error, const char* direction) {
    if(error == -EPIPE || error == -ESTRPIPE || error == -EINTR) {
        const int result = snd_pcm_recover(pcm, static_cast<int>(error), 1);
        if(result < 0) throw std::runtime_error(std::string(direction) + " recovery failed: " + snd_strerror(result));
        std::cerr << direction << " stream recovered from an over/underrun\n";
        return 0;
    }
    throw std::runtime_error(std::string(direction) + " failed: " + snd_strerror(static_cast<int>(error)));
}
}

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);

        Pcm capture(options.captureDevice, SND_PCM_STREAM_CAPTURE, options);
        Pcm playback(options.playbackDevice, SND_PCM_STREAM_PLAYBACK, options);
        Processor processor(options);
        std::vector<int16_t> samples(options.periodFrames * options.channels);

        std::cout << "Streaming " << options.rate << " Hz, " << options.channels
                  << " channels. Press Ctrl+C to stop.\n";
        while(running) {
            snd_pcm_sframes_t frames = snd_pcm_readi(capture.get(), samples.data(), options.periodFrames);
            if(frames < 0) { recover(capture.get(), frames, "Capture"); continue; }
            processor.process(samples, frames);
            snd_pcm_sframes_t written = 0;
            while(written < frames && running) {
                const auto result = snd_pcm_writei(playback.get(), samples.data() + written * options.channels, frames - written);
                if(result < 0) { recover(playback.get(), result, "Playback"); break; }
                written += result;
            }
        }
        snd_pcm_drop(capture.get());
        snd_pcm_drain(playback.get());
    } catch(const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
