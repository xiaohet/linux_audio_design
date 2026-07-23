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

While audio is streaming, type commands in the same terminal and press Enter:

```text
gain 0.6
gaindb -20
highpass 80
lowpass 12000
status
```

`gain` is a linear amplitude multiplier: `0.5` is about -6 dB, `0.1` is -20 dB, and `0` is silence. The `gaindb` command is often more intuitive for volume control. Use `mute` as a diagnostic: if audio is still audible after muting, the USB interface's hardware direct-monitor path is enabled and is bypassing this program.

The real-time low-pass and high-pass effects are fourth-order Butterworth filters (24 dB/octave), making cutoff changes substantially more audible than the original first-order filters. Use `highpass 0` or `lowpass 0` to disable that filter. Type `help` to display all real-time commands, or `quit` to stop the program. Updates take effect on the next audio period without restarting the ALSA stream.

### Scarlett Solo input routing

The Scarlett Solo exposes its two physical inputs as the two channels of a stereo capture stream. A mono source connected to input 2 therefore appears only on the right channel unless it is routed to both outputs. The default routing is `input2`, which duplicates capture input 2 to the left and right playback channels before applying gain and filters.

Change routing while streaming with:

```text
route input1
route input2
route stereo
route mix
```

`stereo` preserves the two capture channels independently. `mix` averages both inputs and sends that mono mix to both outputs. The startup equivalent is `--routing MODE`.

### Diagnosing ALSA recovery

The playback stream is prefilled to several periods before it starts and after an underrun recovery. This makes the configured buffer an actual scheduling reserve rather than starting playback with only one period queued.

Enable rate-limited ALSA diagnostics when investigating a problem:

```bash
./build/realtime_audio --diagnostics
```

The startup output shows the period and buffer sizes ALSA actually negotiated; these can differ from the requested values. During streaming, enter `stats` to print capture/playback state, available frames, delay, and recovery counts. Avoid printing on every audio-loop iteration because terminal I/O can itself cause underruns.

The original `audio_project` executable remains available for offline WAV-file processing.
