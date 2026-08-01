#pragma once

#include <alsa/asoundlib.h>

#include <array>
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
    float noiseGateDb = -55.0f;
    bool diagnostics = false;
    Routing routing = Routing::Input2ToStereo;
    unsigned int webPort = 8080;
};

void print_usage(const char* name);
void list_devices();
Options parse_options(int argc, char** argv);
