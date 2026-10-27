#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Disk writes run on a worker. Audio methods only copy/mix preallocated memory.
class AudioTransport {
public:
    struct Settings {
        bool recordInput = false;
        bool playbackEffects = false;
        std::filesystem::path directory;
    };
    struct State {
        bool recording; bool playing; std::string file; std::string error;
        Settings settings;
        bool activeRecordInput;
        bool activePlaybackEffects;
        std::string activeDirectory;
    };
    struct Directories {
        std::string directory, parent;
        std::vector<std::string> children;
    };
    AudioTransport(unsigned rate, unsigned channels, std::filesystem::path directory);
    ~AudioTransport();
    std::string record();
    std::string stop_recording();
    void play(const std::string& name);
    void stop_playback();
    std::vector<std::string> files() const;
    State state() const;
    void set_settings(const Settings& settings);
    Directories directories(const std::string& path) const;
    // Call in order each audio period: render, DSP (with effects buffer), capture.
    void render_playback(int16_t* effects, int16_t* direct, size_t frames);
    void capture_recording(const int16_t* input, const int16_t* output, size_t frames);
private:
    struct Recording;
    struct Playback;
    unsigned rate_, channels_;
    Settings settings_;
    const std::filesystem::path projectDirectory_;
    // Commands serialize independently of the short audio-thread critical section.
    mutable std::mutex commands_, mutex_;
    std::unique_ptr<Recording> recording_;
    std::unique_ptr<Playback> playback_;
    std::string lastFile_, error_;
};
