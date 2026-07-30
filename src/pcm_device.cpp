#include "pcm_device.h"

#include <cerrno>
#include <iostream>
#include <stdexcept>

Pcm::Pcm(const std::string& device, snd_pcm_stream_t stream, const Options& options) {
    int error = snd_pcm_open(&handle_, device.c_str(), stream, 0);
    if(error < 0)
        throw std::runtime_error("Cannot open " + device + ": " + snd_strerror(error));

    snd_pcm_hw_params_t* hardware;
    snd_pcm_hw_params_alloca(&hardware);
    check(snd_pcm_hw_params_any(handle_, hardware), "initialize hardware parameters");
    check(snd_pcm_hw_params_set_access(handle_, hardware, SND_PCM_ACCESS_RW_INTERLEAVED),
          "set interleaved access");
    check(snd_pcm_hw_params_set_format(handle_, hardware, SND_PCM_FORMAT_S16_LE),
          "set S16_LE format");
    check(snd_pcm_hw_params_set_channels(handle_, hardware, options.channels),
          "set channel count");

    unsigned int rate = options.rate;
    int direction = 0;
    check(snd_pcm_hw_params_set_rate_near(handle_, hardware, &rate, &direction),
          "set sample rate");
    if(rate != options.rate)
        throw std::runtime_error(device + " does not support requested sample rate exactly");

    snd_pcm_uframes_t period = options.periodFrames;
    check(snd_pcm_hw_params_set_period_size_near(handle_, hardware, &period, &direction),
          "set period size");
    snd_pcm_uframes_t buffer = options.bufferFrames;
    check(snd_pcm_hw_params_set_buffer_size_near(handle_, hardware, &buffer),
          "set buffer size");
    check(snd_pcm_hw_params(handle_, hardware), "apply hardware parameters");
    check(snd_pcm_hw_params_get_period_size(hardware, &periodFrames_, &direction),
          "read negotiated period size");
    check(snd_pcm_hw_params_get_buffer_size(hardware, &bufferFrames_),
          "read negotiated buffer size");

    snd_pcm_sw_params_t* software;
    snd_pcm_sw_params_alloca(&software);
    check(snd_pcm_sw_params_current(handle_, software), "initialize software parameters");
    check(snd_pcm_sw_params_set_avail_min(handle_, software, periodFrames_),
          "set minimum available frames");
    if(stream == SND_PCM_STREAM_PLAYBACK) {
        const snd_pcm_uframes_t threshold =
            bufferFrames_ > periodFrames_ ? bufferFrames_ - periodFrames_ : periodFrames_;
        check(snd_pcm_sw_params_set_start_threshold(handle_, software, threshold),
              "set playback start threshold");
    }
    check(snd_pcm_sw_params(handle_, software), "apply software parameters");
    check(snd_pcm_prepare(handle_), "prepare PCM");

    if(options.diagnostics) {
        std::cerr << (stream == SND_PCM_STREAM_CAPTURE ? "Capture" : "Playback")
                  << " ALSA settings: rate=" << rate
                  << ", channels=" << options.channels
                  << ", period=" << periodFrames_
                  << ", buffer=" << bufferFrames_ << '\n';
    }
}

Pcm::~Pcm() {
    if(handle_) snd_pcm_close(handle_);
}

snd_pcm_t* Pcm::get() const { return handle_; }
snd_pcm_uframes_t Pcm::period_frames() const { return periodFrames_; }
snd_pcm_uframes_t Pcm::buffer_frames() const { return bufferFrames_; }

void Pcm::check(int error, const char* operation) {
    if(error < 0)
        throw std::runtime_error(std::string(operation) + ": " + snd_strerror(error));
}

void print_pcm_status(const char* name, const Pcm& pcm) {
    snd_pcm_status_t* status;
    snd_pcm_status_alloca(&status);
    const int result = snd_pcm_status(pcm.get(), status);
    if(result < 0) {
        std::cerr << name << " status unavailable: " << snd_strerror(result) << '\n';
        return;
    }
    std::cerr << name
              << " state=" << snd_pcm_state_name(snd_pcm_status_get_state(status))
              << ", available=" << snd_pcm_status_get_avail(status)
              << ", delay=" << snd_pcm_status_get_delay(status)
              << ", period=" << pcm.period_frames()
              << ", buffer=" << pcm.buffer_frames() << '\n';
}

void recover_pcm(Pcm& pcm, snd_pcm_sframes_t error, const char* direction,
                 unsigned long& counter, RecoveryStats& stats, bool diagnostics) {
    if(error != -EPIPE && error != -ESTRPIPE && error != -EINTR)
        throw std::runtime_error(std::string(direction) + " failed: " +
                                 snd_strerror(static_cast<int>(error)));

    ++counter;
    const auto now = std::chrono::steady_clock::now();
    if(diagnostics &&
       (stats.lastDiagnostic.time_since_epoch().count() == 0 ||
        now - stats.lastDiagnostic >= std::chrono::seconds(2))) {
        std::cerr << direction << " I/O error: " << snd_strerror(static_cast<int>(error))
                  << " (recovery #" << counter << ")\n";
        print_pcm_status(direction, pcm);
        stats.lastDiagnostic = now;
    }

    const int result = snd_pcm_recover(pcm.get(), static_cast<int>(error), 1);
    if(result < 0)
        throw std::runtime_error(std::string(direction) + " recovery failed: " +
                                 snd_strerror(result));
}
