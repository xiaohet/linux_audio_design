# Linux Audio Design

This is a Linux-based project aiming for powerful audio applications on Raspberry Pi. 

So far, there are two executables in this project: `audio_project` for offline WAV-file processing, and `realtime_audio` for real-time USB audio playback via Raspberry Pi.

Both executables provide gain processing. `realtime_audio` adds a fully
parametric peaking EQ, while the offline `audio_project` retains its low-pass
and high-pass filters.

In `audio_project`, an audio wav file reading function and an audio wav file writing function are also used.

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

Use a stable `CARD=` name from `/proc/asound/cards` instead of a numeric card index, which can change after reboot. The seven-band EQ starts flat. Start with the interface's direct-monitor control off, or you will hear both the dry and processed signals.

At 48 kHz, a 128-frame period is about 2.7 ms. If ALSA reports overruns or underruns, try `--period 256 --buffer 1024`. For the lowest latency, disable Wi-Fi/Bluetooth if unused, select the Performance CPU governor, avoid USB hubs, and consider a Raspberry Pi real-time kernel after the basic setup is stable.

Press Ctrl+C to stop. Run `./build/realtime_audio --help` for all options.

While audio is streaming, type commands in the same terminal and press Enter:

```text
gain 0.6
gaindb -20
eq 80 3
eq 640 -4
eq 5120 2.5
mix 50
status
```

`gain` is a linear amplitude multiplier: `0.5` is about -6 dB, `0.1` is -20 dB, and `0` is silence. The `gaindb` command is often more intuitive for volume control. Use `mute` as a diagnostic: if audio is still audible after muting, the USB interface's hardware direct-monitor path is enabled and is bypassing this program.

The real-time graphic EQ uses seven peaking biquads centered at 80, 160, 320, 640, 1280, 2560, and 5120 Hz. Each band has a fixed Q of 1.4 and an independently adjustable gain from -18 to +18 dB. Use `eqoff` to reset every band to 0 dB. The `mix` command accepts a wet percentage from 0 (fully dry) to 100 (fully effected). Output gain is applied after this mix, so it controls both paths equally. Type `help` to display all real-time commands, or `quit` to stop the program. Updates take effect on the next audio period without restarting the ALSA stream.

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

### Browser control panel

The real-time executable includes a responsive browser interface—no separate web server or JavaScript runtime is required. Start the audio engine normally, then open the following address from a phone or computer on the same network:

```text
http://raspberrypi.local:8080
```

The page places input routing, horizontal output-gain and dry/wet controls at the upper left, with seven compact graphic-EQ faders below and a vertical output peak meter on the right. The dry/wet slider continuously blends the routed dry signal with the gate-and-EQ processed signal. Terminal controls continue to work at the same time. Use another port with `--web-port PORT`, or disable the web interface with `--web-port 0`.

If `raspberrypi.local` does not resolve, use the address printed by `hostname -I`, for example `http://192.168.1.50:8080`. The control page is available to devices on the local network and does not include authentication, so use it only on a trusted network.

### Real-time source layout

The real-time application is divided by responsibility:

- `src/realtime_audio.cpp`: application startup and capture/process/playback loop
- `src/audio_config.cpp`: command-line options and ALSA device discovery
- `src/pcm_device.cpp`: ALSA configuration, status, and xrun recovery
- `src/realtime_processor.cpp`: routing, gain, gate, seven-band EQ, dry/wet mixing, and meters
- `src/command_interface.cpp`: interactive terminal commands
- `src/web_control_server.cpp`: HTTP API and embedded webpage

Each module's public classes and structures are declared in the matching header under `include/`.

### Diagnosing noise from a disconnected or sleeping phone

Use the terminal command `levels` while testing the cable disconnected, the phone screen off, and the phone screen on. It reports the peak captured on each Scarlett input before routing or DSP, followed by the processed output peak. If the affected input level rises when the Lightning adapter sleeps, the noise is entering through the analog input rather than being generated by the filters.

The input noise gate defaults to `-55` dBFS. Set its threshold just above the measured idle-noise level:

```text
levels
gate -45
```

For example, if the unwanted input measures around `-50` dBFS, begin with `gate -45`. Use `gate off` to disable it. A higher threshold suppresses more noise but can also mute quiet music, so hardware correction is preferable.

For a phone source, use the Scarlett input in line mode with Inst disabled and keep the hardware input gain only as high as required. Do not connect a stereo phone output directly to a balanced mono input with a simple TRS-to-TRS adapter; use a stereo breakout, a properly resistor-summed mono cable, or a stereo DI/isolator intended for this connection.
