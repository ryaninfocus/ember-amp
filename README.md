# Ember Amp

A guitar amp for Mac and Windows: a standalone app plus VST3 and AU plugins.

## Download

**[Get the latest version from the Releases page](../../releases/latest)**. There's no account or sign-in needed.

| Your computer | Download |
|---|---|
| Mac (Apple Silicon or Intel, macOS 11+) | `Ember-Amp-…-Mac.dmg` |
| Windows (64-bit Windows 10/11) | `Ember-Amp-…-Windows-Setup.exe` |

The first time you open it, your computer will ask once, because Ember Amp isn't registered with Apple or Microsoft:
- **Mac:** go to **System Settings → Privacy & Security** and click **Open Anyway**.
- **Windows:** click **More info → Run anyway**.

## What's inside
- **4 amps**, each with its own cab and 5 presets:
  - **Clean:** Normal Clean, Glass Clean, Ambient Swell, Warm Jazz, Chorus Clean
  - **Crunch:** Classic Crunch, Blues Breaker, British Roar, Edge of Breakup, Rock Rhythm
  - **Distortion:** Prog Metal Rhythm, Djent Tight, Drop C Chug, Octave Doom, Texas Special
  - **Solo:** Singing Lead, Shred, Liquid Legato, Harmony Fifth, Texas Scoop Lead
- **8 pedals:** gate, compressor, overdrive, distortion, pitch shifter, chorus, delay and reverb.
- **Stream to Discord:** the STREAM switch sends the amp (plus your mic) to a free virtual cable (BlackHole on Mac, VB-Audio Cable on Windows), so you can play in Discord, OBS or Zoom without sharing your screen.
- **Other features:** a tuner, IR loading (drag a .wav onto the window), saving your own presets, an optional Dynamics switch, and loudness-matched presets.

## Build it yourself
You need CMake 3.22+ and either the Xcode Command Line Tools (Mac) or Visual Studio 2022 (Windows). JUCE is downloaded automatically.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The results land in `build/EmberAmp_artefacts/Release/`. To build the installers, use `packaging/mac/make_installer.sh` on Mac or `packaging/windows/EmberAmp.iss` (Inno Setup) on Windows.

Releases are built automatically by GitHub Actions (`.github/workflows/build.yml`) whenever a `v*` tag is pushed.
