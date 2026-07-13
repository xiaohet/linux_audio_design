#include "audio_filter.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

void apply_low_pass_filter(WavData& audioData, float cutoffHz) {
	if(audioData.sampleRate <= 0) {
		throw std::runtime_error("Invalid sample rate");
	}

	if(audioData.numChannels <= 0) {
		throw std::runtime_error("Invalid channel count");
	}

	const float nyquistHz = audioData.sampleRate * 0.5f;
	cutoffHz = std::clamp(cutoffHz, 1.0f, nyquistHz);

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
