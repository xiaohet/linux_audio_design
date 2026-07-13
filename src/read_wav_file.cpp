#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include "read_wav_file.h"

WavData read_wav_file(const std::string& filePath) {
	// file path in use: "/mnt/d/AudioDev/Projects/PAProject1/audio.wav"
	std::ifstream file(filePath, std::ios::binary); // read wav file (with name "audio.wav")
	if (!file){ // no file; return error
		throw std::runtime_error("File not found!");
	}
	WavHeader header; // wav file metadata
	file.read(reinterpret_cast<char*>(&header), sizeof(WavHeader)); // read wav file header

	if(std::string(header.riffHeader, 4) != "RIFF" ||
		std::string(header.wavHeader, 4) != "WAVE") {
		throw std::runtime_error("Invalid WAV format!");
	}

	std::cout << "test print: " << header.riffHeader[0] << header.riffHeader[1] << 
		header.wavHeader[0] << header.wavHeader[1] << std::endl;
	std::cout << "--- WAV Metadata ---" << std::endl;
	std::cout << "Sample Rate: " << header.sampleRate << " Hz" << std::endl; // print sample rate
	std::cout << "Channels: " << header.numChannels << std::endl; // print number of channels
	std::cout << "Bit Depth: " << header.bitDepth << "-bit" << std::endl;
	std::cout << "Data Size: " << header.dataSize << " bytes" << std::endl;

	WavData dataRead;
	dataRead.sampleRate = static_cast<int>(header.sampleRate);
	dataRead.numChannels = static_cast<int>(header.numChannels);
	dataRead.bitDepth = static_cast<int>(header.bitDepth);

	if(header.bitDepth != 16){
		throw std::runtime_error("Only 16-bit WAV supported");
	}
	
	size_t sampleCount = header.dataSize / sizeof(int16_t);
	std::vector<int16_t> tempSamples(sampleCount);
	file.read(reinterpret_cast<char*>(tempSamples.data()), header.dataSize);

	dataRead.samples.resize(sampleCount);
	for(size_t i = 0; i < sampleCount; ++i)
	{
		dataRead.samples[i] = tempSamples[i] / 32768.0f;
	}

	// std::cout << "input audio test: " << std::endl;
	// for (int i = 1100; i < 1200; i++){
	// 	std::cout << dataRead.samples[i] << " ";
	// }
	// std::cout << std::endl;

	file.close();

	return dataRead;
}

