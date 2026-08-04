# Prompts

User prompts from the jackoviz session, in order.

## 1

In jackoviz.c, write a C99 program which captures one channel of audio data in realtime using jack audio connection kit, stuff each frame in a first audio ringbuffer, read the ringbuffer on each 2048 or 8192 new samples, do a kaiser windowed FFT using fftw3 and store the result in a second, bidimensional doubly-maped large ringbuffer. Plot this second ringbuffer content using Datoviz surface plot in 3D. Target POSIX compliant OSes, MacOS and Linux.

## 2

Now after the FFT, before storing in the second ringbuffer, compute the decibels of the magnitude of the spectrum, 20*log10 of the magnitude or 10*log10 of the power spectra

## 3

And use this instead of the plain magnitude for plotting

## 4

Now display only the frequencies between 0 and 8000 Hz, forget the rest just before plotting, adapt to the sampling frequency which may change between two runs of this program.

## 5

Great! Now the plot appear in a single color, look if you can use a virdis level-related colouring of the surface and lit it from above with a slightly reflective surface.

## 6

Great! Record all my prompts in PROMPT.md
