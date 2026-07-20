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
#ifndef DEFAULT_PROJECT_DIR
#define DEFAULT_PROJECT_DIR "."
#endif

#ifndef DEFAULT_DATA_DIR
#define DEFAULT_DATA_DIR "data"
#endif

std::string default_data_path(const std::string& fileName) {
	return std::string(DEFAULT_DATA_DIR) + "/" + fileName;
}

bool is_absolute_path(const std::string& path) {
	return !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
}

bool contains_path_separator(const std::string& path) {
	return path.find('/') != std::string::npos || path.find('\\') != std::string::npos;
}

std::string resolve_audio_path(const std::string& path) {
	if(is_absolute_path(path)) {
		return path;
	}

	if(contains_path_separator(path)) {
		return std::string(DEFAULT_PROJECT_DIR) + "/" + path;
	}

	return default_data_path(path);
}

void print_usage(const char* programName) {
	std::cout << "Usage:\n"
		<< "  " << programName << " <input.wav> <output.wav> <gain> <low_pass_cutoff_hz> [high_pass_cutoff_hz]\n\n"
		<< "Examples:\n"
		<< "  " << programName << " data/audio.wav data/audio_out.wav 0.8 1000\n"
		<< "  " << programName << " audio.wav audio_out.wav 0.8 1000 0\n"
		<< "  " << programName << " data/audio.wav data/audio_out.wav 0.8 8000 200\n\n"
		<< "Relative folders are resolved from the project folder. Plain filenames use the data folder.\n"
		<< "Use high_pass_cutoff_hz = 0 to disable the high-pass filter.\n"
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
	float highPassCutoffHz = 0.0f;

	try
	{
		if(argc == 2 && std::string(argv[1]) == "--help") {
			print_usage(argv[0]);
			return 0;
		}

		if(argc == 1) {
			filePath = read_string_prompt("Input WAV path", default_data_path("audio.wav"));
			outFilePath = read_string_prompt("Output WAV path", default_data_path("audio_out.wav"));
			gain = read_float_prompt("Gain", gain);
			lowPassCutoffHz = read_float_prompt("Low-pass cutoff Hz", lowPassCutoffHz);
			highPassCutoffHz = read_float_prompt("High-pass cutoff Hz (0 disables)", highPassCutoffHz);
		} else if(argc == 5 || argc == 6) {
			filePath = resolve_audio_path(argv[1]);
			outFilePath = resolve_audio_path(argv[2]);
			gain = std::stof(argv[3]);
			lowPassCutoffHz = std::stof(argv[4]);
			if(argc == 6) {
				highPassCutoffHz = std::stof(argv[5]);
			}
		} else {
			print_usage(argv[0]);
			return 1;
		}

		WavData wavData = read_wav_file(filePath);
		
		// gain effect
		for(auto& datum : wavData.samples){
			datum *= gain;
		}

		// high-pass filter effect
		if(highPassCutoffHz > 0.0f) {
			apply_high_pass_filter(wavData, highPassCutoffHz);
		}

		// low-pass filter effect
		apply_low_pass_filter(wavData, lowPassCutoffHz);

		write_wav_file(wavData, outFilePath);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return 1;
	}

	return 0;
}