#pragma once

#include <alsa/asoundlib.h>

#include <array>
#include <cstddef>
#include <string>

enum class Routing {
    Stereo,
    Input1ToStereo,
    Input2ToStereo,
    MixToStereo
};

const char* routing_name(Routing routing);
Routing parse_routing(const std::string& value);

struct Options {
    std::string captureDevice = "plughw:CARD=USB,DEV=0";
    std::string playbackDevice = "plughw:CARD=USB,DEV=0";
    unsigned int rate = 48000;
    unsigned int channels = 2;
    snd_pcm_uframes_t periodFrames = 512;
    snd_pcm_uframes_t bufferFrames = 2048;
    float gain = 0.8f;
    std::array<float, 7> eqGainsDb{};
    float compressorThresholdDb = -18.0f;
    float compressorRatio = 1.0f;
    float compressorAttackMs = 10.0f;
    float compressorReleaseMs = 100.0f;
    float compressorMakeupDb = 0.0f;
    float noiseGateDb = -55.0f;
    std::string deepFilterLibrary =
        "$HOME/DeepFilterNet/target/aarch64-unknown-linux-gnu/release/libdeepfilter.so";
    std::string deepFilterModel =
        "$HOME/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz";
    float deepFilterStrength = 0.0f;
    float deepFilterAttenuationLimitDb = 20.0f;
    size_t deepFilterDelaySamples = 1440;
    bool diagnostics = false;
    Routing routing = Routing::Input2ToStereo;
    unsigned int webPort = 8080;
};

void print_usage(const char* name);
void list_devices();
Options parse_options(int argc, char** argv);
