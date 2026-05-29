# VSTCompare

VSTCompare is a Windows-only C++17 command-line tool for comparing two VST3 plug-ins. It hosts both plug-ins, generates deterministic test signals internally, measures their audio behavior, and writes a self-contained HTML report with embedded JSON data.

The project is intended for objective A/B comparison of plug-in character: frequency response, phase, transient behavior, distortion, stereo width, pitch tracking, dynamics, and time response. It does not require external audio files, and the report can be read by humans or by tools that consume the embedded structured data.

Japanese documentation is available in [README-Japanese.md](README-Japanese.md).

## Status

This currently supports Windows only. `CMakeLists.txt` rejects non-Windows builds.

## Prerequisites

- Windows
- CMake 3.25 or later
- MSVC / Visual Studio
- VST3 SDK at `external/vst3sdk`

If the VST3 SDK is stored somewhere else, pass `VST3_SDK_ROOT` when configuring CMake.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

To use a VST3 SDK outside this repository:

```powershell
cmake -S . -B build -DVST3_SDK_ROOT="C:\path\to\vst3sdk"
cmake --build build --config Release
```

The release executable is expected at:

```text
build\bin\Release\vstcompare_cli.exe
```

## Run

```powershell
build\bin\Release\vstcompare_cli.exe --plugin-a "C:\Path\To\PluginA.vst3" --plugin-b "C:\Path\To\PluginB.vst3" --out reports --non-interactive
```

If `--out` points to a directory, VSTCompare creates the directory if needed and writes:

```text
test_report_<PluginA>_vs_<PluginB>.html
```

If `--out` points to a `.html` file, that exact file path is used.

Without `--non-interactive`, the CLI prompts for each published plug-in parameter when standard input is interactive. Leaving a parameter blank skips it.

## CLI Options

| Option | Required | Description |
| --- | --- | --- |
| `--plugin-a <path>` | Yes | Path to the first `.vst3` plug-in. |
| `--plugin-b <path>` | Yes | Path to the second `.vst3` plug-in. |
| `--out <dir-or-file>` | Yes | Output directory or `.html` report file. |
| `--plugin-a-class-id <32hex>` | No | Select a specific VST3 class ID for plug-in A. |
| `--plugin-b-class-id <32hex>` | No | Select a specific VST3 class ID for plug-in B. |
| `--sample-rate <value>` | No | Processing sample rate. Defaults to `48000`. |
| `--block-size <value>` | No | Processing block size. Defaults to `512`. |
| `--non-interactive` | No | Skip parameter prompts and run with default/current plug-in parameters. |
| `--help`, `-h` | No | Print usage text. |

## Test Specifications

VSTCompare runs a fixed set of internally generated tests. The default processing setup is `48000` Hz and block size `512`; both can be changed from the CLI. Most tests convert stereo output to a mono L/R average for analysis, while the mono-to-stereo width test preserves L/R channels. Each test records reported plug-in latency, applied alignment delay, clamped latency, and warnings when alignment or analysis quality is degraded.

### Impulse Response

The impulse response test exposes transient behavior, ringing, tail energy, and residual latency differences.

- Input signal: mono digital impulse, `500 ms`, first sample at amplitude `1.0`, all remaining samples silent.
- Analysis: both plug-ins process the same impulse; output is converted to mono for analysis and aligned using each plug-in's reported latency. VSTCompare computes the A-minus-B delta waveform, ranks the top 5 absolute sample differences, and estimates residual latency from the aligned output peak positions.
- Report content: input/output/delta waveforms, display-aligned channel waveforms, `peakAbsDelta`, `energyDelta`, `estimatedLatencyMs`, plug-in latency samples, alignment delay, and latency clamp warnings.

### Frequency Response

The frequency response test measures tonal balance and broad EQ-style differences.

- Input signal: deterministic mono white noise, `2000 ms`, `-12 dBFS` RMS, fixed seed.
- Analysis: averaged FFT power spectra are computed with FFT size `65536`, Hann windowing, and `50%` overlap. Output spectra are normalized against the input spectrum, then compared as A-minus-B in dB. The delta spectrum is summarized into 30 one-third-octave-style bands from `20 Hz` to `20 kHz`.
- Report content: input spectrum, normalized spectra for A and B, delta spectrum, one-third-octave band table, `peakAbsDeltaDb`, `meanAbsDeltaDb`, estimated residual latency, and alignment warnings.

### Phase Response

The phase response test measures phase rotation and frequency-dependent timing differences.

- Input signal: deterministic mono white noise, `2000 ms`, `-12 dBFS` RMS, fixed seed.
- Analysis: VSTCompare computes transfer phase from the input and output cross-spectrum with FFT size `65536`, Hann windowing, and `50%` overlap. Phase values are wrapped to `-pi` through `+pi`, then A-minus-B phase delta is calculated and summarized into 24 logarithmic bands from `20 Hz` to `20 kHz`.
- Report content: phase curves for A and B, phase delta curve, 24-band summary, `peakAbsDeltaRad`, `meanAbsDeltaRad`, estimated residual latency, and alignment warnings.

### Harmonic Distortion

The harmonic distortion test measures harmonic character from fixed sine tones.

- Input signal: mono sine waves at `100 Hz`, `1 kHz`, `5 kHz`, and `10 kHz`; each is `1000 ms` at `-6 dBFS`.
- Analysis: the first `200 ms` are skipped before analysis. A Blackman-Harris FFT with size `65536` is used to measure dBFS spectra. For each fundamental, VSTCompare extracts local peaks for harmonic orders 1 through 10, compares their amplitudes as A-minus-B, and estimates a noise floor after excluding harmonic neighborhoods.
- Report content: per-frequency spectra, harmonic order table, harmonic deltas, noise floor for A and B, `noiseFloorDeltaDb`, `peakAbsDeltaDb`, `meanAbsDeltaDb`, and per-frequency alignment warnings.

### THD / THD+N

The THD / THD+N test measures how distortion changes with input level and where a processor begins to saturate or clip.

- Input signal: mono `1 kHz` sine sweep from `-60 dBFS` to `+6 dBFS` in `2 dB` steps; each point is `1000 ms`.
- Analysis: the first `200 ms` of each point are skipped. A Blackman-Harris FFT is requested at size `65536` and reduced only if the available post-skip window is shorter. THD is calculated from harmonic orders 2 through 10 relative to the fundamental. THD+N rejects the fundamental band and compares remaining residual power to the fundamental band. The sweep is summarized into `low` (`-60` to `-30 dBFS`), `mid` (`-28` to `-10 dBFS`), `high` (`-8` to `0 dBFS`), and `overdrive` (`2` to `6 dBFS`) regions.
- Report content: THD and THD+N curves for A and B, percent deltas, segment summaries, `peakAbsThdDeltaPercent`, `meanAbsThdDeltaPercent`, `peakAbsThdnDeltaPercent`, `meanAbsThdnDeltaPercent`, and warnings for missing fundamentals, floor-pinned spectra, FFT reduction, or chart clipping.

### Mono-to-Stereo Width

The mono-to-stereo width test measures whether a plug-in widens, narrows, decorrelates, or otherwise changes a mono source.

- Input signal: stereo identical L/R pink noise, `500 ms`, `-12 dBFS` RMS, fixed seed.
- Analysis: stereo output is preserved. Mono plug-in outputs are duplicated to stereo for analysis, and outputs with more than 2 channels use only L/R. VSTCompare aligns the stereo outputs, computes mid and side signals, then measures mid/side ratio and width percentage over time using a `20 ms` window and `10 ms` hop. It also computes a frequency-domain mid/side ratio with FFT size `65536` and `50%` overlap, summarized into 24 logarithmic bands from `20 Hz` to `20 kHz`.
- Report content: time-series width, spectral width, 24-band summary, `peakAbsTimeDeltaDb`, `meanAbsTimeDeltaDb`, `peakAbsBandDeltaDb`, `meanAbsBandDeltaDb`, time and band width-percent deltas, channel-shape warnings, and alignment warnings.

### Pitch Tracking

The pitch tracking test measures how pitch-oriented processors respond to a controlled changing pitch.

- Input signal: mono logarithmic sine sweep from `110 Hz` to `880 Hz`, `5000 ms`, `-6 dBFS`, with `5 ms` fade in/out.
- Analysis: after latency alignment, pitch is estimated in `100 ms` frames every `10 ms` using rising zero-crossing intervals. VSTCompare compares each plug-in's estimated output pitch against the input reference and compares A against B. It records valid-frame rates so weakly tonal or noise-like output can be flagged.
- Report content: input and output pitch curves, `meanAbsErrorHzA`, `meanAbsErrorHzB`, `meanAbsDeltaHz`, `peakAbsDeltaHz`, `validFrameRateA`, `validFrameRateB`, estimated residual latency, and low-validity warnings.

### Dynamics

The dynamics test measures static level transfer, gain reduction, and compressor-like behavior.

- Input signal: mono `1 kHz` sine with stepped levels from `-90 dBFS` to `0 dBFS` in `1 dB` increments; each step lasts `50 ms`.
- Analysis: after alignment, VSTCompare measures a `20 ms` RMS window around the center of each step. It builds an input/output curve, computes gain reduction as input minus output, and estimates threshold, ratio, and knee width. Threshold detection starts where the smoothed I/O slope drops below `0.9`; ratio is estimated from the compressed region above threshold.
- Report content: I/O curve, gain reduction curve, output-level delta, gain-reduction delta, estimated threshold/ratio/knee for each plug-in, `thresholdDeltaDb`, `ratioDelta`, `kneeWidthDeltaDb`, peak/mean output and gain-reduction deltas, and fitting warnings.

### Time Response

The time response test measures burst latency, envelope speed, release behavior, and residual ringing.

- Input signal: mono `1 kHz` tone burst at `-6 dBFS`, with `200 ms` pre-silence, `100 ms` tone-on time, and `200 ms` post-silence.
- Analysis: after latency alignment, VSTCompare computes a `20 ms` RMS envelope. Attack is measured as the time from `10%` to `90%` of the peak envelope; release is measured from `90%` down to `10%`. Residual A/B latency is estimated with cross-correlation within `+/-50 ms`, and the envelope curve is sampled every `1 ms`.
- Report content: envelope curves, `residualLatencyMs`, attack and release times for A and B, `attackDeltaMs`, `releaseDeltaMs`, post-release residual levels and delta, and warnings when attack/release or residual windows are unstable.

## Reports

The report is a single HTML file with embedded CSS, charts, metrics, warnings, and JSON. It includes deterministic summaries for humans and LLM-oriented structured data; no external LLM or network service is required to generate the report.

## Tests

After building, run:

```powershell
ctest --test-dir build -C Release
```

## Repository Layout

- `src/`: CLI, pipeline, VST3 hosting, test managers, and report generation.
- `include/vstcompare/`: public project headers.
- `tests/`: CTest-based unit and behavior tests.
- `external/vst3sdk/`: Steinberg VST3 SDK dependency.
- `reports/`: example or generated report output.
- `vsts-for-dev-only/`: development-only plug-ins/binaries, not part of the project license grant.

## License

Original VSTCompare project code and documentation are released under Creative Commons Zero v1.0 Universal (CC0 1.0). See [LICENSE.md](LICENSE.md).

This CC0 grant applies only to original project code and documentation. The VST3 SDK under `external/vst3sdk` remains under the VST3 SDK MIT license; see `external/vst3sdk/LICENSE.txt`. Third-party SDK files, generated reports, and development-only VST plug-ins/binaries are not relicensed by this project.
