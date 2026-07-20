#include "audio_filter.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
void validate_audio_data(const WavData& audioData) {
	if(audioData.sampleRate <= 0) {
		throw std::runtime_error("Invalid sample rate");
	}

	if(audioData.numChannels <= 0) {
		throw std::runtime_error("Invalid channel count");
	}
}

float clamp_cutoff(float cutoffHz, int sampleRate) {
	const float nyquistHz = sampleRate * 0.5f;
	return std::clamp(cutoffHz, 1.0f, nyquistHz);
}
}

void apply_low_pass_filter(WavData& audioData, float cutoffHz) {
	validate_audio_data(audioData);
	cutoffHz = clamp_cutoff(cutoffHz, audioData.sampleRate);

	const float dt = 1.0f / static_cast<float>(audioData.sampleRate);
	const float rc = 1.0f / (2.0f * static_cast<float>(M_PI) * cutoffHz);
	const float alpha = dt / (rc + dt);

	std::vector<float> previous(audioData.numChannels, 0.0f);
	for(size_t i = 0; i < audioData.samples.size(); ++i) {
		const int channel = i % audioData.numChannels;
		previous[channel] = previous[channel] + alpha * (audioData.samples[i] - previous[channel]);
		audioData.samples[i] = previous[channel];
	}
}

void apply_high_pass_filter(WavData& audioData, float cutoffHz) {
	validate_audio_data(audioData);
	cutoffHz = clamp_cutoff(cutoffHz, audioData.sampleRate);

	const float dt = 1.0f / static_cast<float>(audioData.sampleRate);
	const float rc = 1.0f / (2.0f * static_cast<float>(M_PI) * cutoffHz);
	const float alpha = rc / (rc + dt);

	std::vector<float> previousInput(audioData.numChannels, 0.0f);
	std::vector<float> previousOutput(audioData.numChannels, 0.0f);
	for(size_t i = 0; i < audioData.samples.size(); ++i) {
		const int channel = i % audioData.numChannels;
		const float input = audioData.samples[i];
		const float output = alpha * (previousOutput[channel] + input - previousInput[channel]);
		previousInput[channel] = input;
		previousOutput[channel] = output;
		audioData.samples[i] = output;
	}
}