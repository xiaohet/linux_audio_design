#pragma once

#include "audio_config.h"

#include <alsa/asoundlib.h>

#include <chrono>
#include <string>

class Pcm {
public:
    Pcm(const std::string& device, snd_pcm_stream_t stream, const Options& options);
    ~Pcm();

    Pcm(const Pcm&) = delete;
    Pcm& operator=(const Pcm&) = delete;

    snd_pcm_t* get() const;
    snd_pcm_uframes_t period_frames() const;
    snd_pcm_uframes_t buffer_frames() const;

private:
    static void check(int error, const char* operation);

    snd_pcm_t* handle_ = nullptr;
    snd_pcm_uframes_t periodFrames_ = 0;
    snd_pcm_uframes_t bufferFrames_ = 0;
};

struct RecoveryStats {
    unsigned long capture = 0;
    unsigned long playback = 0;
    std::chrono::steady_clock::time_point lastDiagnostic{};
};

void print_pcm_status(const char* name, const Pcm& pcm);
void recover_pcm(Pcm& pcm, snd_pcm_sframes_t error, const char* direction,
                 unsigned long& counter, RecoveryStats& stats, bool diagnostics);
