#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "audio_filter.h"
#include "read_wav_file.h"
#include "write_wav_file.h"

namespace {
void print_usage(const char* programName) {
	std::cout << "Usage:\n"
		<< "  " << programName << " <input.wav> <output.wav> <gain> <cutoff_hz>\n\n"
		<< "Example:\n"
		<< "  " << programName << " audio.wav audio_out.wav 0.8 1000\n\n"
		<< "Run without arguments for interactive mode.\n";
}

float read_float_prompt(const std::string& label, float defaultValue) {
	std::cout << label << " [" << defaultValue << "]: ";

	std::string input;
	std::getline(std::cin, input);
	if(input.empty()) {
		return defaultValue;
	}

	std::stringstream stream(input);
	float value = defaultValue;
	if(!(stream >> value)) {
		throw std::runtime_error("Invalid number for " + label);
	}

	return value;
}

std::string read_string_prompt(const std::string& label, const std::string& defaultValue) {
	std::cout << label << " [" << defaultValue << "]: ";

	std::string input;
	std::getline(std::cin, input);
	return input.empty() ? defaultValue : input;
}
}

int main(int argc, char* argv[]) {
	std::string filePath;
	std::string outFilePath;
	float gain = 1.0f;
	float cutoffHz = 1000.0f;

	try
	{
		if(argc == 2 && std::string(argv[1]) == "--help") {
			print_usage(argv[0]);
			return 0;
		}

		if(argc == 1) {
			filePath = read_string_prompt("Input WAV path", "/mnt/d/AudioDev/Projects/PAProject1/audio.wav");
			outFilePath = read_string_prompt("Output WAV path", "/mnt/d/AudioDev/Projects/PAProject1/audio_out.wav");
			gain = read_float_prompt("Gain", gain);
			cutoffHz = read_float_prompt("Low-pass cutoff Hz", cutoffHz);
		} else if(argc == 5) {
			filePath = argv[1];
			outFilePath = argv[2];
			gain = std::stof(argv[3]);
			cutoffHz = std::stof(argv[4]);
		} else {
			print_usage(argv[0]);
			return 1;
		}

		WavData wavData = read_wav_file(filePath);
		
		// gain effect
		for(auto& datum : wavData.samples){
			datum *= gain;
		}

		// low-pass filter effect
		apply_low_pass_filter(wavData, cutoffHz);

		write_wav_file(wavData, outFilePath);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return 1;
	}

	return 0;
}
