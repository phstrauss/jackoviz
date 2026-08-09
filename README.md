# jackoviz

Scientific visualization of audio signals.
Realtime mono audio → Kaiser-windowed FFT → 3D Datoviz spectrogram.

Screenshots (Suzanne Vega singing Tom's Diner):

![2D STFT](./screenshots/stft-2d.png "2D short-time FFT")
![2D Spectrogram](./screenshots/spectrogram-2d.png "2D spectrogram")
![3D Spectrogram](./screenshots/spectrogram-3d.png "3D spectrogram")

## Pipeline

1. **JACK** process callback writes one input channel into a lock-free `jvz_jack_ringbuffer`.
2. On each Datoviz frame, read a FFT sized number of samples from our jvz_jack_ringbuffer, then process a **Kaiser-windowed** real FFT with **FFTW3**.
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
First, clone and build [Datoviz](https://datoviz.org/) v0.4.0, then :

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
./jackoviz -f 4000                 # set the maximum plotted frequency to 4kHz
./jackoviz -s system:capture_1     # auto-connect capture
./jackoviz -n 8192 -b 6.0          # longer FFT, sharper Kaiser window
./jackoviz --frames 120            # smoke: exit after 120 frames
./jackoviz --fast                  # scope + 1D spectrum only (lighter CPU)
./jackoviz --fast -s system:capture_1
```

Press key **0** for a basic scope, **1** for a real-time STFT frequency–magnitude analyzer, **2** for a spectrogram, **3** for a 3D spectrogram. Look at the top of the C file to adjust some constants to your taste, rebuild afterwards.

You can adjust the plot floor and ceiling by hitting the **f** and **c** keys, respectively.

**`--fast`** skips the 2D and 3D spectrogram panels and their per-frame uploads (`upload_spectrogram`), keeping only the oscilloscope and 1D spectrum views. Keys **2** and **3** do nothing in this mode; the app starts on the 1D spectrum.

Drag in the window to orbit the surface (arcball). Close the window to quit.

The time resolution is, using our customized jack ringbuffer, finer when using a small (64, 128 or 256 samples) jack audio buffer depth.

## Layout of the spectrogram ringbuffer

- Capacity: 256 time columns (power of two), each with `fft_size/2+1` bins.
- Backing store: one shared-memory region of `capacity × bins × sizeof(double)` bytes, **mapped twice** into a contiguous `2×` virtual range.
- Pushing a column writes at `write_col % capacity`; reading the last *H* columns uses the mirror so the view never needs a wrap copy.
