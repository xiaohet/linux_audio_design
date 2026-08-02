#pragma once

#include "audio_config.h"

#include <alsa/asoundlib.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

class Biquad {
public:
    explicit Biquad(unsigned int channels);
    void configure_peaking(float frequencyHz, float gainDb, float sampleRate, float q);
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
    static constexpr size_t EqBandCount = 7;
    static constexpr std::array<float, EqBandCount> EqFrequencies{
        80.0f, 160.0f, 320.0f, 640.0f, 1280.0f, 2560.0f, 5120.0f};
    static constexpr float EqQ = 1.4f;

    struct Snapshot {
        float gain;
        std::array<float, EqBandCount> eqGainsDb;
        float peak;
        float input1Peak;
        float input2Peak;
        float noiseGateDb;
        bool gateOpen;
        float dryWet;
        float compressorThresholdDb;
        float compressorRatio;
        float compressorAttackMs;
        float compressorReleaseMs;
        float compressorMakeupDb;
        float compressorGainReductionDb;
        Routing routing;
    };

    explicit Processor(const Options& options);

    void process(std::vector<int16_t>& samples, snd_pcm_sframes_t frames);
    void set_gain(float value);
    void set_gain_db(float value);
    void set_routing(Routing value);
    void set_dry_wet(float value);
    void set_compressor(float thresholdDb, float ratio, float attackMs,
                        float releaseMs, float makeupDb);
    void set_noise_gate_db(float value);
    void set_eq_gain_db(size_t band, float value);
    void reset_eq();
    void print_status() const;
    void print_signal_levels() const;
    Snapshot snapshot() const;

private:
    void apply_routing(std::vector<int16_t>& samples, snd_pcm_sframes_t frames) const;
    void update_eq();

    unsigned int rate_;
    unsigned int channels_;
    float gain_;
    std::array<float, EqBandCount> eqGainsDb_;
    float peak_ = 0.0f;
    float input1Peak_ = 0.0f;
    float input2Peak_ = 0.0f;
    float noiseGateDb_;
    float gateGain_ = 0.0f;
    bool gateOpen_ = false;
    float dryWet_ = 1.0f;
    float compressorThresholdDb_;
    float compressorRatio_;
    float compressorAttackMs_;
    float compressorReleaseMs_;
    float compressorMakeupDb_;
    float compressorEnvelope_ = 0.0f;
    float compressorGainReductionDb_ = 0.0f;
    Routing routing_;
    std::array<Biquad, EqBandCount> equalizers_;
    std::vector<float> dryFrame_;
    std::vector<float> wetFrame_;
    mutable std::mutex mutex_;
};
