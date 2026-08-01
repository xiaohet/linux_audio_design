#include "realtime_processor.h"

#include <algorithm>
#include <cmath>
#include <iostream>

Biquad::Biquad(unsigned int channels) : z1_(channels), z2_(channels) {}

void Biquad::configure_peaking(float frequencyHz, float gainDb,
                               float sampleRate, float q) {
    constexpr float pi = 3.14159265358979323846f;
    const float amplitude = std::pow(10.0f, gainDb / 40.0f);
    const float omega = 2.0f * pi * frequencyHz / sampleRate;
    const float cosine = std::cos(omega);
    const float sine = std::sin(omega);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha / amplitude;

    b0_ = (1.0f + alpha * amplitude) / a0;
    b1_ = (-2.0f * cosine) / a0;
    b2_ = (1.0f - alpha * amplitude) / a0;
    a1_ = (-2.0f * cosine) / a0;
    a2_ = (1.0f - alpha / amplitude) / a0;
    reset();
}

float Biquad::process(float input, size_t channel) {
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

void Biquad::reset() {
    std::fill(z1_.begin(), z1_.end(), 0.0f);
    std::fill(z2_.begin(), z2_.end(), 0.0f);
}

Processor::Processor(const Options& options)
    : rate_(options.rate), channels_(options.channels), gain_(options.gain),
      eqGainsDb_(options.eqGainsDb),
      noiseGateDb_(options.noiseGateDb), routing_(options.routing),
      equalizers_{Biquad(options.channels), Biquad(options.channels),
                  Biquad(options.channels), Biquad(options.channels),
                  Biquad(options.channels), Biquad(options.channels),
                  Biquad(options.channels)} {
    update_eq();
}

void Processor::process(std::vector<int16_t>& samples, snd_pcm_sframes_t frames) {
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

    apply_routing(samples, frames);
    const size_t count = static_cast<size_t>(frames) * channels_;
    float routedPeak = 0.0f;
    for(size_t i = 0; i < count; ++i)
        routedPeak = std::max(routedPeak, std::abs(samples[i] / 32768.0f));

    const bool gateEnabled = noiseGateDb_ > -119.9f;
    if(gateEnabled) {
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
        const float dry = samples[i] / 32768.0f;
        float wet = dry;
        if(gateEnabled) {
            if(channel == 0) {
                const float target = gateOpen_ ? 1.0f : 0.0f;
                const float coefficient = target > gateGain_ ? attack : release;
                gateGain_ += coefficient * (target - gateGain_);
            }
            wet *= gateGain_;
        }
        for(auto& equalizer : equalizers_)
            wet = equalizer.process(wet, channel);
        float value = (dry * (1.0f - dryWet_) + wet * dryWet_) * gain_;
        value = std::clamp(value, -1.0f, 0.999969f);
        blockPeak = std::max(blockPeak, std::abs(value));
        samples[i] = static_cast<int16_t>(std::lrint(value * 32768.0f));
    }
    peak_ = std::max(blockPeak, peak_ * 0.92f);
}

void Processor::set_gain(float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gain_ = value;
}

void Processor::set_gain_db(float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gain_ = std::pow(10.0f, value / 20.0f);
}

void Processor::set_routing(Routing value) {
    std::lock_guard<std::mutex> lock(mutex_);
    routing_ = value;
}

void Processor::set_dry_wet(float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    dryWet_ = std::clamp(value, 0.0f, 1.0f);
}

void Processor::set_noise_gate_db(float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    noiseGateDb_ = value;
    if(value <= -119.9f) {
        gateOpen_ = true;
        gateGain_ = 1.0f;
    }
}

void Processor::set_eq_gain_db(size_t band, float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(band >= EqBandCount) return;
    eqGainsDb_[band] = value;
    update_eq();
}

void Processor::reset_eq() {
    std::lock_guard<std::mutex> lock(mutex_);
    eqGainsDb_.fill(0.0f);
    update_eq();
}

void Processor::print_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "Gain: " << gain_;
    if(gain_ > 0.0f) std::cout << " (" << 20.0f * std::log10(gain_) << " dB)";
    else std::cout << " (muted)";
    std::cout << " | EQ:";
    for(size_t band = 0; band < EqBandCount; ++band)
        std::cout << ' ' << EqFrequencies[band] << "Hz="
                  << eqGainsDb_[band] << "dB";
    std::cout << " | Routing: " << routing_name(routing_)
              << " | Dry/wet: " << dryWet_ * 100.0f << "% wet"
              << " | Gate: "
              << (noiseGateDb_ > -119.9f
                      ? std::to_string(noiseGateDb_) + " dBFS"
                      : "off")
              << '\n';
}

void Processor::print_signal_levels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto db = [](float peak) {
        return peak > 0.0f ? 20.0f * std::log10(peak) : -120.0f;
    };
    std::cerr << "Signal peaks: input1=" << db(input1Peak_)
              << " dBFS, input2=" << db(input2Peak_)
              << " dBFS, output=" << db(peak_)
              << " dBFS, gate=" << (gateOpen_ ? "open" : "closed") << '\n';
}

Processor::Snapshot Processor::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {gain_, eqGainsDb_, peak_, input1Peak_, input2Peak_,
            noiseGateDb_, gateOpen_, dryWet_, routing_};
}

void Processor::apply_routing(std::vector<int16_t>& samples,
                              snd_pcm_sframes_t frames) const {
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

void Processor::update_eq() {
    for(size_t band = 0; band < EqBandCount; ++band)
        equalizers_[band].configure_peaking(
            EqFrequencies[band], eqGainsDb_[band],
            static_cast<float>(rate_), EqQ);
}
