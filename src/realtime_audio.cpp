#include "audio_config.h"
#include "command_interface.h"
#include "pcm_device.h"
#include "realtime_processor.h"
#include "web_control_server.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> running{true};

void stop(int) {
    running = false;
}
}

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);

        Pcm capture(options.captureDevice, SND_PCM_STREAM_CAPTURE, options);
        Pcm playback(options.playbackDevice, SND_PCM_STREAM_PLAYBACK, options);
        Processor processor(options);
        WebControlServer webControls(options.webPort, options.rate, processor);
        std::vector<int16_t> samples(options.periodFrames * options.channels);
        RecoveryStats recoveries;

        std::cout << "Streaming " << options.rate << " Hz, " << options.channels
                  << " channels. Press Ctrl+C to stop.\n";
        print_control_help();
        processor.print_status();

        while(running) {
            snd_pcm_sframes_t frames =
                snd_pcm_readi(capture.get(), samples.data(), options.periodFrames);
            if(frames < 0) {
                if(!running && frames == -EINTR) break;
                recover_pcm(capture, frames, "Capture", recoveries.capture,
                            recoveries, options.diagnostics);
                continue;
            }

            processor.process(samples, frames);
            snd_pcm_sframes_t written = 0;
            while(written < frames && running) {
                const auto result = snd_pcm_writei(
                    playback.get(),
                    samples.data() + written * options.channels,
                    frames - written);
                if(result < 0) {
                    if(!running && result == -EINTR) break;
                    recover_pcm(playback, result, "Playback", recoveries.playback,
                                recoveries, options.diagnostics);
                    break;
                }
                if(result == 0)
                    throw std::runtime_error("Playback made no progress");
                written += result;
            }

            if(!handle_control_input(processor, options.rate, capture, playback,
                                     recoveries)) {
                running = false;
            }
        }

        snd_pcm_drop(capture.get());
        snd_pcm_drain(playback.get());
    } catch(const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
