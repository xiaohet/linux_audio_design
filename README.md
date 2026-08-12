# Linux Audio Design

This is a Linux-based project aiming for powerful audio applications on Raspberry Pi. 

The project provides `audio_project` for offline WAV processing,
`realtime_audio` for real-time USB audio playback, and
`deepfilter_benchmark` for native DeepFilterNet timing and listening tests.

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

### MCP3008 hardware gain control

The optional MCP3008 controller reads a 3.3 V-referenced ADC channel and maps
values 0 through 1023 to output gain from -60 through +12 dB. First verify the
SPI wiring without starting the audio system:

```bash
./build/mcp3008_test /dev/spidev0.0 0
```

CH0 connected through equal 10 kOhm resistors to 3.3 V and ground should read
approximately 512 (1.65 V). Ground should read approximately 0, and 3.3 V
approximately 1023. Never apply 5 V to an MCP3008 input connected to the Pi.

Enable CH0 hardware gain control by adding `--mcp3008` to the normal command:

```bash
./build/realtime_audio \
  --capture plughw:CARD=Device \
  --playback plughw:CARD=Device \
  --period 128 --buffer 512 \
  --mcp3008 --mcp3008-channel 0
```

The equal-resistor test point produces about -24 dB. The reading is smoothed
and small ADC changes are ignored to prevent audible gain jitter. A 10 kOhm
linear potentiometer can later replace the divider: connect its outer terminals
to 3.3 V and ground and its wiper to CH0. While MCP3008 control is enabled it is
the authoritative gain source, so browser or terminal gain changes are replaced
by the next changed hardware reading. The range can be customized with
`--hardware-gain-min DB` and `--hardware-gain-max DB`; another SPI chip-select
can be selected with `--mcp3008-device /dev/spidev0.1`.

While audio is streaming, type commands in the same terminal and press Enter:

```text
gain -20
eq 80 3
eq 640 -4
eq 5120 2.5
mix 50
comp -18 4 10 100 3
status
noise 60
dfstats
```

`gain` sets output gain in decibels, matching the browser control; for example, `gain -6` reduces the level by approximately half and `gain 0` is unity gain. Use `mute` as a diagnostic: if audio is still audible after muting, the USB interface's hardware direct-monitor path is enabled and is bypassing this program.

The real-time graphic EQ uses seven peaking biquads centered at 80, 160, 320, 640, 1280, 2560, and 5120 Hz. Each band has a fixed Q of 1.4 and an independently adjustable gain from -18 to +18 dB. Use `eqoff` to reset every band to 0 dB. The stereo-linked compressor follows the EQ and provides threshold, ratio, attack, release, and makeup-gain controls; its default 1:1 ratio leaves the signal unchanged. The `mix` command accepts a wet percentage from 0 (fully dry) to 100 (fully effected). Output gain is applied after this mix, so it controls both paths equally. Type `help` to display all real-time commands, or `quit` to stop the program. Updates take effect on the next audio period without restarting the ALSA stream.

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

The compact top row places input routing, output gain, dry/wet, and noise suppression on the left, with a separate single-column compressor card on its right. Seven compact graphic-EQ faders sit below, with a smaller vertical output peak meter on the far right. A live gain-reduction readout shows when the compressor is working. The dry/wet slider continuously blends the routed dry signal with the gate, EQ, and compressor processed signal. Terminal controls continue to work at the same time. Use another port with `--web-port PORT`, or disable the web interface with `--web-port 0`.

The Noise suppression slider is independent of the effects dry/wet slider. Zero is
off; higher values blend in more DeepFilterNet output. DeepFilterNet is a speech
enhancement model, so start around 10–30% for guitar and 60–100% for noisy voice.

If `raspberrypi.local` does not resolve, use the address printed by `hostname -I`, for example `http://192.168.1.50:8080`. The control page is available to devices on the local network and does not include authentication, so use it only on a trusted network.

### Real-time DeepFilterNet integration

The real-time program optionally loads the DeepFilterNet v0.5.6 C API at runtime.
When this repository contains the tested DeepFilterNet checkout, the default paths
are:

```text
DeepFilterNet/target/aarch64-unknown-linux-gnu/release/libdeepfilter.so
DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz
```

Run `realtime_audio` from the repository root so those relative paths resolve:

```bash
cd "$HOME/linux_audio_design_pi"
./build/realtime_audio
```

If they are not present there, the program also checks the same paths below
`$HOME/DeepFilterNet` automatically.

If DeepFilterNet lives elsewhere, specify both paths:

```bash
./build/realtime_audio \
  --deepfilter-library "$HOME/DeepFilterNet/target/aarch64-unknown-linux-gnu/release/libdeepfilter.so" \
  --deepfilter-model "$HOME/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz" \
  --noise-suppression 0
```

The model is mono and processes 480-sample frames. A worker thread and lock-free
sample FIFOs adapt that frame size to ALSA periods without waiting in the audio
thread. Four model frames are reserved for scheduling variation. The dry path is
delayed by the model's 1440-sample STFT/lookahead delay plus this scheduling
reserve before dry/wet suppression mixing. At 48 kHz this is about 70 ms total.
The suppression result is duplicated to both playback channels; enabling it while
using `route stereo` therefore intentionally produces dual mono.

The model stays warm even when suppression is at 0%, allowing click-free 40 ms
control smoothing and immediate use. If the model or library is absent, the audio
program still starts with suppression unavailable. Use these terminal commands:

```text
noise 0
noise 60
dfstats
```

`dfstats` reports inference mean/maximum time, model deadline misses, input FIFO
overruns, processed-output fallback periods, and stale samples discarded during
timeline recovery. An isolated model deadline miss may be absorbed by the
scheduling reserve. A fallback period means processed audio was unavailable and
latency-aligned dry audio was used instead. Sequence tags prevent late processed
samples from being mixed with current dry samples after a long scheduling stall.
Keep the attenuation limit conservative for non-speech material:

```bash
./build/realtime_audio --deepfilter-atten-limit 20
```

The default model-delay value is specific to the tested regular DeepFilterNet3
configuration (`fft_size=960`, `hop_size=480`, `lookahead=2`). Override it only
when using a model with different settings:

```bash
./build/realtime_audio --deepfilter-delay SAMPLES
```

The browser shows a compact live DeepFilter status below the suppression slider.
For monitoring without VS Code, leave the application in `tmux` and query its
HTTP state from an ordinary SSH session:

```bash
sudo apt install -y tmux jq
tmux new -s audio
./build/realtime_audio
# Detach with Ctrl+B, then D.
```

In another SSH connection:

```bash
watch -n 2 'curl -s http://127.0.0.1:8080/api/state | jq {
  mean_ms:.deepFilterMeanMs,
  max_ms:.deepFilterMaximumMs,
  late:.deepFilterDeadlineMisses,
  fallback_periods:.deepFilterOutputUnderruns,
  stale_samples:.deepFilterStaleOutputSamples
}'
```

### DeepFilterNet standalone benchmark

`deepfilter_benchmark` measures the native DeepFilterNet inference runtime without ALSA. DeepFilterNet is loaded at runtime, so the rest of this project still builds and runs when DeepFilterNet is not installed. The benchmark currently uses the project's 48 kHz mono processing plan; stereo WAV input is downmixed to mono.

Build the tested DeepFilterNet v0.5.6 native C API on 64-bit Raspberry Pi OS.
This revision uses `cargo-c` to produce the shared library:

```bash
sudo apt update
sudo apt install -y git build-essential cmake pkg-config clang libssl-dev
rustc --version
cd "$HOME"
git clone https://github.com/Rikorose/DeepFilterNet.git
cd DeepFilterNet
git checkout v0.5.6
cargo update -p time@0.3.28 --precise 0.3.36
cargo install cargo-c --version '=0.10.11+cargo-0.86.0' --locked
cargo cbuild --release -p deep_filter --features capi
```

The tested toolchain is Rust 1.85. If the packaged Rust is older, install Rust
with `rustup`, reopen the terminal (or source `$HOME/.cargo/env`), and repeat
the build. The pinned `cargo-c` version avoids requiring a newer compiler.

This creates
`target/aarch64-unknown-linux-gnu/release/libdeepfilter.so`. Use the regular
`DeepFilterNet3_onnx.tar.gz` model. The tested DeepFilterNet2 archive triggers
unsupported Tract graph operations on this platform.

Build this project after pulling the benchmark changes:

```bash
cd "$HOME/linux_audio_design_pi"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/deepfilter_benchmark --help
```

First run a short functional test using the benchmark's deterministic synthetic tone-and-noise input:

```bash
./build/deepfilter_benchmark \
  --library "$HOME/DeepFilterNet/target/aarch64-unknown-linux-gnu/release/libdeepfilter.so" \
  --model "$HOME/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz" \
  --warmup 5 --duration 30 \
  --csv deepfilter_short.csv
```

For the meaningful performance test, avoid writing enhanced audio because a long output recording consumes memory and adds copying work. Use a 30-second warm-up followed by ten measured minutes:

```bash
vcgencmd measure_temp
vcgencmd get_throttled

/usr/bin/time -v ./build/deepfilter_benchmark \
  --library "$HOME/DeepFilterNet/target/aarch64-unknown-linux-gnu/release/libdeepfilter.so" \
  --model "$HOME/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz" \
  --warmup 30 --duration 600 \
  --csv deepfilter_10min.csv

vcgencmd measure_temp
vcgencmd get_throttled
```

The executable returns status `0` for a conservative pass, `2` when the measurements need review, and `1` for a setup/runtime error. A pass requires average RTF below 0.60, 99th-percentile processing below 80% of the model frame deadline, and zero deadline misses. The full report includes model initialization time, mean/median/p95/p99/maximum frame times, RTF, deadline misses, process CPU use, peak resident memory, and Pi temperature when the Linux thermal sensor is available. `vcgencmd get_throttled` should report `0x0`; otherwise fix power or cooling before trusting the result.

For a listening test, prepare a conventional 48 kHz, 16-bit PCM WAV containing voice, guitar, or recorded noise. Keep this run short and write the enhanced mono result:

```bash
./build/deepfilter_benchmark \
  --library "$HOME/DeepFilterNet/target/aarch64-unknown-linux-gnu/release/libdeepfilter.so" \
  --model "$HOME/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz" \
  --input test_voice_or_guitar.wav \
  --warmup 2 --duration 30 \
  --output deepfilter_enhanced.wav \
  --csv deepfilter_listening.csv
```

Listen specifically for removed guitar sustain or harmonics, watery artifacts,
damaged pick transients, and changes to voice intelligibility. Repeat the long
performance test at least three times, including once after the Pi has warmed
up. Use it as a baseline before enabling the model in `realtime_audio`;
inference-only success does not prove that USB audio plus the web interface will
remain underrun-free.

### Real-time source layout

The real-time application is divided by responsibility:

- `src/realtime_audio.cpp`: application startup and capture/process/playback loop
- `src/audio_config.cpp`: command-line options and ALSA device discovery
- `src/pcm_device.cpp`: ALSA configuration, status, and xrun recovery
- `src/deepfilter_processor.cpp`: dynamic DeepFilterNet loading, worker, FIFOs, aligned suppression mix, and diagnostics
- `src/realtime_processor.cpp`: routing, gain, gate, seven-band EQ, effects dry/wet mixing, and meters
- `src/command_interface.cpp`: interactive terminal commands
- `src/web_control_server.cpp`: HTTP API and embedded webpage
- `src/deepfilter_benchmark.cpp`: standalone native DeepFilterNet performance and listening benchmark

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
