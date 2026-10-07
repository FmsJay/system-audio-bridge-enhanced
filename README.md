# System Audio Bridge (enhanced)

A Windows VST3 that captures system audio (YouTube, Spotify, a game, a call) into a REAPER track. It's a
clean-room reimplementation of the idea behind Bird's freeware *System Audio Bridge*, with two changes:

1. **Recorded takes play back.** With the transport playing (but not recording), the plugin passes
   the track's own audio through. The original keeps outputting live system audio, so after you
   recorded a take you heard silence instead of it. Set *During playback* to *Keep capturing* if you
   want the original behaviour.
2. **Send to the default device.** *Send output to default device* plays whatever reaches the plugin
   on the default Windows output. Put an instance on the master track with capture off and send on
   to hear the REAPER mix through your normal speakers or headphones, even while REAPER runs on an
   ASIO interface.

Capture uses Windows *process-loopback exclusion* (Windows 10 2004 or later). It records everything
Windows plays **except REAPER itself**, so sending the mix to the same device doesn't feed back. If
exclusion isn't available, it falls back to whole-device loopback and the status line warns you.
*Whole default device* forces that fallback mode.

## Install with ReaPack

*Extensions → ReaPack → Import repositories…* and paste:

```
https://github.com/FmsJay/system-audio-bridge-enhanced/raw/main/index.xml
```

Then install **System Audio Bridge (enhanced).vst3** and restart REAPER (or
*Options → Preferences → Plug-ins → VST → Re-scan*). It installs to `UserPlugins\FX`, which REAPER
always scans.

## Parameters

| Parameter | Default | |
|---|---|---|
| Capture system audio | on | Enable WASAPI loopback capture |
| During playback | Play recorded track audio | Or *Keep capturing* (original behaviour) |
| Capture source | Everything except REAPER | Or *Whole default device* |
| Capture gain | 0 dB | |
| Mix track input with capture | off | Off replaces the track input with system audio |
| Send output to default device | off | Plays the plugin's output on the default Windows device |
| Send gain | 0 dB | |

The capture and send streams follow the Windows default device that was active when they started.
Toggle the parameter to pick up a new default device.

## Build

Requires Visual Studio 2022 Build Tools and CMake ≥ 3.22. JUCE 8.0.4 is fetched automatically.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/SystemAudioBridgeEnhanced_artefacts/Release/VST3/`.

## Licence

AGPL-3.0 (inherited from JUCE 8's open-source licence). Contains no code from the original plugin.
