#include "mcp3008.h"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

Mcp3008::Mcp3008(const std::string& device, uint32_t speedHz)
    : speedHz_(speedHz) {
    file_ = open(device.c_str(), O_RDWR | O_CLOEXEC);
    if(file_ < 0)
        throw std::runtime_error("Cannot open " + device + ": " +
                                 std::strerror(errno));

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    if(ioctl(file_, SPI_IOC_WR_MODE, &mode) < 0 ||
       ioctl(file_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
       ioctl(file_, SPI_IOC_WR_MAX_SPEED_HZ, &speedHz_) < 0) {
        const std::string message = "Cannot configure " + device + ": " +
                                    std::strerror(errno);
        close(file_);
        file_ = -1;
        throw std::runtime_error(message);
    }
}

Mcp3008::~Mcp3008() {
    if(file_ >= 0) close(file_);
}

int Mcp3008::read(unsigned int channel) const {
    if(channel > 7) throw std::runtime_error("MCP3008 channel must be 0 through 7");

    uint8_t transmit[3]{1, static_cast<uint8_t>((8 + channel) << 4), 0};
    uint8_t receive[3]{};
    spi_ioc_transfer transfer{};
    transfer.tx_buf = reinterpret_cast<unsigned long>(transmit);
    transfer.rx_buf = reinterpret_cast<unsigned long>(receive);
    transfer.len = sizeof(transmit);
    transfer.speed_hz = speedHz_;
    transfer.bits_per_word = 8;

    if(ioctl(file_, SPI_IOC_MESSAGE(1), &transfer) < 0)
        throw std::runtime_error("MCP3008 SPI transfer failed: " +
                                 std::string(std::strerror(errno)));
    return ((receive[1] & 0x03) << 8) | receive[2];
}
