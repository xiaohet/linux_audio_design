#include "audio_config.h"

#include <alsa/asoundlib.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

const char* routing_name(Routing routing) {
    switch(routing) {
        case Routing::Stereo: return "stereo";
        case Routing::Input1ToStereo: return "input1";
        case Routing::Input2ToStereo: return "input2";
        case Routing::MixToStereo: return "mix";
    }
    return "unknown";
}

Routing parse_routing(const std::string& value) {
    if(value == "stereo") return Routing::Stereo;
    if(value == "input1") return Routing::Input1ToStereo;
    if(value == "input2") return Routing::Input2ToStereo;
    if(value == "mix") return Routing::MixToStereo;
    throw std::runtime_error(
        "Invalid routing mode; use stereo, input1, input2, or mix");
}

void print_usage(const char* name) {
    std::cout
        << "Usage: " << name << " [options]\n"
        << "  --capture DEVICE       ALSA capture PCM (default plughw:CARD=USB,DEV=0)\n"
        << "  --playback DEVICE      ALSA playback PCM (default plughw:CARD=USB,DEV=0)\n"
        << "  --rate HZ              Sample rate (default 48000)\n"
        << "  --channels N           Channel count (default 2)\n"
        << "  --period FRAMES        Frames per processing period (default 512)\n"
        << "  --buffer FRAMES        ALSA buffer frames (default 2048)\n"
        << "  --gain VALUE           Linear gain (default 0.8)\n"
        << "  --gain-db DB           Gain in decibels (overrides --gain)\n"
        << "  Seven-band EQ frequencies are fixed at 80, 160, 320, 640,\n"
        << "  1280, 2560, and 5120 Hz; gains default to 0 dB.\n"
        << "  --gate-db DB           Noise-gate threshold (default -55; -120 disables)\n"
        << "  --deepfilter-library PATH  DeepFilterNet v0.5.6 C library\n"
        << "  --deepfilter-model PATH    DeepFilterNet3 ONNX model archive\n"
        << "  --noise-suppression PERCENT  Initial suppression mix (default 0)\n"
        << "  --deepfilter-atten-limit DB  Maximum suppression (default 20)\n"
        << "  --deepfilter-delay SAMPLES   Model delay before scheduling buffer (default 1440)\n"
        << "  --routing MODE         stereo, input1, input2, or mix (default input2)\n"
        << "  --web-port PORT        Browser control port (default 8080; 0 disables)\n"
        << "  --mcp3008              Enable MCP3008 controls (default)\n"
        << "  --no-mcp3008           Disable MCP3008 controls\n"
        << "  --mcp3008-device PATH  SPI device (default /dev/spidev0.0)\n"
        << "  --mcp3008-channel N    ADC channel 0-7 (default 0)\n"
        << "  --mcp3008-mix-channel N  ADC channel 0-7 for dry/wet (default 1)\n"
        << "  --hardware-gain-min DB Gain at ADC value 0 (default -60)\n"
        << "  --hardware-gain-max DB Gain at ADC value 1023 (default +12)\n"
        << "  --diagnostics          Print negotiated ALSA settings and rate-limited xrun details\n"
        << "  --list-devices         Print ALSA PCM device names\n"
        << "  --help                 Show this help\n";
}

void list_devices() {
    void** hints = nullptr;
    const int result = snd_device_name_hint(-1, "pcm", &hints);
    if(result < 0) throw std::runtime_error(snd_strerror(result));
    for(void** hint = hints; *hint != nullptr; ++hint) {
        char* name = snd_device_name_get_hint(*hint, "NAME");
        char* description = snd_device_name_get_hint(*hint, "DESC");
        if(name) std::cout << name << (description ? "\t" : "\n");
        if(description) {
            for(char* p = description; *p; ++p) {
                if(*p == '\n') *p = ' ';
            }
            std::cout << description << '\n';
        }
        std::free(name);
        std::free(description);
    }
    snd_device_name_free_hint(hints);
}

namespace {
template<typename T>
T number(const char* text, const std::string& option);

template<>
unsigned int number<unsigned int>(const char* text, const std::string& option) {
    try {
        size_t used = 0;
        const auto value = std::stoul(text, &used);
        if(used != std::strlen(text) || value == 0)
            throw std::invalid_argument("range");
        return static_cast<unsigned int>(value);
    } catch(...) {
        throw std::runtime_error("Invalid value for " + option);
    }
}

template<>
float number<float>(const char* text, const std::string& option) {
    try {
        size_t used = 0;
        const float value = std::stof(text, &used);
        if(used != std::strlen(text) || !std::isfinite(value))
            throw std::invalid_argument("range");
        return value;
    } catch(...) {
        throw std::runtime_error("Invalid value for " + option);
    }
}
}

Options parse_options(int argc, char** argv) {
    Options options;
    for(int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if(argument == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if(argument == "--list-devices") {
            list_devices();
            std::exit(0);
        }
        if(argument == "--diagnostics") {
            options.diagnostics = true;
            continue;
        }
        if(argument == "--mcp3008") {
            options.mcp3008 = true;
            continue;
        }
        if(argument == "--no-mcp3008") {
            options.mcp3008 = false;
            continue;
        }
        if(i + 1 >= argc)
            throw std::runtime_error("Missing value for " + argument);

        const char* value = argv[++i];
        if(argument == "--capture") options.captureDevice = value;
        else if(argument == "--playback") options.playbackDevice = value;
        else if(argument == "--rate") options.rate = number<unsigned int>(value, argument);
        else if(argument == "--channels") options.channels = number<unsigned int>(value, argument);
        else if(argument == "--period") options.periodFrames = number<unsigned int>(value, argument);
        else if(argument == "--buffer") options.bufferFrames = number<unsigned int>(value, argument);
        else if(argument == "--gain") options.gain = number<float>(value, argument);
        else if(argument == "--gain-db")
            options.gain = std::pow(10.0f, number<float>(value, argument) / 20.0f);
        else if(argument == "--gate-db") options.noiseGateDb = number<float>(value, argument);
        else if(argument == "--deepfilter-library") options.deepFilterLibrary = value;
        else if(argument == "--deepfilter-model") options.deepFilterModel = value;
        else if(argument == "--noise-suppression")
            options.deepFilterStrength = number<float>(value, argument) / 100.0f;
        else if(argument == "--deepfilter-atten-limit")
            options.deepFilterAttenuationLimitDb = number<float>(value, argument);
        else if(argument == "--deepfilter-delay")
            options.deepFilterDelaySamples = number<unsigned int>(value, argument);
        else if(argument == "--routing") options.routing = parse_routing(value);
        else if(argument == "--web-port") {
            try {
                size_t used = 0;
                const auto port = std::stoul(value, &used);
                if(used != std::strlen(value) || port > 65535)
                    throw std::invalid_argument("range");
                options.webPort = static_cast<unsigned int>(port);
            } catch(...) {
                throw std::runtime_error("Invalid value for --web-port");
            }
        } else if(argument == "--mcp3008-device") options.mcp3008Device = value;
        else if(argument == "--mcp3008-channel") {
            try {
                size_t used = 0;
                const auto channel = std::stoul(value, &used);
                if(used != std::strlen(value) || channel > 7)
                    throw std::invalid_argument("range");
                options.mcp3008Channel = static_cast<unsigned int>(channel);
            } catch(...) {
                throw std::runtime_error("Invalid value for --mcp3008-channel");
            }
        }
        else if(argument == "--mcp3008-mix-channel") {
            try {
                size_t used = 0;
                const auto channel = std::stoul(value, &used);
                if(used != std::strlen(value) || channel > 7)
                    throw std::invalid_argument("range");
                options.mcp3008MixChannel = static_cast<int>(channel);
            } catch(...) {
                throw std::runtime_error("Invalid value for --mcp3008-mix-channel");
            }
        }
        else if(argument == "--hardware-gain-min")
            options.hardwareGainMinDb = number<float>(value, argument);
        else if(argument == "--hardware-gain-max")
            options.hardwareGainMaxDb = number<float>(value, argument);
        else {
            throw std::runtime_error("Unknown option: " + argument);
        }
    }

    if(options.bufferFrames < options.periodFrames * 2)
        throw std::runtime_error("--buffer must be at least twice --period");
    if(options.noiseGateDb < -120.0f || options.noiseGateDb > -10.0f)
        throw std::runtime_error("--gate-db must be between -120 and -10 dBFS");
    if(options.deepFilterStrength < 0.0f || options.deepFilterStrength > 1.0f)
        throw std::runtime_error("--noise-suppression must be between 0 and 100");
    if(options.deepFilterAttenuationLimitDb < 0.0f ||
       options.deepFilterAttenuationLimitDb > 100.0f)
        throw std::runtime_error("--deepfilter-atten-limit must be between 0 and 100 dB");
    if(options.mcp3008Channel > 7)
        throw std::runtime_error("--mcp3008-channel must be between 0 and 7");
    if(options.mcp3008 && options.mcp3008MixChannel == static_cast<int>(options.mcp3008Channel))
        throw std::runtime_error("MCP3008 gain and mix controls must use different channels");
    if(options.hardwareGainMinDb >= options.hardwareGainMaxDb)
        throw std::runtime_error("--hardware-gain-min must be less than --hardware-gain-max");
    return options;
}
