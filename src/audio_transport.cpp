#include "audio_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
uint32_t le(const unsigned char* p, unsigned bytes) {
    uint32_t n = 0;
    for(unsigned i = 0; i < bytes; ++i) n |= uint32_t(p[i]) << (8*i);
    return n;
}
void put(std::ostream& f, uint32_t n, unsigned bytes) {
    for(unsigned i = 0; i < bytes; ++i) f.put(static_cast<char>(n >> (8*i)));
}
void header(std::ostream& f, unsigned rate, unsigned channels, uint32_t bytes) {
    f.seekp(0); f.write("RIFF",4); put(f,36+bytes,4); f.write("WAVEfmt ",8);
    put(f,16,4); put(f,1,2); put(f,channels,2); put(f,rate,4);
    put(f,rate*channels*2,4); put(f,channels*2,2); put(f,16,2);
    f.write("data",4); put(f,bytes,4);
}
bool wav(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){return std::tolower(c);});
    return ext == ".wav";
}
}

struct AudioTransport::Recording {
    std::ofstream file;
    std::string name;
    std::vector<int16_t> ring;
    std::atomic<size_t> read{0}, write{0};
    std::atomic<bool> stopping{false}, failed{false};
    std::thread worker;
    unsigned rate, channels;
    Recording(const std::filesystem::path& path, unsigned r, unsigned c)
        : file(path, std::ios::binary), name(path.filename().string()),
          ring(size_t(r)*c*4), rate(r), channels(c) {
        if(!file) throw std::runtime_error("Cannot create recording: " + path.string());
        header(file,rate,channels,0);
        worker = std::thread([this] {
            uint32_t bytes = 0;
            while(true) {
                const size_t start = read.load();
                const size_t end = write.load();
                if(start == end) {
                    if(stopping.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                const size_t count = std::min(end-start, ring.size()-start%ring.size());
                if(count*2 > uint64_t(std::numeric_limits<uint32_t>::max()-36)-bytes) {
                    failed = true; break;
                }
                // Raspberry Pi and supported Linux hosts use little-endian PCM.
                file.write(reinterpret_cast<const char*>(ring.data()+start%ring.size()), count*2);
                if(!file) { failed = true; break; }
                bytes += static_cast<uint32_t>(count*2);
                read.store(start+count);
            }
            if(file) { header(file,rate,channels,bytes); file.flush(); }
            if(!file) failed = true;
        });
    }
    ~Recording() { stopping = true; if(worker.joinable()) worker.join(); }
};
struct AudioTransport::Playback {
    std::string name;
    std::vector<float> samples;
    unsigned rate = 0, channels = 0;
    double position = 0;
};

AudioTransport::AudioTransport(unsigned rate, unsigned channels, std::filesystem::path dir)
    : rate_(rate), channels_(channels), directory_(std::move(dir)) {
    std::filesystem::create_directories(directory_);
}
AudioTransport::~AudioTransport() { try { stop_recording(); } catch(...) {} }

std::vector<std::string> AudioTransport::files() const {
    std::vector<std::string> names;
    for(const auto& entry : std::filesystem::directory_iterator(directory_))
        if(entry.is_regular_file() && !entry.is_symlink() && wav(entry.path()))
            names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}
std::string AudioTransport::record() {
    std::lock_guard<std::mutex> command(commands_);
    { std::lock_guard<std::mutex> lock(mutex_);
      if(recording_ || (playback_ && playback_->position < playback_->samples.size()/playback_->channels))
          throw std::runtime_error("Recording/playback is already active"); }
    std::filesystem::path path;
    for(unsigned i = 0; i < 10000; ++i) {
        std::ostringstream name; name << "record" << std::setw(4) << std::setfill('0') << i << ".wav";
        path = directory_/name.str();
        if(!std::filesystem::exists(path) && !std::filesystem::is_symlink(path)) break;
        path.clear();
    }
    if(path.empty()) throw std::runtime_error("All record0000..record9999 names are occupied");
    auto session = std::make_unique<Recording>(path,rate_,channels_);
    std::lock_guard<std::mutex> lock(mutex_);
    lastFile_ = session->name; error_.clear(); recording_ = std::move(session);
    return lastFile_;
}
std::string AudioTransport::stop_recording() {
    std::lock_guard<std::mutex> command(commands_);
    std::unique_ptr<Recording> session;
    { std::lock_guard<std::mutex> lock(mutex_); session = std::move(recording_); }
    if(!session) throw std::runtime_error("No recording is active");
    session->stopping = true; session->worker.join();
    const auto name = session->name;
    if(session->failed) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Recording failed (disk error, WAV size limit, or writer buffer overflow): " + name;
        throw std::runtime_error(error_);
    }
    return name;
}
void AudioTransport::stop_playback() {
    std::lock_guard<std::mutex> command(commands_);
    std::unique_ptr<Playback> old;
    { std::lock_guard<std::mutex> lock(mutex_); old = std::move(playback_); }
}
AudioTransport::State AudioTransport::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {bool(recording_), playback_ && playback_->position < playback_->samples.size()/playback_->channels,
            lastFile_, recording_ && recording_->failed ? "Recording writer failed; stop recording to finalize the file" : error_};
}
void AudioTransport::play(const std::string& name) {
    std::lock_guard<std::mutex> command(commands_);
    { std::lock_guard<std::mutex> lock(mutex_);
      if(recording_ || (playback_ && playback_->position < playback_->samples.size()/playback_->channels))
          throw std::runtime_error("Recording/playback is already active"); }
    const std::filesystem::path relative(name);
    const auto path = directory_/relative;
    if(name.empty() || relative != relative.filename() || !wav(relative) ||
       std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path))
        throw std::runtime_error("Select a WAV filename from the data directory");
    std::ifstream f(path,std::ios::binary);
    unsigned char h[12];
    if(!f.read(reinterpret_cast<char*>(h),12) || std::memcmp(h,"RIFF",4) || std::memcmp(h+8,"WAVE",4))
        throw std::runtime_error("Invalid RIFF WAV file");
    auto next = std::make_unique<Playback>(); next->name = name;
    unsigned format = 0, bits = 0, align = 0;
    std::streampos dataAt{}; uint32_t dataSize = 0;
    while(f.read(reinterpret_cast<char*>(h),8)) {
        const uint32_t size = le(h+4,4); const auto start = f.tellg();
        if(!std::memcmp(h,"fmt ",4)) {
            unsigned char fmt[16];
            if(size < 16 || !f.read(reinterpret_cast<char*>(fmt),16)) break;
            format=le(fmt,2); next->channels=le(fmt+2,2); next->rate=le(fmt+4,4);
            align=le(fmt+12,2); bits=le(fmt+14,2);
        } else if(!std::memcmp(h,"data",4)) { dataAt=start; dataSize=size; }
        if(dataSize && format) break;
        f.seekg(start+static_cast<std::streamoff>(uint64_t(size)+(size&1)));
    }
    if(!dataSize || !next->rate || next->rate > 384000 ||
       (next->channels != 1 && next->channels != 2) ||
       !((format==1 && (bits==8 || bits==16 || bits==24 || bits==32)) || (format==3 && bits==32)) ||
       align != next->channels*(bits/8) || dataSize%align)
        throw std::runtime_error("WAV must be mono/stereo PCM 8/16/24/32-bit or float32");
    // Bound memory use on the Pi; decoding happens on the command/web thread.
    const size_t count = dataSize/(bits/8);
    if(count > 64*1024*1024) throw std::runtime_error("WAV exceeds 256 MiB decoded playback limit");
    f.clear(); f.seekg(0,std::ios::end);
    if(f.tellg()-dataAt < static_cast<std::streamoff>(dataSize)) throw std::runtime_error("Truncated WAV data");
    f.seekg(dataAt); next->samples.resize(count);
    for(auto& sample : next->samples) {
        unsigned char b[4]{};
        if(!f.read(reinterpret_cast<char*>(b),bits/8)) throw std::runtime_error("Cannot read WAV data");
        uint32_t value=le(b,bits/8);
        if(format==3) std::memcpy(&sample,&value,4);
        else if(bits==8) sample=(int(value)-128)/128.0f;
        else {
            int64_t signedValue=value;
            if(value & (uint32_t(1)<<(bits-1))) signedValue-=int64_t(1)<<bits;
            sample=static_cast<float>(signedValue/double(uint64_t(1)<<(bits-1)));
        }
        if(!std::isfinite(sample)) sample=0;
        sample=std::clamp(sample,-1.0f,1.0f);
    }
    std::unique_ptr<Playback> old;
    { std::lock_guard<std::mutex> lock(mutex_);
      old=std::move(playback_); playback_=std::move(next); lastFile_=name; error_.clear(); }
}
void AudioTransport::process(int16_t* output, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(playback_) {
        auto& p=*playback_; const size_t length=p.samples.size()/p.channels;
        for(size_t i=0; i<frames && p.position<length; ++i,p.position+=double(p.rate)/rate_) {
            const size_t a=static_cast<size_t>(p.position), b=std::min(a+1,length-1);
            const float fraction=static_cast<float>(p.position-a);
            for(unsigned ch=0; ch<channels_; ++ch) {
                const unsigned source=p.channels==1 ? 0 : ch%p.channels;
                float sample=p.samples[a*p.channels+source]*(1-fraction)+p.samples[b*p.channels+source]*fraction;
                if(channels_==1 && p.channels==2)
                    sample=(sample+p.samples[a*2+1]*(1-fraction)+p.samples[b*2+1]*fraction)*0.5f;
                const float mixed=output[i*channels_+ch]+sample*32768.0f;
                output[i*channels_+ch]=static_cast<int16_t>(std::clamp(mixed,-32768.0f,32767.0f));
            }
        }
    }
    if(recording_ && !recording_->failed) {
        auto& r=*recording_; const size_t end=r.write.load(), count=frames*channels_;
        if(count > r.ring.size()-(end-r.read.load())) { r.failed=true; return; }
        for(size_t i=0;i<count;++i) r.ring[(end+i)%r.ring.size()]=output[i];
        r.write.store(end+count);
    }
}
