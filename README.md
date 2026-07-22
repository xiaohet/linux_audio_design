# linux_audio_design

This is a Linux-based project aiming for powerful audio applications on Raspberry Pi. 

So far, these functions have been implemented: 
- an audio wav file reading function;
- an audio wav file writing function;
- a gain function;
- a lowpass filter;
- a highpass filter.

## Raspberry Pi 4 real-time USB audio

Connect the phone's analog output to the USB interface input, and connect headphones or speakers to the interface output. On Raspberry Pi OS (64-bit recommended), install the build dependencies and compile:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libasound2-dev alsa-utils
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Find the USB interface's ALSA PCM name:

```bash
./build/realtime_audio --list-devices
arecord -l
aplay -l
```

If the interface appears as card `Device`, start a low-latency stereo pass-through at 48 kHz:

```bash
./build/realtime_audio \
  --capture plughw:CARD=Device \
  --playback plughw:CARD=Device \
  --rate 48000 --channels 2 \
  --period 128 --buffer 512 --gain 0.8
```

Use a stable `CARD=` name from `/proc/asound/cards` instead of a numeric card index, which can change after reboot. Add `--highpass 80` and/or `--lowpass 12000` to enable the real-time filters. Start with the interface's direct-monitor control off, or you will hear both the dry and processed signals.

At 48 kHz, a 128-frame period is about 2.7 ms. If ALSA reports overruns or underruns, try `--period 256 --buffer 1024`. For the lowest latency, disable Wi-Fi/Bluetooth if unused, select the Performance CPU governor, avoid USB hubs, and consider a Raspberry Pi real-time kernel after the basic setup is stable.

Press Ctrl+C to stop. Run `./build/realtime_audio --help` for all options.

The original `audio_project` executable remains available for offline WAV-file processing.
