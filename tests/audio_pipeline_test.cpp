#include "audio_transport.h"
#include "realtime_processor.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <vector>

int main() {
    const auto directory = std::filesystem::temp_directory_path()/
        ("pi-pipeline-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Options options;
    options.gain = 0.5f;
    options.noiseGateDb = -120;
    options.routing = Routing::Input2ToStereo;
    options.deepFilterLibrary = "/nonexistent/pi-pipeline-test/libdeepfilter.so";
    {
        AudioTransport transport(options.rate,2,directory);
        Processor processor(options);
        // Make a known stereo file without requiring an ALSA device.
        const std::vector<int16_t> fileAudio{3000,4000,3000,4000};
        transport.record();
        transport.capture_recording(fileAudio.data(),fileAudio.data(),2);
        transport.stop_recording();
        const auto settings = transport.state().settings;
        for(bool effectsMode : {false,true}) {
            transport.set_settings({false,effectsMode,settings.directory});
            transport.play("record0000.wav");
            std::vector<int16_t> live{1000,2000,1000,2000}, effects(4), direct(4);
            transport.render_playback(effects.data(),direct.data(),2);
            processor.process(live,2,effects.data());
            for(size_t i=0; i<live.size(); ++i)
                live[i]=static_cast<int16_t>(std::clamp(int(live[i])+int(direct[i]),-32768,32767));
            // Live input 2 is routed to both channels. WAV stereo bypasses routing.
            // Only effects playback receives the shared 0.5 output gain.
            const std::vector<int16_t> expected = effectsMode
                ? std::vector<int16_t>{2500,3000,2500,3000}
                : std::vector<int16_t>{4000,5000,4000,5000};
            assert(live == expected);
        }
    }
    std::filesystem::remove_all(directory);
}
