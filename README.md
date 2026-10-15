# tempo v2.1

Code for: Ajaikrishna V. A. and Rathnakannan K., "Bounded Temporal Alignment for Free-Running Multi-Camera Embedded Vision Systems", submitted to *IEEE Embedded Systems Letters*.

Data: https://doi.org/10.5281/zenodo.23388763

| Path | Contents |
|---|---|
| `src/tempo.c`, `src/tempo.h` | Library (C99, freestanding, no heap, no floating point) |
| `src/test_tempo.c` | Self-tests |
| `src/bench_truth.c` | Synthetic ground-truth benchmark |
| `src/tempo_replay.c` | Offline replay of recorded timestamps |
| `src/tempo_sync.c`, `src/v4l2cap.c`, `src/v4l2cap.h` | V4L2 capture with INDEX / NEAREST / PREDICTED grouping |
| `src/ledcap.c` | V4L2 capture with LED brightness sampling |
| `src/nvmux_probe.c` | DeepStream `nvstreammux` measurement |
| `tools/ledtool.py` | LED location and Gray-code decoding |
| `arduino/` | LED Gray-code counter firmware (250 µs and 1 ms steps) |
| `capture_orin.sh`, `capture_x86_led.sh` | Capture scripts used for the measurements |
| `analysis/` | Scripts that compute the paper's figures and numbers from the data archive |

## Build

```bash
cd src
make
make test
make bench
```

`nvmux_probe` requires DeepStream: `make nvmux_probe`.

## Reproduce

```bash
export ESL_DATA=/path/to/ESL_tempo_data
cd analysis && ./run_all.sh
```

Requires Python 3 with numpy, scipy and matplotlib.

## Benchmark (`make bench`)

4 cameras, offsets {0, +5, −8, +11} ppm, 1 h, T = 10 ms. Spread of true capture instants of the selected frames, mean / max (ms):

| Arrival jitter | NEAREST | PREDICTED | PREDICTED + latency calibration |
|---|---|---|---|
| unequal latencies, 0 µs | 8.08 / 15.36 | 8.08 / 15.36 | 6.12 / 10.00 |
| unequal latencies, 2000 µs | 8.16 / 18.87 | 8.01 / 15.91 | 6.43 / 10.14 |
| equal latencies, 0 µs | 6.12 / 9.99 | 6.12 / 9.99 | 6.12 / 9.99 |
| equal latencies, 2000 µs | 6.58 / 12.43 | 6.64 / 10.12 | 6.64 / 10.12 |

## Footprint

| | |
|---|---|
| `tempo.o` | 4944 B (aarch64, GCC 9) |
| Static RAM | 0 |
| `tempo_clock` / `tempo_aligner` | 904 B / 160 B |
| Ring buffers, window 512 | 6 KiB per camera |
| CPU, Jetson AGX Orin, 4 × 100 fps | 0.75 µs per frame (association); 1.33 % of one core (capture program) |

## Versions

v2.0 was used for the measurements. v2.1 removes a 64-bit overflow after about 26 days of continuous capture; on the recorded data both versions select identical frames.

## Licence

MIT (`LICENSE`). Citation: `CITATION.cff`.
