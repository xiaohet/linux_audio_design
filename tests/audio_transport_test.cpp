#include "audio_transport.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
void rejected(const std::function<void()>& action) {
    bool failed=false;
    try { action(); } catch(const std::exception&) { failed=true; }
    assert(failed);
}
uint32_t read32(std::ifstream& f) {
    unsigned char b[4]; f.read(reinterpret_cast<char*>(b),4);
    return b[0]|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);
}
int main() {
    const auto dir=fs::temp_directory_path()/ ("pi-transport-test-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    {
        AudioTransport t(48000,2,dir);
        assert(t.files().empty());
        assert(t.record()=="record0000.wav");
        rejected([&]{t.record();}); rejected([&]{t.play("record0000.wav");});
        int16_t output[]={1000,-1000,2000,-2000,3000,-3000,4000,-4000};
        t.process(output,4);
        assert(t.stop_recording()=="record0000.wav");
        std::ifstream f(dir/"record0000.wav",std::ios::binary);
        f.seekg(40); assert(read32(f)==16);
        for(auto sample : output) { unsigned char b[2]; f.read(reinterpret_cast<char*>(b),2);
            assert(static_cast<int16_t>(b[0]|(unsigned(b[1])<<8))==sample); }
        f.close();
        fs::copy_file(dir/"record0000.wav",dir/"arbitrary name + %.WAV");
        assert(t.files().size()==2);
        t.play("arbitrary name + %.WAV");
        assert(t.state().playing);
        rejected([&]{t.record();}); rejected([&]{t.play("record0000.wav");});
        int16_t live[]={500,500,500,500,500,500,500,500};
        t.process(live,4);
        for(size_t i=0;i<8;++i) assert(live[i]==output[i]+500);
        assert(!t.state().playing);
        assert(t.record()=="record0001.wav");
        t.stop_recording();
        rejected([&]{t.play("../record0000.wav");});
        rejected([&]{t.play("missing.wav");});
        rejected([&]{t.stop_recording();});
        // A half-rate file is linearly resampled and mono is sent to both channels.
        fs::copy_file(dir/"record0000.wav",dir/"mono.wav");
        std::fstream mono(dir/"mono.wav",std::ios::binary|std::ios::in|std::ios::out);
        mono.seekp(22); mono.put(1); mono.put(0);
        mono.seekp(24); mono.put(char(0xc0)); mono.put(char(0x5d)); mono.put(0); mono.put(0);
        mono.seekp(32); mono.put(2); mono.put(0); mono.close();
        t.play("mono.wav");
        int16_t silent[8]{}; t.process(silent,4);
        assert(silent[0]==1000 && silent[1]==1000);
        assert(silent[2]==0 && silent[3]==0);
        assert(silent[4]==-1000 && silent[5]==-1000);
        t.stop_playback(); assert(!t.state().playing);
        std::ofstream broken(dir/"broken.wav",std::ios::binary); broken << "RIFF"; broken.close();
        rejected([&]{t.play("broken.wav");});
        fs::copy_file(dir/"record0000.wav",dir/"truncated.wav");
        fs::resize_file(dir/"truncated.wav",45);
        rejected([&]{t.play("truncated.wav");});
        // Destructor must flush the final period and finalize an active recording.
        t.record(); t.process(output,4);
    }
    std::ifstream final(dir/"record0002.wav",std::ios::binary);
    final.seekg(40); assert(read32(final)==16); final.close();
    fs::remove_all(dir);
    std::cout << "Audio transport tests passed\n";
}
