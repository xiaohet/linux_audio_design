#include "command_interface.h"

#include "audio_config.h"

#include <cmath>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

void print_control_help() {
    std::cout
        << "\nReal-time controls (type a command and press Enter):\n"
        << "  gain DB                Set output gain in decibels, for example: gain -20\n"
        << "  mute                    Set gain to zero\n"
        << "  mix PERCENT             Set effects mix: 0 is dry, 100 is wet\n"
        << "  comp THRESH RATIO ATTACK RELEASE MAKEUP\n"
        << "                          Set compressor values in dB, ratio, ms, ms, dB\n"
        << "  eq FREQUENCY DB         Set a band gain; e.g. eq 320 -4\n"
        << "  eqoff                   Reset all seven EQ bands to 0 dB\n"
        << "  gate DB|off            Set or disable the input noise gate\n"
        << "  noise PERCENT          DeepFilterNet mix: 0 is off, 100 is full\n"
        << "  dfstats                Show DeepFilterNet timing and FIFO counters\n"
        << "  route MODE             stereo, input1, input2, or mix\n"
        << "  status                  Show current processing parameters\n"
        << "  levels                  Show pre-DSP input and output peak levels\n"
        << "  stats                   Show ALSA state and recovery counters\n"
        << "  help                    Show these commands\n"
        << "  quit                    Stop audio and exit\n\n";
}

bool handle_control_input(Processor& processor, const Pcm& capture,
                          const Pcm& playback,
                          const RecoveryStats& recoveries) {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if(poll(&input, 1, 0) <= 0 || !(input.revents & POLLIN)) return true;

    std::string line;
    if(!std::getline(std::cin, line)) return true;
    std::istringstream commandLine(line);
    std::string command;
    commandLine >> command;
    if(command.empty()) return true;

    if(command == "quit" || command == "q") return false;
    if(command == "help") {
        print_control_help();
        return true;
    }
    if(command == "status") {
        processor.print_status();
        return true;
    }
    if(command == "stats") {
        std::cerr << "Recoveries: capture=" << recoveries.capture
                  << ", playback=" << recoveries.playback << '\n';
        print_pcm_status("Capture", capture);
        print_pcm_status("Playback", playback);
        return true;
    }
    if(command == "levels") {
        processor.print_signal_levels();
        return true;
    }
    if(command == "dfstats") {
        const auto state = processor.snapshot();
        if(!state.deepFilterAvailable) {
            std::cerr << "DeepFilterNet is unavailable.\n";
        } else {
            std::cerr << "DeepFilterNet: strength="
                      << state.noiseSuppression * 100.0f
                      << "%, frames=" << state.deepFilterFrames
                      << ", mean=" << state.deepFilterMeanMs
                      << " ms, max=" << state.deepFilterMaximumMs
                      << " ms, deadline misses="
                      << state.deepFilterDeadlineMisses
                      << ", input overruns=" << state.deepFilterInputOverruns
                      << ", output underruns="
                      << state.deepFilterOutputUnderruns
                      << ", stale samples discarded="
                      << state.deepFilterStaleOutputSamples << '\n';
        }
        return true;
    }
    if(command == "route") {
        std::string value;
        std::string extra;
        if(!(commandLine >> value) || (commandLine >> extra)) {
            std::cerr << "Expected: route stereo|input1|input2|mix\n";
            return true;
        }
        try {
            processor.set_routing(parse_routing(value));
            processor.print_status();
        } catch(const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
        return true;
    }
    if(command == "mute") {
        processor.set_gain(0.0f);
        processor.print_status();
        return true;
    }
    if(command == "eqoff") {
        processor.reset_eq();
        processor.print_status();
        return true;
    }
    if(command == "gate") {
        std::string value;
        std::string extra;
        if(!(commandLine >> value) || (commandLine >> extra)) {
            std::cerr << "Expected: gate DB|off\n";
            return true;
        }
        if(value == "off") {
            processor.set_noise_gate_db(-120.0f);
        } else {
            try {
                size_t used = 0;
                const float threshold = std::stof(value, &used);
                if(used != value.size() || threshold < -120.0f || threshold > -10.0f)
                    throw std::invalid_argument("range");
                processor.set_noise_gate_db(threshold);
            } catch(...) {
                std::cerr << "Gate threshold must be between -120 and -10 dBFS.\n";
                return true;
            }
        }
        processor.print_status();
        return true;
    }

    if(command == "eq") {
        float frequency = 0.0f;
        float gain = 0.0f;
        std::string extra;
        if(!(commandLine >> frequency >> gain) || (commandLine >> extra) ||
           !std::isfinite(frequency) || !std::isfinite(gain)) {
            std::cerr << "Expected: eq FREQUENCY DB\n";
            return true;
        }
        size_t band = Processor::EqBandCount;
        for(size_t index = 0; index < Processor::EqBandCount; ++index) {
            if(std::abs(frequency - Processor::EqFrequencies[index]) < 0.5f) {
                band = index;
                break;
            }
        }
        if(band == Processor::EqBandCount) {
            std::cerr << "Frequency must be 80, 160, 320, 640, 1280, 2560, or 5120 Hz.\n";
            return true;
        }
        if(gain < -18.0f || gain > 18.0f) {
            std::cerr << "EQ gain must be between -18 and +18 dB.\n";
            return true;
        }
        processor.set_eq_gain_db(band, gain);
        processor.print_status();
        return true;
    }

    if(command == "comp") {
        float threshold = 0.0f;
        float ratio = 0.0f;
        float attack = 0.0f;
        float release = 0.0f;
        float makeup = 0.0f;
        std::string extra;
        if(!(commandLine >> threshold >> ratio >> attack >> release >> makeup) ||
           (commandLine >> extra) || !std::isfinite(threshold) ||
           !std::isfinite(ratio) || !std::isfinite(attack) ||
           !std::isfinite(release) || !std::isfinite(makeup)) {
            std::cerr << "Expected: comp THRESHOLD_DB RATIO ATTACK_MS RELEASE_MS MAKEUP_DB\n";
            return true;
        }
        if(threshold < -60.0f || threshold > 0.0f ||
           ratio < 1.0f || ratio > 20.0f || attack < 0.1f ||
           attack > 200.0f || release < 10.0f || release > 2000.0f ||
           makeup < 0.0f || makeup > 24.0f) {
            std::cerr << "Compressor ranges: threshold -60..0 dB, ratio 1..20, "
                         "attack 0.1..200 ms, release 10..2000 ms, makeup 0..24 dB.\n";
            return true;
        }
        processor.set_compressor(threshold, ratio, attack, release, makeup);
        processor.print_status();
        return true;
    }

    if(command != "gain" && command != "mix" &&
       command != "noise") {
        std::cerr << "Unknown command. Type help for available controls.\n";
        return true;
    }

    float value = 0.0f;
    std::string extra;
    if(!(commandLine >> value) || (commandLine >> extra) || !std::isfinite(value)) {
        std::cerr << "Expected: " << command << " VALUE\n";
        return true;
    }

    if(command == "gain") {
        if(value < -120.0f || value > 20.0f) {
            std::cerr << "Gain must be between -120 and +20 dB.\n";
            return true;
        }
        processor.set_gain_db(value);
    } else if(command == "mix") {
        if(value < 0.0f || value > 100.0f) {
            std::cerr << "Mix must be between 0 and 100 percent.\n";
            return true;
        }
        processor.set_dry_wet(value / 100.0f);
    } else {
        if(value < 0.0f || value > 100.0f) {
            std::cerr << "Noise suppression must be between 0 and 100 percent.\n";
            return true;
        }
        processor.set_noise_suppression(value / 100.0f);
    }
    processor.print_status();
    return true;
}
