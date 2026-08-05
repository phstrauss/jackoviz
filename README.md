# jackoviz

Realtime mono audio → Kaiser-windowed FFT → 3D Datoviz surface spectrogram.

## Pipeline

1. **JACK** process callback writes one input channel into a lock-free `jack_ringbuffer`.
2. On each Datoviz frame, when at least **2048** new samples are available, a **Kaiser-windowed** real FFT is run with **FFTW3**.
3. Magnitude spectra (dB, normalized) are appended as columns into a **bidimensional doubly-mapped** ringbuffer (time × frequency), so a wrapping history can be read as one contiguous block.
4. That history drives a Datoviz **`dvz_geometry_surface_grid`** mesh, updated every frame.

## Requirements

| Dependency | Notes |
|---|---|
| JACK | `pkg-config jack` — jackd must be running |
| FFTW3 | `pkg-config fftw3` |
| Datoviz ≥ 0.4 | Set `DATOVIZ_ROOT` to a built checkout, or use `datoviz-config` |
| POSIX | macOS / Linux (`shm_open` + mirrored `mmap`) |

## Build

```bash
# default: DATOVIZ_ROOT=$HOME/work/datoviz
make

# or
make DATOVIZ_ROOT=/path/to/datoviz
```

## Run

```bash
# jackd must already be running
./jackoviz                         # connect manually in QjackCtl / jack_connect
./jackoviz -s system:capture_1     # auto-connect capture
./jackoviz -n 8192 -b 10.0         # longer FFT, sharper Kaiser window
./jackoviz --frames 120            # smoke: exit after 120 frames
```

Drag in the window to orbit the surface (arcball). Close the window to quit.

## Layout of the spectrogram ringbuffer

- Capacity: 128 time columns (power of two), each with `fft_size/2+1` bins.
- Backing store: one shared-memory region of `capacity × bins × sizeof(double)` bytes, **mapped twice** into a contiguous `2×` virtual range.
- Pushing a column writes at `write_col % capacity`; reading the last *H* columns uses the mirror so the view never needs a wrap copy.
