#include "audio_filter.h"
#include <cmath>
#include <iomanip>
#include <iostream>

namespace {
constexpr float kPi = 3.14159265358979323846f;

WavData make_sine(float frequencyHz, float amplitude, float seconds, int sampleRate) {
	WavData data;
	data.sampleRate = sampleRate;
	data.numChannels = 1;
	data.bitDepth = 16;

	const int sampleCount = static_cast<int>(seconds * sampleRate);
	data.samples.resize(sampleCount);
	for(int i = 0; i < sampleCount; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
		data.samples[i] = amplitude * std::sin(2.0f * kPi * frequencyHz * t);
	}

	return data;
}

float rms_after_settling(const WavData& data, float skipSeconds) {
	const size_t start = static_cast<size_t>(skipSeconds * data.sampleRate) * data.numChannels;
	double sumSquares = 0.0;
	size_t count = 0;

	for(size_t i = start; i < data.samples.size(); ++i) {
		sumSquares += data.samples[i] * data.samples[i];
		++count;
	}

	return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sumSquares / count));
}

void test_tone(float toneHz, float cutoffHz) {
	WavData tone = make_sine(toneHz, 0.8f, 1.0f, 48000);
	const float before = rms_after_settling(tone, 0.05f);

	apply_low_pass_filter(tone, cutoffHz);
	const float after = rms_after_settling(tone, 0.05f);
	const float gainDb = 20.0f * std::log10(after / before);

	std::cout << std::setw(6) << toneHz << " Hz tone, "
		<< std::setw(6) << cutoffHz << " Hz cutoff: "
		<< "RMS before = " << before
		<< ", RMS after = " << after
		<< ", change = " << gainDb << " dB\n";
}
}

int main() {
	std::cout << std::fixed << std::setprecision(4);
	test_tone(200.0f, 1000.0f);
	test_tone(5000.0f, 1000.0f);
	test_tone(5000.0f, 8000.0f);
	return 0;
}