#include "realtime_processor.h"
#include "deepfilter_processor.h"

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
      noiseGateDb_(options.noiseGateDb),
      compressorThresholdDb_(options.compressorThresholdDb),
      compressorRatio_(options.compressorRatio),
      compressorAttackMs_(options.compressorAttackMs),
      compressorReleaseMs_(options.compressorReleaseMs),
      compressorMakeupDb_(options.compressorMakeupDb), routing_(options.routing),
      equalizers_{Biquad(options.channels), Biquad(options.channels),
                  Biquad(options.channels), Biquad(options.channels),
                  Biquad(options.channels), Biquad(options.channels),
                  Biquad(options.channels)},
      dryFrame_(options.channels), wetFrame_(options.channels) {
    update_eq();
    try {
        deepFilter_ = std::make_unique<DeepFilterProcessor>(options);
        if(deepFilter_->available()) {
            std::cout << "DeepFilterNet ready: strength="
                      << options.deepFilterStrength * 100.0f
                      << "%, attenuation limit="
                      << options.deepFilterAttenuationLimitDb << " dB\n";
        } else {
            std::cerr << "DeepFilterNet unavailable; noise suppression is disabled. "
                         "Check --deepfilter-library and --deepfilter-model.\n";
        }
    } catch(const std::exception& error) {
        std::cerr << "DeepFilterNet disabled: " << error.what() << '\n';
        deepFilter_.reset();
    }
}

Processor::~Processor() = default;

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
    if(deepFilter_)
        deepFilter_->process(samples, static_cast<size_t>(frames), channels_, routing_);
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

    const float gateAttack = 1.0f - std::exp(-1.0f / (0.005f * rate_));
    const float gateRelease = 1.0f - std::exp(-1.0f / (0.040f * rate_));
    const float compressorAttack = 1.0f - std::exp(
        -1.0f / (compressorAttackMs_ * 0.001f * rate_));
    const float compressorRelease = 1.0f - std::exp(
        -1.0f / (compressorReleaseMs_ * 0.001f * rate_));
    float blockPeak = 0.0f;
    for(snd_pcm_sframes_t frame = 0; frame < frames; ++frame) {
        float detector = 0.0f;
        const size_t offset = static_cast<size_t>(frame) * channels_;
        if(gateEnabled) {
            const float target = gateOpen_ ? 1.0f : 0.0f;
            const float coefficient = target > gateGain_ ? gateAttack : gateRelease;
            gateGain_ += coefficient * (target - gateGain_);
        }
        for(size_t channel = 0; channel < channels_; ++channel) {
            const size_t index = offset + channel;
            dryFrame_[channel] = samples[index] / 32768.0f;
            wetFrame_[channel] = dryFrame_[channel];
            if(gateEnabled) wetFrame_[channel] *= gateGain_;
            for(auto& equalizer : equalizers_)
                wetFrame_[channel] = equalizer.process(wetFrame_[channel], channel);
            detector = std::max(detector, std::abs(wetFrame_[channel]));
        }

        const float envelopeCoefficient = detector > compressorEnvelope_
                                              ? compressorAttack
                                              : compressorRelease;
        compressorEnvelope_ += envelopeCoefficient *
                               (detector - compressorEnvelope_);
        float reductionDb = 0.0f;
        if(compressorEnvelope_ > 0.0f) {
            const float levelDb = 20.0f * std::log10(compressorEnvelope_);
            if(levelDb > compressorThresholdDb_) {
                const float compressedDb = compressorThresholdDb_ +
                    (levelDb - compressorThresholdDb_) / compressorRatio_;
                reductionDb = compressedDb - levelDb;
            }
        }
        compressorGainReductionDb_ = reductionDb;
        const float compressorGain = std::pow(
            10.0f, (reductionDb + compressorMakeupDb_) / 20.0f);

        for(size_t channel = 0; channel < channels_; ++channel) {
            const size_t index = offset + channel;
            const float wet = wetFrame_[channel] * compressorGain;
            float value = (dryFrame_[channel] * (1.0f - dryWet_) +
                           wet * dryWet_) * gain_;
            value = std::clamp(value, -1.0f, 0.999969f);
            blockPeak = std::max(blockPeak, std::abs(value));
            samples[index] = static_cast<int16_t>(std::lrint(value * 32768.0f));
        }
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

void Processor::set_compressor(float thresholdDb, float ratio, float attackMs,
                               float releaseMs, float makeupDb) {
    std::lock_guard<std::mutex> lock(mutex_);
    compressorThresholdDb_ = std::clamp(thresholdDb, -60.0f, 0.0f);
    compressorRatio_ = std::clamp(ratio, 1.0f, 20.0f);
    compressorAttackMs_ = std::clamp(attackMs, 0.1f, 200.0f);
    compressorReleaseMs_ = std::clamp(releaseMs, 10.0f, 2000.0f);
    compressorMakeupDb_ = std::clamp(makeupDb, 0.0f, 24.0f);
}

void Processor::set_noise_gate_db(float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    noiseGateDb_ = value;
    if(value <= -119.9f) {
        gateOpen_ = true;
        gateGain_ = 1.0f;
    }
}

void Processor::set_noise_suppression(float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(deepFilter_) deepFilter_->set_strength(value);
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
    const auto deep = deepFilter_ ? deepFilter_->snapshot()
                                  : DeepFilterProcessor::Snapshot{};
    std::cout << " | Routing: " << routing_name(routing_)
              << " | Noise suppression: "
              << (deep.available ? std::to_string(deep.strength * 100.0f) + "%"
                                 : "unavailable")
              << " | Dry/wet: " << dryWet_ * 100.0f << "% wet"
              << " | Compressor: " << compressorThresholdDb_ << " dB, "
              << compressorRatio_ << ":1, " << compressorAttackMs_ << "/"
              << compressorReleaseMs_ << " ms, makeup "
              << compressorMakeupDb_ << " dB"
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
    const auto deep = deepFilter_ ? deepFilter_->snapshot()
                                  : DeepFilterProcessor::Snapshot{};
    std::cerr << "Signal peaks: input1=" << db(input1Peak_)
              << " dBFS, input2=" << db(input2Peak_)
              << " dBFS, output=" << db(peak_)
              << " dBFS, gate=" << (gateOpen_ ? "open" : "closed")
              << ", deepfilter="
              << (deep.available ? std::to_string(deep.strength * 100.0f) + "%"
                                 : "unavailable") << '\n';
}

Processor::Snapshot Processor::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto deep = deepFilter_ ? deepFilter_->snapshot()
                                  : DeepFilterProcessor::Snapshot{};
    return {gain_, eqGainsDb_, peak_, input1Peak_, input2Peak_,
            noiseGateDb_, gateOpen_, dryWet_, compressorThresholdDb_,
            compressorRatio_, compressorAttackMs_, compressorReleaseMs_,
            compressorMakeupDb_, compressorGainReductionDb_,
            deep.available, deep.strength, deep.meanMilliseconds,
            deep.maximumMilliseconds, deep.framesProcessed,
            deep.deadlineMisses, deep.inputOverruns,
            deep.outputUnderruns, deep.staleOutputSamples, routing_};
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
