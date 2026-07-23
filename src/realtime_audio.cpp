#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
std::atomic<bool> running{true};

void stop(int) { running = false; }

struct Options {
    std::string captureDevice = "plughw:CARD=USB,DEV=0";
    std::string playbackDevice = "plughw:CARD=USB,DEV=0";
    unsigned int rate = 48000;
    unsigned int channels = 2;
    snd_pcm_uframes_t periodFrames = 512;
    snd_pcm_uframes_t bufferFrames = 2048;
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
        << "  --gain-db DB           Gain in decibels (overrides --gain)\n"
        << "  --lowpass HZ           Low-pass cutoff; 0 disables (default 0)\n"
        << "  --highpass HZ          High-pass cutoff; 0 disables (default 0)\n"
        << "  --list-devices         Print ALSA PCM device names\n"
        << "  --help                 Show this help\n";
}

void controlHelp() {
    std::cout
        << "\nReal-time controls (type a command and press Enter):\n"
        << "  gain VALUE             Set linear gain, for example: gain 0.5\n"
        << "  gaindb DB              Set gain in decibels, for example: gaindb -20\n"
        << "  mute                    Set gain to zero (useful for checking direct monitoring)\n"
        << "  lowpass HZ             Set cutoff; 0 disables the low-pass filter\n"
        << "  highpass HZ            Set cutoff; 0 disables the high-pass filter\n"
        << "  status                  Show the current parameter values\n"
        << "  help                    Show these commands\n"
        << "  quit                    Stop audio and exit\n\n";
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
        else if(arg == "--gain-db") options.gain = std::pow(10.0f, number<float>(value, arg) / 20.0f);
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

class Biquad {
public:
    enum class Type { LowPass, HighPass };

    explicit Biquad(unsigned int channels) : z1_(channels), z2_(channels) {}

    void configure(Type type, float cutoffHz, float sampleRate, float q) {
        constexpr float pi = 3.14159265358979323846f;
        const float omega = 2.0f * pi * cutoffHz / sampleRate;
        const float cosine = std::cos(omega);
        const float sine = std::sin(omega);
        const float alpha = sine / (2.0f * q);
        const float a0 = 1.0f + alpha;

        if(type == Type::LowPass) {
            b0_ = ((1.0f - cosine) * 0.5f) / a0;
            b1_ = (1.0f - cosine) / a0;
            b2_ = b0_;
        } else {
            b0_ = ((1.0f + cosine) * 0.5f) / a0;
            b1_ = -(1.0f + cosine) / a0;
            b2_ = b0_;
        }
        a1_ = (-2.0f * cosine) / a0;
        a2_ = (1.0f - alpha) / a0;
        reset();
    }

    float process(float input, size_t channel) {
        const float output = b0_ * input + z1_[channel];
        z1_[channel] = b1_ * input - a1_ * output + z2_[channel];
        z2_[channel] = b2_ * input - a2_ * output;
        return output;
    }

private:
    void reset() {
        std::fill(z1_.begin(), z1_.end(), 0.0f);
        std::fill(z2_.begin(), z2_.end(), 0.0f);
    }

    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;
    std::vector<float> z1_, z2_;
};

class Processor {
public:
    explicit Processor(const Options& o)
        : rate_(o.rate), channels_(o.channels), gain_(o.gain),
          lowPassHz_(o.lowPassHz), highPassHz_(o.highPassHz),
          lowPass_{Biquad(o.channels), Biquad(o.channels)},
          highPass_{Biquad(o.channels), Biquad(o.channels)} {
        updateFilters();
    }

    void process(std::vector<int16_t>& samples, snd_pcm_sframes_t frames) {
        const size_t count = static_cast<size_t>(frames) * channels_;
        for(size_t i = 0; i < count; ++i) {
            const size_t channel = i % channels_;
            float value = samples[i] / 32768.0f;
            if(highPassHz_ > 0) {
                for(auto& section : highPass_) value = section.process(value, channel);
            }
            if(lowPassHz_ > 0) {
                for(auto& section : lowPass_) value = section.process(value, channel);
            }
            value *= gain_;
            value = std::clamp(value, -1.0f, 0.999969f);
            samples[i] = static_cast<int16_t>(std::lrint(value * 32768.0f));
        }
    }

    void setGain(float value) { gain_ = value; }
    void setGainDb(float value) { gain_ = std::pow(10.0f, value / 20.0f); }

    void setLowPass(float value) {
        lowPassHz_ = value;
        updateFilters();
    }

    void setHighPass(float value) {
        highPassHz_ = value;
        updateFilters();
    }

    void printStatus() const {
        std::cout << "Gain: " << gain_;
        if(gain_ > 0.0f) std::cout << " (" << 20.0f * std::log10(gain_) << " dB)";
        else std::cout << " (muted)";
        std::cout << " | High-pass: "
                  << (highPassHz_ > 0 ? std::to_string(highPassHz_) + " Hz" : "off")
                  << " | Low-pass: "
                  << (lowPassHz_ > 0 ? std::to_string(lowPassHz_) + " Hz" : "off")
                  << '\n';
    }

private:
    void updateFilters() {
        // Q values for the two sections of a fourth-order Butterworth filter.
        constexpr std::array<float, 2> butterworthQ{0.5411961f, 1.3065630f};
        if(lowPassHz_ > 0) {
            for(size_t i = 0; i < lowPass_.size(); ++i)
                lowPass_[i].configure(Biquad::Type::LowPass, lowPassHz_,
                                      static_cast<float>(rate_), butterworthQ[i]);
        }
        if(highPassHz_ > 0) {
            for(size_t i = 0; i < highPass_.size(); ++i)
                highPass_[i].configure(Biquad::Type::HighPass, highPassHz_,
                                       static_cast<float>(rate_), butterworthQ[i]);
        }
    }

    unsigned int rate_;
    unsigned int channels_;
    float gain_;
    float lowPassHz_;
    float highPassHz_;
    std::array<Biquad, 2> lowPass_;
    std::array<Biquad, 2> highPass_;
};

void handleControlInput(Processor& processor, unsigned int sampleRate) {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if(poll(&input, 1, 0) <= 0 || !(input.revents & POLLIN)) return;

    std::string line;
    if(!std::getline(std::cin, line)) return;
    std::istringstream commandLine(line);
    std::string command;
    commandLine >> command;
    if(command.empty()) return;

    if(command == "quit" || command == "q") {
        running = false;
        return;
    }
    if(command == "help") {
        controlHelp();
        return;
    }
    if(command == "status") {
        processor.printStatus();
        return;
    }
    if(command == "mute") {
        processor.setGain(0.0f);
        processor.printStatus();
        return;
    }
    if(command != "gain" && command != "gaindb" &&
       command != "lowpass" && command != "highpass") {
        std::cerr << "Unknown command. Type help for available controls.\n";
        return;
    }

    float value = 0.0f;
    std::string extra;
    if(!(commandLine >> value) || (commandLine >> extra) || !std::isfinite(value)) {
        std::cerr << "Expected: " << command << " VALUE\n";
        return;
    }

    if(command == "gain") {
        if(value < 0.0f || value > 10.0f) {
            std::cerr << "Gain must be between 0 and 10.\n";
            return;
        }
        processor.setGain(value);
    } else if(command == "gaindb") {
        if(value < -120.0f || value > 20.0f) {
            std::cerr << "Gain must be between -120 and +20 dB.\n";
            return;
        }
        processor.setGainDb(value);
    } else if(command == "lowpass") {
        if(value < 0.0f || value >= sampleRate * 0.5f) {
            std::cerr << "Low-pass cutoff must be 0 (off) or below the Nyquist frequency.\n";
            return;
        }
        processor.setLowPass(value);
    } else if(command == "highpass") {
        if(value < 0.0f || value >= sampleRate * 0.5f) {
            std::cerr << "High-pass cutoff must be 0 (off) or below the Nyquist frequency.\n";
            return;
        }
        processor.setHighPass(value);
    }
    processor.printStatus();
}

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
        controlHelp();
        processor.printStatus();
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
            handleControlInput(processor, options.rate);
        }
        snd_pcm_drop(capture.get());
        snd_pcm_drain(playback.get());
    } catch(const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
