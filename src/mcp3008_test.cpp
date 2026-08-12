#include "mcp3008.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    try {
        const std::string device = argc > 1 ? argv[1] : "/dev/spidev0.0";
        const unsigned int channel = argc > 2 ? std::stoul(argv[2]) : 0;
        Mcp3008 adc(device);
        std::cout << "Reading MCP3008 CH" << channel << " on " << device
                  << " (Ctrl+C to stop)\n";
        while(true) {
            const int raw = adc.read(channel);
            std::cout << "raw=" << raw << "  voltage="
                      << raw * 3.3 / 1023.0 << " V\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } catch(const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
