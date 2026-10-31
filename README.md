# System Audio Bridge (enhanced)

A Windows VST3 that captures system audio (YouTube, Spotify, a game, a call) into a REAPER track. It's a
clean-room reimplementation of the idea behind Bird's freeware *System Audio Bridge*, with three changes:

1. **Recorded takes play back.** With the transport playing (but not recording), the plugin passes
   the track's own audio through. The original keeps outputting live system audio, so after you
   recorded a take you heard silence instead of it. Set *During playback* to *Keep capturing* if you
   want the original behaviour.
2. **Send to a Windows device.** *Send output to Windows device* plays whatever reaches the plugin
   on the default Windows output, or on a device chosen under *Send to*. Put an instance on the master
   track with capture off and send on to hear the REAPER mix through your normal speakers or
   headphones, even while REAPER runs on an ASIO interface.
3. **Choose devices.** *Capture from* and *Send to* pick specific Windows output devices (see
   [Devices](#devices)).

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
| Send output to Windows device | off | Plays the plugin's output on the *Send to* device |
| Send only while REAPER is playing | off | When on, the send goes silent while the transport is stopped (still sends while recording) |
| Send gain | 0 dB | |

*Capture source* applies only while *Capture from* is set to *Default device*.

## Devices

Below the parameters, two menus choose Windows output devices. Both start on *Default device*, which
is also what projects saved with 1.0.x load as, so existing projects behave as before.

- **Capture from** - *Default device* uses the *Capture source* parameter (everything except REAPER,
  or the whole default device). Choosing a device captures a **whole-device loopback** of it: everything
  playing on that device, including REAPER's own output if REAPER plays there. Process exclusion
  can't be combined with a chosen device, which is why the menu names it differently.
- **Send to** - the device the send plays on.

The choice is saved in the project by device ID, with its name. If a chosen device is unplugged or
disabled, the plugin says so in the status line (*Capture device missing: ...*) and stays silent. It
never falls back to another device, and it reconnects on its own when the device returns. A device another
app holds in exclusive mode (for example a Voicemeeter hardware output) shows *... device in use* and is
retried the same way until it is released. *Refresh devices*
re-reads the list after plugging something in. Choosing the same device for capture and send warns
about feedback.

On *Default device*, a stream uses whichever device was the Windows default when it started. Toggle
the parameter to pick up a new default (a stream that loses its device reopens the current default).

## Build

Requires Visual Studio 2022 Build Tools and CMake ≥ 3.22. JUCE 8.0.4 is fetched automatically.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/SystemAudioBridgeEnhanced_artefacts/Release/VST3/`.

## Licence

AGPL-3.0 (inherited from JUCE 8's open-source licence). Contains no code from the original plugin.
