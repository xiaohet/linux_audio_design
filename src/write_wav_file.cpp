#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "write_wav_file.h"

template <typename T>
void write_binary(std::ostream& stream, T data) {
	stream.write(reinterpret_cast<const char*>(&data), sizeof(T));
}

void write_wav_file(const WavData& dataWrite, const std::string& fileName){
	// audio config
	const int sampleRate = dataWrite.sampleRate;
	const int bitDepth = dataWrite.bitDepth;
	const int numChannels = dataWrite.numChannels;
	std::vector<float> audioBuf = dataWrite.samples;
	std::vector<int16_t> outputPCM(audioBuf.size());

	const int numSamples = audioBuf.size();

	// convert audio data back to int from float
	for(size_t i = 0; i < audioBuf.size(); ++i)
	{
		float sample = audioBuf[i];
		sample = std::clamp(sample, -1.0f, 1.0f);
		outputPCM[i] = static_cast<int16_t>(sample * 32767.0f);
	}

	int32_t dataSize = numSamples * (bitDepth / 8);
	int32_t riffSize = 36 + dataSize;
	int32_t byteRate = sampleRate * numChannels * (bitDepth / 8);
	int16_t blockAlign = numChannels * (bitDepth / 8);

	// data sample test
	// std::cout << "output audio test: " << std::endl;
	// for (int i = 1100; i < 1200; i++){
	// 	std::cout << audioBuf[i] << " ";
	// }
	// std::cout << std::endl;

	std::ofstream wavFile(fileName, std::ios::binary);
	if (!wavFile.is_open()) {
		std::cerr << "Could not open file!" << std::endl;
		return;
	}

	// write header by order
	wavFile.write("RIFF", 4);
	write_binary(wavFile, riffSize);
	wavFile.write("WAVE", 4);

	wavFile.write("fmt ", 4);
	write_binary<int32_t>(wavFile, 16); // sub chunk size; 16 for PCM
	write_binary<int16_t>(wavFile, 1); // audio format; 1 for uncompressed PCM
	write_binary<int16_t>(wavFile, numChannels);
	write_binary<int32_t>(wavFile, sampleRate);
	write_binary<int32_t>(wavFile, byteRate);
	write_binary<int16_t>(wavFile, blockAlign);
	write_binary<int16_t>(wavFile, bitDepth);

	// write data descriptor
	wavFile.write("data", 4);
	write_binary(wavFile, dataSize);

	// write data
	wavFile.write(reinterpret_cast<const char*>(outputPCM.data()), dataSize);

	std::cout << "Write success!" << std::endl;
	wavFile.close();
}

