# TODO/Roadmap

## 2026-08-12, jackoviz v0.9.0

Two main bugs fixed to the extent of what can be achieved with the current Datoviz (0.4.0-rc2,
see Datoviz author's [proposal for the future of mesh-grid fields](https://github.com/datoviz/datoviz/blob/v0.4-dev/spec/scene/proposals/future/SAMPLED_FIELD_VIEWS_AND_INCREMENTAL_UPDATES.md))

### Before jackoviz v1.0.0

- Single instance app (singleton app) on the whole host (ADC, jack server, vulkan use).
- Add an "About" dialog in the GUI, with single version string shared by jackoviz.c and remote ctrl.
- MacOS app bundle including everything, Qt related AND what is needed from Homebrew.

### For the future, towards v2.0.0

Just some rough ideas :

- Use the new Datoviz code as outlined from the URL above, landing in between Datoviz v0.4.0-rc4 and v0.5.0.
- Host the Datoviz scene in the Datoviz Qt adapter if Qt itself fit for a high update rate app. GTK just ok,
  fine when the mouse pointer outside of the widgets scope, borderline otherwise. Qt unknown to me
  in this area. For documentation, see $DATOVIZ_ROOT/spec/scene/integration/QT_HOSTING.md and
  $DATOVIZ_ROOT/examples/qt/qt_hosting.cpp
- Performance profiling.
- True stereo (one FFT per channel, sum the magnitude).
- Some basic form of formant analysis by akima interpolation of the magnitude peaks leading to a spectral enveloppe?
- LPC formant analysis?
- Act from file, give a "Play" option?
- Hilbert-Huang Transform (spectrum), only from files, not real-time?
