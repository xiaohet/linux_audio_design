#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Disk writes run on a worker. process() only copies/mixes preallocated memory.
class AudioTransport {
public:
    struct State { bool recording; bool playing; std::string file; std::string error; };
    AudioTransport(unsigned rate, unsigned channels, std::filesystem::path directory);
    ~AudioTransport();
    std::string record();
    std::string stop_recording();
    void play(const std::string& name);
    void stop_playback();
    std::vector<std::string> files() const;
    State state() const;
    void process(int16_t* output, size_t frames);
private:
    struct Recording;
    struct Playback;
    unsigned rate_, channels_;
    std::filesystem::path directory_;
    // Commands serialize independently of the short audio-thread critical section.
    mutable std::mutex commands_, mutex_;
    std::unique_ptr<Recording> recording_;
    std::unique_ptr<Playback> playback_;
    std::string lastFile_, error_;
};
