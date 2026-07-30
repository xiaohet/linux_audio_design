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
        << "  gain VALUE             Set linear gain, for example: gain 0.5\n"
        << "  gaindb DB              Set gain in decibels, for example: gaindb -20\n"
        << "  mute                    Set gain to zero\n"
        << "  bypass on|off           Bypass gain, gate, and both filters\n"
        << "  lowpass HZ             Set cutoff; 0 disables the low-pass filter\n"
        << "  highpass HZ            Set cutoff; 0 disables the high-pass filter\n"
        << "  gate DB|off            Set or disable the input noise gate\n"
        << "  route MODE             stereo, input1, input2, or mix\n"
        << "  status                  Show current processing parameters\n"
        << "  levels                  Show pre-DSP input and output peak levels\n"
        << "  stats                   Show ALSA state and recovery counters\n"
        << "  help                    Show these commands\n"
        << "  quit                    Stop audio and exit\n\n";
}

bool handle_control_input(Processor& processor, unsigned int sampleRate,
                          const Pcm& capture, const Pcm& playback,
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
    if(command == "bypass") {
        std::string value;
        std::string extra;
        if(!(commandLine >> value) || (commandLine >> extra) ||
           (value != "on" && value != "off")) {
            std::cerr << "Expected: bypass on|off\n";
            return true;
        }
        processor.set_bypass(value == "on");
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

    if(command != "gain" && command != "gaindb" &&
       command != "lowpass" && command != "highpass") {
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
        if(value < 0.0f || value > 10.0f) {
            std::cerr << "Gain must be between 0 and 10.\n";
            return true;
        }
        processor.set_gain(value);
    } else if(command == "gaindb") {
        if(value < -120.0f || value > 20.0f) {
            std::cerr << "Gain must be between -120 and +20 dB.\n";
            return true;
        }
        processor.set_gain_db(value);
    } else if(command == "lowpass") {
        if(value < 0.0f || value >= sampleRate * 0.5f) {
            std::cerr << "Low-pass cutoff must be 0 or below the Nyquist frequency.\n";
            return true;
        }
        processor.set_low_pass(value);
    } else {
        if(value < 0.0f || value >= sampleRate * 0.5f) {
            std::cerr << "High-pass cutoff must be 0 or below the Nyquist frequency.\n";
            return true;
        }
        processor.set_high_pass(value);
    }
    processor.print_status();
    return true;
}
