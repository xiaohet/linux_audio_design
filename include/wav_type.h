#pragma once

#include <cstdint>
#include <vector>

struct WavHeader {
	char riffHeader[4];
	uint32_t wavSize;
	char wavHeader[4];
	char fmtHeader[4];
	uint32_t fmtChunkSize;
	uint16_t audioFormat;
	uint16_t numChannels;
	uint32_t sampleRate;
	uint32_t byteRate;
	uint16_t sampleAlign;
	uint16_t bitDepth;
	char dataHeader[4];
	uint32_t dataSize;
};

struct WavData {
	int sampleRate;
	int numChannels;
	int bitDepth;
	std::vector<float> samples;
};

