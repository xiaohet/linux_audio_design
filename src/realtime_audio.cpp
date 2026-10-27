#include "audio_config.h"
#include "command_interface.h"
#include "pcm_device.h"
#include "mcp3008.h"
#include "realtime_processor.h"
#include "web_control_server.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <algorithm>

namespace {
std::atomic<bool> running{true};

void stop(int) {
    running = false;
}

class TerminalControls {
public:
    TerminalControls(Processor& processor, const Pcm& capture, const Pcm& playback,
                     AudioTransport& transport)
        : worker_([this, &processor, &capture, &playback, &transport] {
            while(!stop_ && running) {
                RecoveryStats snapshot;
                { std::lock_guard<std::mutex> lock(mutex_); snapshot = stats_; }
                if(!handle_control_input(processor,capture,playback,snapshot,transport)) running=false;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }) {}
    ~TerminalControls() { stop_=true; worker_.join(); }
    void publish(const RecoveryStats& stats) {
        std::lock_guard<std::mutex> lock(mutex_); stats_=stats;
    }
private:
    std::mutex mutex_;
    RecoveryStats stats_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
}

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);

        Pcm capture(options.captureDevice, SND_PCM_STREAM_CAPTURE, options);
        Pcm playback(options.playbackDevice, SND_PCM_STREAM_PLAYBACK, options);
        Processor processor(options);
        std::unique_ptr<Mcp3008> hardwareGain;
        float filteredGainAdc = 0.0f;
        float filteredMixAdc = 0.0f;
        int lastAppliedGainAdc = -100;
        int lastAppliedMixAdc = -100;
        if(options.mcp3008) {
            hardwareGain = std::make_unique<Mcp3008>(options.mcp3008Device);
            filteredGainAdc = static_cast<float>(hardwareGain->read(options.mcp3008Channel));
            const float initialGainDb = options.hardwareGainMinDb +
                (filteredGainAdc / 1023.0f) *
                (options.hardwareGainMaxDb - options.hardwareGainMinDb);
            processor.set_gain_db(initialGainDb);
            lastAppliedGainAdc = static_cast<int>(filteredGainAdc + 0.5f);
            std::cout << "Hardware gain: MCP3008 CH" << options.mcp3008Channel
                      << " on " << options.mcp3008Device << " maps "
                      << options.hardwareGainMinDb << " to "
                      << options.hardwareGainMaxDb << " dB; initial ADC "
                      << lastAppliedGainAdc << " -> " << initialGainDb << " dB.\n";
            if(options.mcp3008MixChannel >= 0) {
                filteredMixAdc = static_cast<float>(
                    hardwareGain->read(static_cast<unsigned int>(options.mcp3008MixChannel)));
                processor.set_dry_wet(filteredMixAdc / 1023.0f);
                lastAppliedMixAdc = static_cast<int>(filteredMixAdc + 0.5f);
                std::cout << "Hardware dry/wet: MCP3008 CH"
                          << options.mcp3008MixChannel << "; initial ADC "
                          << lastAppliedMixAdc << " -> "
                          << (filteredMixAdc / 1023.0f * 100.0f) << "% wet.\n";
            }
        }
        AudioTransport transport(options.rate, options.channels, DEFAULT_DATA_DIR);
        WebControlServer webControls(options.webPort, processor, transport);
        std::vector<int16_t> samples(options.periodFrames * options.channels);
        std::vector<int16_t> rawInput(samples.size()), effectsPlayback(samples.size()), directPlayback(samples.size());
        RecoveryStats recoveries;

        std::cout << "Streaming " << options.rate << " Hz, " << options.channels
                  << " channels. Press Ctrl+C to stop.\n";
        print_control_help();
        processor.print_status();
        TerminalControls terminal(processor,capture,playback,transport);

        while(running) {
            if(hardwareGain) {
                const int raw = hardwareGain->read(options.mcp3008Channel);
                filteredGainAdc += 0.15f *
                    (static_cast<float>(raw) - filteredGainAdc);
                const int stableAdc = static_cast<int>(filteredGainAdc + 0.5f);
                if(std::abs(stableAdc - lastAppliedGainAdc) >= 3) {
                    const float position = filteredGainAdc / 1023.0f;
                    const float gainDb = options.hardwareGainMinDb + position *
                        (options.hardwareGainMaxDb - options.hardwareGainMinDb);
                    processor.set_gain_db(gainDb);
                    lastAppliedGainAdc = stableAdc;
                }
                if(options.mcp3008MixChannel >= 0) {
                    const int mixRaw = hardwareGain->read(
                        static_cast<unsigned int>(options.mcp3008MixChannel));
                    filteredMixAdc += 0.15f *
                        (static_cast<float>(mixRaw) - filteredMixAdc);
                    const int stableMixAdc = static_cast<int>(filteredMixAdc + 0.5f);
                    if(std::abs(stableMixAdc - lastAppliedMixAdc) >= 3) {
                        processor.set_dry_wet(filteredMixAdc / 1023.0f);
                        lastAppliedMixAdc = stableMixAdc;
                    }
                }
            }
            snd_pcm_sframes_t frames =
                snd_pcm_readi(capture.get(), samples.data(), options.periodFrames);
            if(frames < 0) {
                if(!running && frames == -EINTR) break;
                recover_pcm(capture, frames, "Capture", recoveries.capture,
                            recoveries, options.diagnostics);
                continue;
            }

            const size_t count = static_cast<size_t>(frames)*options.channels;
            std::copy_n(samples.data(), count, rawInput.data());
            transport.render_playback(effectsPlayback.data(),directPlayback.data(),static_cast<size_t>(frames));
            processor.process(samples, frames, effectsPlayback.data());
            for(size_t i=0; i<count; ++i)
                samples[i] = static_cast<int16_t>(std::clamp(int(samples[i])+int(directPlayback[i]),-32768,32767));
            transport.capture_recording(rawInput.data(),samples.data(),static_cast<size_t>(frames));
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

            terminal.publish(recoveries);
        }

        snd_pcm_drop(capture.get());
        snd_pcm_drain(playback.get());
    } catch(const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
