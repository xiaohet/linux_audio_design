#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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

enum class Routing {
    Stereo,
    Input1ToStereo,
    Input2ToStereo,
    MixToStereo
};

const char* routingName(Routing routing) {
    switch(routing) {
        case Routing::Stereo: return "stereo";
        case Routing::Input1ToStereo: return "input1";
        case Routing::Input2ToStereo: return "input2";
        case Routing::MixToStereo: return "mix";
    }
    return "unknown";
}

Routing parseRouting(const std::string& value) {
    if(value == "stereo") return Routing::Stereo;
    if(value == "input1") return Routing::Input1ToStereo;
    if(value == "input2") return Routing::Input2ToStereo;
    if(value == "mix") return Routing::MixToStereo;
    throw std::runtime_error(
        "Invalid routing mode; use stereo, input1, input2, or mix");
}

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
    bool diagnostics = false;
    Routing routing = Routing::Input2ToStereo;
};

void usage(const char* name) {
    std::cout
        << "Usage: " << name << " [options]\n"
        << "  --capture DEVICE       ALSA capture PCM (default plughw:CARD=Device)\n"
        << "  --playback DEVICE      ALSA playback PCM (default plughw:CARD=Device)\n"
        << "  --rate HZ              Sample rate (default 48000)\n"
        << "  --channels N           Channel count (default 2)\n"
        << "  --period FRAMES        Frames per processing period (default 512)\n"
        << "  --buffer FRAMES        ALSA buffer frames (default 2048)\n"
        << "  --gain VALUE           Linear gain (default 0.8)\n"
        << "  --gain-db DB           Gain in decibels (overrides --gain)\n"
        << "  --lowpass HZ           Low-pass cutoff; 0 disables (default 0)\n"
        << "  --highpass HZ          High-pass cutoff; 0 disables (default 0)\n"
        << "  --routing MODE         stereo, input1, input2, or mix (default input2)\n"
        << "  --diagnostics          Print negotiated ALSA settings and rate-limited xrun details\n"
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
        << "  route MODE             stereo, input1, input2, or mix\n"
        << "  status                  Show the current parameter values\n"
        << "  stats                   Show ALSA buffer state and recovery counters\n"
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
        if(arg == "--diagnostics") { options.diagnostics = true; continue; }
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
        else if(arg == "--routing") options.routing = parseRouting(value);
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

        check(snd_pcm_hw_params_get_period_size(params, &periodFrames_, &direction),
              "read negotiated period size");
        check(snd_pcm_hw_params_get_buffer_size(params, &bufferFrames_),
              "read negotiated buffer size");

        snd_pcm_sw_params_t* software;
        snd_pcm_sw_params_alloca(&software);
        check(snd_pcm_sw_params_current(handle_, software), "initialize software parameters");
        check(snd_pcm_sw_params_set_avail_min(handle_, software, periodFrames_),
              "set minimum available frames");
        if(stream == SND_PCM_STREAM_PLAYBACK) {
            // Do not start with only one period queued. Prefilling gives the
            // capture/process loop real scheduling margin and is reapplied by
            // snd_pcm_prepare() after an xrun.
            const snd_pcm_uframes_t threshold =
                bufferFrames_ > periodFrames_ ? bufferFrames_ - periodFrames_ : periodFrames_;
            check(snd_pcm_sw_params_set_start_threshold(handle_, software, threshold),
                  "set playback start threshold");
        }
        check(snd_pcm_sw_params(handle_, software), "apply software parameters");
        check(snd_pcm_prepare(handle_), "prepare PCM");

        if(options.diagnostics) {
            std::cerr << (stream == SND_PCM_STREAM_CAPTURE ? "Capture" : "Playback")
                      << " ALSA settings: rate=" << rate
                      << ", channels=" << options.channels
                      << ", period=" << periodFrames_
                      << ", buffer=" << bufferFrames_ << '\n';
        }
    }

    ~Pcm() { if(handle_) snd_pcm_close(handle_); }
    snd_pcm_t* get() const { return handle_; }
    snd_pcm_uframes_t periodFrames() const { return periodFrames_; }
    snd_pcm_uframes_t bufferFrames() const { return bufferFrames_; }

private:
    void check(int error, const char* operation) {
        if(error < 0) throw std::runtime_error(std::string(operation) + ": " + snd_strerror(error));
    }
    snd_pcm_t* handle_ = nullptr;
    snd_pcm_uframes_t periodFrames_ = 0;
    snd_pcm_uframes_t bufferFrames_ = 0;
};

struct RecoveryStats {
    unsigned long capture = 0;
    unsigned long playback = 0;
    std::chrono::steady_clock::time_point lastDiagnostic{};
};

void printPcmStatus(const char* name, const Pcm& pcm) {
    snd_pcm_status_t* status;
    snd_pcm_status_alloca(&status);
    const int result = snd_pcm_status(pcm.get(), status);
    if(result < 0) {
        std::cerr << name << " status unavailable: " << snd_strerror(result) << '\n';
        return;
    }
    std::cerr << name
              << " state=" << snd_pcm_state_name(snd_pcm_status_get_state(status))
              << ", available=" << snd_pcm_status_get_avail(status)
              << ", delay=" << snd_pcm_status_get_delay(status)
              << ", period=" << pcm.periodFrames()
              << ", buffer=" << pcm.bufferFrames() << '\n';
}

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
        if(!std::isfinite(output) || !std::isfinite(z1_[channel]) ||
           !std::isfinite(z2_[channel])) {
            z1_[channel] = 0.0f;
            z2_[channel] = 0.0f;
            return 0.0f;
        }
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
          lowPassHz_(o.lowPassHz), highPassHz_(o.highPassHz), routing_(o.routing),
          lowPass_{Biquad(o.channels), Biquad(o.channels)},
          highPass_{Biquad(o.channels), Biquad(o.channels)} {
        updateFilters();
    }

    void process(std::vector<int16_t>& samples, snd_pcm_sframes_t frames) {
        applyRouting(samples, frames);
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
    void setRouting(Routing value) { routing_ = value; }

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
                  << " | Routing: " << routingName(routing_)
                  << '\n';
    }

private:
    void applyRouting(std::vector<int16_t>& samples, snd_pcm_sframes_t frames) const {
        if(channels_ != 2 || routing_ == Routing::Stereo) return;

        for(snd_pcm_sframes_t frame = 0; frame < frames; ++frame) {
            const size_t offset = static_cast<size_t>(frame) * 2;
            int16_t mono = 0;
            if(routing_ == Routing::Input1ToStereo) {
                mono = samples[offset];
            } else if(routing_ == Routing::Input2ToStereo) {
                mono = samples[offset + 1];
            } else {
                const int mixed = static_cast<int>(samples[offset]) +
                                  static_cast<int>(samples[offset + 1]);
                mono = static_cast<int16_t>(mixed / 2);
            }
            samples[offset] = mono;
            samples[offset + 1] = mono;
        }
    }

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
    Routing routing_;
    std::array<Biquad, 2> lowPass_;
    std::array<Biquad, 2> highPass_;
};

void handleControlInput(Processor& processor, unsigned int sampleRate,
                        const Pcm& capture, const Pcm& playback,
                        const RecoveryStats& recoveries) {
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
    if(command == "stats") {
        std::cerr << "Recoveries: capture=" << recoveries.capture
                  << ", playback=" << recoveries.playback << '\n';
        printPcmStatus("Capture", capture);
        printPcmStatus("Playback", playback);
        return;
    }
    if(command == "route") {
        std::string value;
        std::string extra;
        if(!(commandLine >> value) || (commandLine >> extra)) {
            std::cerr << "Expected: route stereo|input1|input2|mix\n";
            return;
        }
        try {
            processor.setRouting(parseRouting(value));
            processor.printStatus();
        } catch(const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
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

void recover(Pcm& pcm, snd_pcm_sframes_t error, const char* direction,
             unsigned long& counter, RecoveryStats& stats, bool diagnostics) {
    if(error == -EPIPE || error == -ESTRPIPE || error == -EINTR) {
        ++counter;
        const auto now = std::chrono::steady_clock::now();
        if(diagnostics &&
           (stats.lastDiagnostic.time_since_epoch().count() == 0 ||
            now - stats.lastDiagnostic >= std::chrono::seconds(2))) {
            std::cerr << direction << " I/O error: " << snd_strerror(static_cast<int>(error))
                      << " (recovery #" << counter << ")\n";
            printPcmStatus(direction, pcm);
            stats.lastDiagnostic = now;
        }

        const int result = snd_pcm_recover(pcm.get(), static_cast<int>(error), 1);
        if(result < 0) throw std::runtime_error(std::string(direction) + " recovery failed: " + snd_strerror(result));
        return;
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
        RecoveryStats recoveries;

        std::cout << "Streaming " << options.rate << " Hz, " << options.channels
                  << " channels. Press Ctrl+C to stop.\n";
        controlHelp();
        processor.printStatus();
        while(running) {
            snd_pcm_sframes_t frames = snd_pcm_readi(capture.get(), samples.data(), options.periodFrames);
            if(frames < 0) {
                if(!running && frames == -EINTR) break;
                recover(capture, frames, "Capture", recoveries.capture,
                        recoveries, options.diagnostics);
                continue;
            }
            processor.process(samples, frames);
            snd_pcm_sframes_t written = 0;
            while(written < frames && running) {
                const auto result = snd_pcm_writei(playback.get(), samples.data() + written * options.channels, frames - written);
                if(result < 0) {
                    if(!running && result == -EINTR) break;
                    recover(playback, result, "Playback", recoveries.playback,
                            recoveries, options.diagnostics);
                    break;
                }
                if(result == 0) {
                    throw std::runtime_error("Playback made no progress");
                }
                written += result;
            }
            handleControlInput(processor, options.rate, capture, playback, recoveries);
        }
        snd_pcm_drop(capture.get());
        snd_pcm_drain(playback.get());
    } catch(const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
