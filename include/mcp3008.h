#pragma once

#include <cstdint>
#include <string>

class Mcp3008 {
public:
    explicit Mcp3008(const std::string& device, uint32_t speedHz = 1000000);
    ~Mcp3008();

    Mcp3008(const Mcp3008&) = delete;
    Mcp3008& operator=(const Mcp3008&) = delete;

    int read(unsigned int channel) const;

private:
    int file_ = -1;
    uint32_t speedHz_;
};
