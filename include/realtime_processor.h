#pragma once

#include "audio_config.h"

#include <alsa/asoundlib.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

class Biquad {
public:
    enum class Type { LowPass, HighPass };

    explicit Biquad(unsigned int channels);
    void configure(Type type, float cutoffHz, float sampleRate, float q);
    float process(float input, size_t channel);

private:
    void reset();

    float b0_ = 1.0f;
    float b1_ = 0.0f;
    float b2_ = 0.0f;
    float a1_ = 0.0f;
    float a2_ = 0.0f;
    std::vector<float> z1_;
    std::vector<float> z2_;
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

    explicit Processor(const Options& options);

    void process(std::vector<int16_t>& samples, snd_pcm_sframes_t frames);
    void set_gain(float value);
    void set_gain_db(float value);
    void set_routing(Routing value);
    void set_bypass(bool value);
    void set_noise_gate_db(float value);
    void set_low_pass(float value);
    void set_high_pass(float value);
    void print_status() const;
    void print_signal_levels() const;
    Snapshot snapshot() const;

private:
    void apply_routing(std::vector<int16_t>& samples, snd_pcm_sframes_t frames) const;
    void update_filters();

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
