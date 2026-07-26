#include <alsa/asoundlib.h>
#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
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
    float noiseGateDb = -55.0f;
    bool diagnostics = false;
    Routing routing = Routing::Input2ToStereo;
    unsigned int webPort = 8080;
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
        << "  --gate-db DB           Noise-gate threshold (default -55; -120 disables)\n"
        << "  --routing MODE         stereo, input1, input2, or mix (default input2)\n"
        << "  --web-port PORT        Browser control port (default 8080; 0 disables)\n"
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
        << "  bypass on|off           Bypass gain and both filters\n"
        << "  lowpass HZ             Set cutoff; 0 disables the low-pass filter\n"
        << "  highpass HZ            Set cutoff; 0 disables the high-pass filter\n"
        << "  gate DB|off            Set or disable the input noise gate\n"
        << "  levels                  Show pre-DSP input and output peak levels\n"
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
        else if(arg == "--gate-db") options.noiseGateDb = number<float>(value, arg);
        else if(arg == "--routing") options.routing = parseRouting(value);
        else if(arg == "--web-port") {
            try {
                size_t used = 0;
                const auto port = std::stoul(value, &used);
                if(used != std::strlen(value) || port > 65535)
                    throw std::invalid_argument("range");
                options.webPort = static_cast<unsigned int>(port);
            } catch(...) {
                throw std::runtime_error("Invalid value for --web-port");
            }
        }
        else throw std::runtime_error("Unknown option: " + arg);
    }
    if(options.bufferFrames < options.periodFrames * 2)
        throw std::runtime_error("--buffer must be at least twice --period");
    if(options.lowPassHz < 0 || options.highPassHz < 0)
        throw std::runtime_error("Filter cutoffs cannot be negative");
    if(options.noiseGateDb < -120.0f || options.noiseGateDb > -10.0f)
        throw std::runtime_error("--gate-db must be between -120 and -10 dBFS");
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
    struct Snapshot {
        float gain;
        float lowPassHz;
        float highPassHz;
        float peak;
        float input1Peak;
        float input2Peak;
        float noiseGateDb;
        bool gateOpen;
        bool bypass;
        Routing routing;
    };

    explicit Processor(const Options& o)
        : rate_(o.rate), channels_(o.channels), gain_(o.gain),
          lowPassHz_(o.lowPassHz), highPassHz_(o.highPassHz),
          noiseGateDb_(o.noiseGateDb), routing_(o.routing),
          lowPass_{Biquad(o.channels), Biquad(o.channels)},
          highPass_{Biquad(o.channels), Biquad(o.channels)} {
        updateFilters();
    }

    void process(std::vector<int16_t>& samples, snd_pcm_sframes_t frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        float input1BlockPeak = 0.0f;
        float input2BlockPeak = 0.0f;
        for(snd_pcm_sframes_t frame = 0; frame < frames; ++frame) {
            const size_t offset = static_cast<size_t>(frame) * channels_;
            input1BlockPeak = std::max(
                input1BlockPeak, std::abs(samples[offset] / 32768.0f));
            if(channels_ > 1) {
                input2BlockPeak = std::max(
                    input2BlockPeak, std::abs(samples[offset + 1] / 32768.0f));
            }
        }
        input1Peak_ = std::max(input1BlockPeak, input1Peak_ * 0.92f);
        input2Peak_ = std::max(input2BlockPeak, input2Peak_ * 0.92f);

        applyRouting(samples, frames);
        const size_t count = static_cast<size_t>(frames) * channels_;
        float routedPeak = 0.0f;
        for(size_t i = 0; i < count; ++i)
            routedPeak = std::max(routedPeak, std::abs(samples[i] / 32768.0f));

        const bool gateEnabled = noiseGateDb_ > -119.9f;
        if(gateEnabled && !bypass_) {
            const float openThreshold = std::pow(10.0f, noiseGateDb_ / 20.0f);
            const float closeThreshold = openThreshold * 0.5f;
            if(gateOpen_) gateOpen_ = routedPeak >= closeThreshold;
            else gateOpen_ = routedPeak >= openThreshold;
        } else {
            gateOpen_ = true;
            gateGain_ = 1.0f;
        }

        const float attack = 1.0f - std::exp(-1.0f / (0.005f * rate_));
        const float release = 1.0f - std::exp(-1.0f / (0.040f * rate_));
        float blockPeak = 0.0f;
        for(size_t i = 0; i < count; ++i) {
            const size_t channel = i % channels_;
            float value = samples[i] / 32768.0f;
            if(!bypass_) {
                if(gateEnabled) {
                    if(channel == 0) {
                        const float target = gateOpen_ ? 1.0f : 0.0f;
                        const float coefficient = target > gateGain_ ? attack : release;
                        gateGain_ += coefficient * (target - gateGain_);
                    }
                    value *= gateGain_;
                }
                if(highPassHz_ > 0) {
                    for(auto& section : highPass_) value = section.process(value, channel);
                }
                if(lowPassHz_ > 0) {
                    for(auto& section : lowPass_) value = section.process(value, channel);
                }
                value *= gain_;
            }
            value = std::clamp(value, -1.0f, 0.999969f);
            blockPeak = std::max(blockPeak, std::abs(value));
            samples[i] = static_cast<int16_t>(std::lrint(value * 32768.0f));
        }
        peak_ = std::max(blockPeak, peak_ * 0.92f);
    }

    void setGain(float value) {
        std::lock_guard<std::mutex> lock(mutex_);
        gain_ = value;
    }
    void setGainDb(float value) {
        std::lock_guard<std::mutex> lock(mutex_);
        gain_ = std::pow(10.0f, value / 20.0f);
    }
    void setRouting(Routing value) {
        std::lock_guard<std::mutex> lock(mutex_);
        routing_ = value;
    }
    void setBypass(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(bypass_ && !value) updateFilters();
        bypass_ = value;
    }
    void setNoiseGateDb(float value) {
        std::lock_guard<std::mutex> lock(mutex_);
        noiseGateDb_ = value;
        if(value <= -119.9f) {
            gateOpen_ = true;
            gateGain_ = 1.0f;
        }
    }

    void setLowPass(float value) {
        std::lock_guard<std::mutex> lock(mutex_);
        lowPassHz_ = value;
        updateFilters();
    }

    void setHighPass(float value) {
        std::lock_guard<std::mutex> lock(mutex_);
        highPassHz_ = value;
        updateFilters();
    }

    void printStatus() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "Gain: " << gain_;
        if(gain_ > 0.0f) std::cout << " (" << 20.0f * std::log10(gain_) << " dB)";
        else std::cout << " (muted)";
        std::cout << " | High-pass: "
                  << (highPassHz_ > 0 ? std::to_string(highPassHz_) + " Hz" : "off")
                  << " | Low-pass: "
                  << (lowPassHz_ > 0 ? std::to_string(lowPassHz_) + " Hz" : "off")
                  << " | Routing: " << routingName(routing_)
                  << " | Bypass: " << (bypass_ ? "on" : "off")
                  << " | Gate: "
                  << (noiseGateDb_ > -119.9f ? std::to_string(noiseGateDb_) + " dBFS" : "off")
                  << '\n';
    }

    void printSignalLevels() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto db = [](float peak) {
            return peak > 0.0f ? 20.0f * std::log10(peak) : -120.0f;
        };
        std::cerr << "Signal peaks: input1=" << db(input1Peak_)
                  << " dBFS, input2=" << db(input2Peak_)
                  << " dBFS, output=" << db(peak_)
                  << " dBFS, gate=" << (gateOpen_ ? "open" : "closed") << '\n';
    }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {gain_, lowPassHz_, highPassHz_, peak_, input1Peak_, input2Peak_,
                noiseGateDb_, gateOpen_, bypass_, routing_};
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
    float peak_ = 0.0f;
    float input1Peak_ = 0.0f;
    float input2Peak_ = 0.0f;
    float noiseGateDb_;
    float gateGain_ = 0.0f;
    bool gateOpen_ = false;
    bool bypass_ = false;
    Routing routing_;
    std::array<Biquad, 2> lowPass_;
    std::array<Biquad, 2> highPass_;
    mutable std::mutex mutex_;
};

class WebControlServer {
public:
    WebControlServer(unsigned int port, unsigned int sampleRate, Processor& processor)
        : port_(port), sampleRate_(sampleRate), processor_(processor) {
        if(port_ == 0) return;

        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if(listener_ < 0) throw std::runtime_error("Cannot create web control socket");

        int reuse = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<uint16_t>(port_));
        if(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            const std::string message = "Cannot bind web control port " +
                                        std::to_string(port_) + ": " + std::strerror(errno);
            ::close(listener_);
            listener_ = -1;
            throw std::runtime_error(message);
        }
        if(::listen(listener_, 8) < 0) {
            ::close(listener_);
            listener_ = -1;
            throw std::runtime_error("Cannot listen on web control port");
        }

        worker_ = std::thread(&WebControlServer::serve, this);
        std::cout << "Browser controls: http://raspberrypi.local:" << port_ << '\n';
    }

    ~WebControlServer() {
        stop_ = true;
        if(listener_ >= 0) ::close(listener_);
        if(worker_.joinable()) worker_.join();
    }

    WebControlServer(const WebControlServer&) = delete;
    WebControlServer& operator=(const WebControlServer&) = delete;

private:
    static constexpr const char* page_ = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pi Audio Control</title>
<style>
:root{color-scheme:dark;--bg:#111410;--panel:#1b211b;--line:#344033;--text:#f1f4ec;--muted:#a9b3a5;--accent:#b8f34a;--accent2:#67d7c4}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 80% 0,#26351f 0,transparent 38%),var(--bg);color:var(--text);font:16px/1.45 system-ui,sans-serif}
main{width:min(1120px,calc(100% - 28px));margin:auto;padding:34px 0 50px}
header{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;margin-bottom:24px}
h1{font-size:clamp(2rem,8vw,4.6rem);line-height:.9;letter-spacing:-.06em;margin:0}.eyebrow{color:var(--accent);font-size:.75rem;font-weight:800;letter-spacing:.18em;text-transform:uppercase;margin-bottom:12px}
.status{border:1px solid var(--line);border-radius:99px;padding:8px 13px;color:var(--muted);white-space:nowrap}.status.online{color:var(--accent);border-color:#607d36}
.console{display:grid;grid-template-columns:repeat(3,108px) 170px 150px;justify-content:center;gap:12px}.card{background:color-mix(in srgb,var(--panel) 92%,transparent);border:1px solid var(--line);border-radius:18px;padding:14px;box-shadow:0 18px 50px #0004}
.row{display:flex;align-items:baseline;justify-content:space-between;gap:18px;margin-bottom:15px}.label{font-weight:750}.value{font:700 1.3rem ui-monospace,monospace;color:var(--accent)}
input[type=range]{width:100%;height:34px;margin:0;accent-color:var(--accent);cursor:pointer}small{display:block;color:var(--muted);margin-top:8px}
select{width:100%;background:#111610;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:12px;font:inherit}
.fader{height:430px;display:flex;flex-direction:column;align-items:center}.fader .row,.peak-card .row{width:100%;flex-direction:column;align-items:center;gap:2px;text-align:center}.vertical-slider{writing-mode:vertical-lr;direction:rtl;width:34px!important;height:305px!important;flex:1}
.utility{height:430px;display:flex;flex-direction:column;justify-content:space-between;gap:12px}.routing-block .label{display:block;margin-bottom:12px}.bypass-panel,.routing-panel{width:100%}.routing-panel{margin-top:auto}
.peak-card{height:430px;display:flex;flex-direction:column;align-items:center}.peak-card .row{width:100%}.meter-stack{display:flex;flex:1;min-height:0;align-items:stretch;gap:8px}.meter{width:30px;background:#0c100c;border:1px solid var(--line);border-radius:5px;overflow:hidden;display:flex;align-items:flex-end}.meter span{display:block;width:100%;height:0;background:linear-gradient(0deg,var(--accent2) 0 72%,#f4d35e 86%,#ff5c5c 100%);transition:height 70ms linear}
.scale{display:flex;flex-direction:column;justify-content:space-between;color:var(--muted);font:600 .68rem ui-monospace,monospace;padding:1px 0}
.bypass{width:100%;border:1px solid #607d36;background:#172014;color:var(--accent);border-radius:12px;padding:14px 18px;font:800 .86rem system-ui,sans-serif;letter-spacing:.1em;text-transform:uppercase;cursor:pointer}.bypass.active{background:#ffcb69;color:#241b08;border-color:#ffcb69}
footer{color:var(--muted);font-size:.82rem;margin-top:18px;text-align:center}
@media(max-width:780px){.console{grid-template-columns:repeat(3,minmax(92px,1fr))}.utility{grid-column:span 2}.peak-card{grid-column:span 1}.fader,.utility,.peak-card{height:390px}.vertical-slider{height:270px!important}}
</style>
</head>
<body><main>
<header><div><div class="eyebrow">Raspberry Pi · USB Audio</div><h1>Sound<br>shaping.</h1></div><div id="status" class="status">Connecting…</div></header>
<section class="console">
  <div class="card fader">
    <div class="row"><span class="label">Output gain</span><span id="gainValue" class="value">−1.9 dB</span></div>
    <input id="gain" class="vertical-slider" type="range" min="-60" max="12" step="0.1" value="-1.9" aria-label="Output gain in decibels">
  </div>
  <div class="card fader">
    <div class="row"><span class="label">Low-pass</span><span id="lpValue" class="value">Off</span></div>
    <input id="lp" class="vertical-slider" type="range" min="0" max="20000" step="50" value="0" aria-label="Low-pass cutoff">
  </div>
  <div class="card fader">
    <div class="row"><span class="label">High-pass</span><span id="hpValue" class="value">Off</span></div>
    <input id="hp" class="vertical-slider" type="range" min="0" max="5000" step="10" value="0" aria-label="High-pass cutoff">
  </div>
  <div class="utility">
    <div class="card bypass-panel">
      <button id="bypass" class="bypass" type="button" aria-pressed="false">BYPASS OFF</button>
    </div>
    <div class="card routing-panel routing-block">
      <span class="label">Input routing</span>
      <select id="routing" aria-label="Input routing">
        <option value="input2">Input 2 → both speakers</option>
        <option value="input1">Input 1 → both speakers</option>
        <option value="mix">Mix inputs → both speakers</option>
        <option value="stereo">Preserve stereo channels</option>
      </select>
    </div>
  </div>
  <div class="card peak-card">
    <div class="row"><span class="label">Output peak</span><span id="peakValue" class="value">&lt;-60dBFS</span></div>
    <div class="meter-stack">
      <div class="meter" role="meter" aria-label="Output peak level" aria-valuemin="-60" aria-valuemax="0" aria-valuenow="-60"><span id="peakBar"></span></div>
      <div class="scale"><span>0</span><span>−6</span><span>−18</span><span>−36</span><span>−60</span></div>
    </div>
  </div>
</section>
<footer>Controls update the running audio engine immediately · raspberrypi.local</footer>
</main>
<script>
const $=id=>document.getElementById(id);
const gain=$('gain'),hp=$('hp'),lp=$('lp'),routing=$('routing'),status=$('status'),bypass=$('bypass'),peakBar=$('peakBar'),peakValue=$('peakValue');
let timer,bypassed=false,bypassVersion=0;
const hz=v=>+v===0?'Off':(+v>=1000?(+v/1000).toFixed(+v%1000?1:0)+' kHz':v+' Hz');
function labels(){ $('gainValue').textContent=(+gain.value).toFixed(1)+' dB';$('hpValue').textContent=hz(hp.value);$('lpValue').textContent=hz(lp.value) }
function showBypass(value){bypassed=!!value;bypass.classList.toggle('active',bypassed);bypass.setAttribute('aria-pressed',bypassed);bypass.textContent=bypassed?'BYPASS ON':'BYPASS OFF'}
function showPeak(linear){const db=linear>0?20*Math.log10(linear):-120,p=Math.max(0,Math.min(100,(db+60)/60*100));peakBar.style.height=p+'%';peakValue.textContent=db<=-60?'<-60dBFS':db.toFixed(1)+'dBFS';peakBar.parentElement.setAttribute('aria-valuenow',Math.max(-60,db).toFixed(1))}
async function send(){
  clearTimeout(timer);
  const q=new URLSearchParams({gainDb:gain.value,highpass:hp.value,lowpass:lp.value,routing:routing.value});
  try{const r=await fetch('/api/set?'+q);if(!r.ok)throw Error();status.textContent='Live';status.className='status online'}catch{status.textContent='Disconnected';status.className='status'}
}
function changed(){labels();clearTimeout(timer);timer=setTimeout(send,45)}
[gain,hp,lp].forEach(x=>x.addEventListener('input',changed));routing.addEventListener('change',send);
async function setBypass(next){const version=++bypassVersion;showBypass(next);try{const r=await fetch('/api/set?bypass='+(next?'1':'0'));if(!r.ok)throw Error();const s=await r.json();if(version===bypassVersion)showBypass(s.bypass)}catch{if(version===bypassVersion)showBypass(!next)}}
bypass.addEventListener('click',()=>setBypass(!bypassed));
async function load(){
  const version=bypassVersion;
  try{const s=await(await fetch('/api/state')).json();gain.value=s.gainDb;hp.value=s.highpass;lp.value=s.lowpass;routing.value=s.routing;if(version===bypassVersion)showBypass(s.bypass);showPeak(s.peak);labels();status.textContent='Live';status.className='status online'}
  catch{status.textContent='Disconnected';status.className='status'}
}
async function meter(){try{const s=await(await fetch('/api/state')).json();showPeak(s.peak)}catch{}}
load();setInterval(load,5000);setInterval(meter,90);
</script></body></html>)HTML";

    static std::map<std::string, std::string> queryParameters(const std::string& target) {
        std::map<std::string, std::string> result;
        const size_t question = target.find('?');
        if(question == std::string::npos) return result;
        std::istringstream pairs(target.substr(question + 1));
        std::string pair;
        while(std::getline(pairs, pair, '&')) {
            const size_t equals = pair.find('=');
            if(equals != std::string::npos)
                result[pair.substr(0, equals)] = pair.substr(equals + 1);
        }
        return result;
    }

    std::string stateJson() const {
        const auto state = processor_.snapshot();
        const float gainDb = state.gain > 0 ? 20.0f * std::log10(state.gain) : -120.0f;
        std::ostringstream json;
        json << "{\"gainDb\":" << gainDb
             << ",\"lowpass\":" << state.lowPassHz
             << ",\"highpass\":" << state.highPassHz
             << ",\"peak\":" << state.peak
             << ",\"input1Peak\":" << state.input1Peak
             << ",\"input2Peak\":" << state.input2Peak
             << ",\"gateDb\":" << state.noiseGateDb
             << ",\"gateOpen\":" << (state.gateOpen ? "true" : "false")
             << ",\"bypass\":" << (state.bypass ? "true" : "false")
             << ",\"routing\":\"" << routingName(state.routing) << "\"}";
        return json.str();
    }

    void apply(const std::map<std::string, std::string>& parameters) {
        auto value = parameters.find("gainDb");
        if(value != parameters.end()) {
            const float gainDb = std::stof(value->second);
            if(gainDb < -120.0f || gainDb > 20.0f) throw std::runtime_error("gain");
            processor_.setGainDb(gainDb);
        }
        value = parameters.find("lowpass");
        if(value != parameters.end()) {
            const float cutoff = std::stof(value->second);
            if(cutoff < 0 || cutoff >= sampleRate_ * 0.5f) throw std::runtime_error("lowpass");
            processor_.setLowPass(cutoff);
        }
        value = parameters.find("highpass");
        if(value != parameters.end()) {
            const float cutoff = std::stof(value->second);
            if(cutoff < 0 || cutoff >= sampleRate_ * 0.5f) throw std::runtime_error("highpass");
            processor_.setHighPass(cutoff);
        }
        value = parameters.find("routing");
        if(value != parameters.end()) processor_.setRouting(parseRouting(value->second));
        value = parameters.find("gateDb");
        if(value != parameters.end()) {
            const float threshold = std::stof(value->second);
            if(threshold < -120.0f || threshold > -10.0f)
                throw std::runtime_error("gate");
            processor_.setNoiseGateDb(threshold);
        }
        value = parameters.find("bypass");
        if(value != parameters.end()) {
            if(value->second != "0" && value->second != "1")
                throw std::runtime_error("bypass");
            processor_.setBypass(value->second == "1");
        }
    }

    static void sendResponse(int client, const char* status, const char* contentType,
                             const std::string& body) {
        std::ostringstream response;
        response << "HTTP/1.1 " << status << "\r\n"
                 << "Content-Type: " << contentType << "\r\n"
                 << "Content-Length: " << body.size() << "\r\n"
                 << "Cache-Control: no-store\r\n"
                 << "Connection: close\r\n"
                 << "X-Content-Type-Options: nosniff\r\n\r\n"
                 << body;
        const std::string data = response.str();
        size_t sent = 0;
        while(sent < data.size()) {
            const ssize_t result = ::send(client, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if(result <= 0) break;
            sent += static_cast<size_t>(result);
        }
    }

    void handle(int client) {
        timeval timeout{1, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::array<char, 8192> buffer{};
        const ssize_t count = ::recv(client, buffer.data(), buffer.size() - 1, 0);
        if(count <= 0) return;

        std::istringstream request(std::string(buffer.data(), static_cast<size_t>(count)));
        std::string method, target, version;
        request >> method >> target >> version;
        if(method != "GET") {
            sendResponse(client, "405 Method Not Allowed", "text/plain", "GET required");
        } else if(target == "/") {
            sendResponse(client, "200 OK", "text/html; charset=utf-8", page_);
        } else if(target == "/api/state") {
            sendResponse(client, "200 OK", "application/json", stateJson());
        } else if(target.rfind("/api/set?", 0) == 0) {
            try {
                apply(queryParameters(target));
                sendResponse(client, "200 OK", "application/json", stateJson());
            } catch(...) {
                sendResponse(client, "400 Bad Request", "application/json",
                             "{\"error\":\"Invalid control value\"}");
            }
        } else {
            sendResponse(client, "404 Not Found", "text/plain", "Not found");
        }
    }

    void serve() {
        while(!stop_) {
            pollfd descriptor{listener_, POLLIN, 0};
            const int ready = poll(&descriptor, 1, 250);
            if(ready <= 0 || !(descriptor.revents & POLLIN)) continue;
            const int client = ::accept(listener_, nullptr, nullptr);
            if(client < 0) continue;
            handle(client);
            ::close(client);
        }
    }

    unsigned int port_;
    unsigned int sampleRate_;
    Processor& processor_;
    int listener_ = -1;
    std::atomic<bool> stop_{false};
    std::thread worker_;
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
    if(command == "levels") {
        processor.printSignalLevels();
        return;
    }
    if(command == "bypass") {
        std::string value;
        std::string extra;
        if(!(commandLine >> value) || (commandLine >> extra) ||
           (value != "on" && value != "off")) {
            std::cerr << "Expected: bypass on|off\n";
            return;
        }
        processor.setBypass(value == "on");
        processor.printStatus();
        return;
    }
    if(command == "gate") {
        std::string value;
        std::string extra;
        if(!(commandLine >> value) || (commandLine >> extra)) {
            std::cerr << "Expected: gate DB|off\n";
            return;
        }
        if(value == "off") {
            processor.setNoiseGateDb(-120.0f);
        } else {
            try {
                size_t used = 0;
                const float threshold = std::stof(value, &used);
                if(used != value.size() || threshold < -120.0f || threshold > -10.0f)
                    throw std::invalid_argument("range");
                processor.setNoiseGateDb(threshold);
            } catch(...) {
                std::cerr << "Gate threshold must be between -120 and -10 dBFS.\n";
                return;
            }
        }
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
        WebControlServer webControls(options.webPort, options.rate, processor);
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
