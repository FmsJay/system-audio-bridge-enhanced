# Native Voicemeeter integration handoff

Initialize the pinned Chroma source with git submodule update --init. On Windows, install Visual Studio 2022 C++ Build Tools with CMake and the Windows SDK, then run Build-Chroma.ps1 in this directory. The script builds the x64 Release DLL into the submodule out directory. It does not deploy it.

Validated using CMake 3.31.6, MSVC 19.44 and SDK 10.0.26100.0. The local DLL exported dummy_export at ordinal 1. Start-PotatoMod.ps1 checks the hashes of the installed upstream trial on Potato 3.1.1.8. A locally compiled DLL requires a separate runtime trial. Next test: verify mouse targeting on a dial, fader, route and dialog after resizing, then verify restoration of settings.

The preferred REAPER routing implementation extends this repository's existing plugin. Inspect Source/Wasapi.cpp and Source/PluginProcessor.cpp before designing selectable endpoints and shared send/return presets. Future milestones cover app sessions and Windows shortcuts, DAW routing, independent Windows speaker endpoints and VAIO 4, VST performance tests, and portable device profiles.

## Local runtime trial

The workspace trial needs three files together: stock voicemeeter8x64.exe, the release-patched voicemeeter8x64_vmchroma.exe, and locally built vmchroma64.dll. Set the working directory to the installed Voicemeeter folder. Omitting the adjacent stock EXE caused a fatal Chroma startup dialog. Keep these machine-local binaries outside Git.

Verified that the workspace DLL was loaded and the Potato mixer rendered. At original size and 90%, native mouse messages on unused strip 4 changed gain, compressor and B3 routing, with Remote API readback. Each value and the window dimensions were restored. At both sizes the VBAN button opened its network configuration window. These were synthetic messages, not physical mouse or mixed-DPI tests. Audio playback, stability over time and settings restore are still unverified. The workspace trial is the active mixer at this checkpoint. To return to the installed release, shut down the trial from its menu, then use Start-PotatoMod.ps1.

Next: validate settings export/restore in isolation and check audible playback before declaring milestone 1 complete.

## Settings audit and bridge inspection

Audit-Settings.py saves two native exports to a newly created private directory and records 120 strip/bus routing and level parameters before and after. Run with Python on Windows and --output pointing outside the repository. It performs no load or mixer parameter writes. Current trial produced identical 137057-byte exports with no observed parameter changes. Each contains consecutive VoiceMeeterParameters opening tags at lines 21 and 22, with only one corresponding closing tag. Thus strict XML parsing fails deterministically; this alone does not prove Potato cannot load the file.

Native restoration remains untested. A second desktop process is not an isolated audio environment because of shared installed drivers and remote state. Use a separately provisioned Windows test environment for a native load test, or explicitly plan a controlled live restore with downtime and a recovery path. Preserve original exports byte-for-byte; do not silently “repair” them or claim a successful restore based on XML parsing. Machine snapshots and exports live outside the Git checkout.

Bridge source inspection: Wasapi.cpp activateDefaultRenderDevice selects eRender/eConsole through GetDefaultAudioEndpoint. CaptureSource supports excluding the host process and whole-default-device loopback. DeviceRender.start currently accepts only sampleRate. PluginProcessor stores its APVTS tree in host state. Integration should extend explicit endpoint selection and persist endpoint IDs with friendly logical names while preserving existing projects' default-device behavior. A missing selected endpoint must be surfaced rather than silently rerouted. Process-excluding capture and endpoint-specific loopback are separate modes and need distinct UI labels and tests.

## Native Windows audio shortcuts implemented

The main Potato Menu now contains Windows Sound Settings, Windows Volume Mixer, and Sound Control Panel, beside Chroma's existing menu entry. audio-shortcuts.patch modifies the pinned Chroma source. Build-Chroma.ps1 applies it when absent and checks already-applied state before compiling. Commit the patch when changing the submodule source, since the submodule itself still points to the upstream revision.

Latest DLL SHA256 B676FF9F764EA796578D8DA47D9E7FEC3ED9579573E4413017BCD0F09D869DEB is active in the workspace runtime trial. Verified actual native popup labels, exercised all three command IDs, and observed Settings and Sound windows. Exact Settings subpage rendering was not inspected. The classic panel uses system control.exe with mmsys.cpl; attempting to ShellExecute the CPL directly failed in the first trial and was corrected. Menu launch failures show a message instead of terminating the mixer. The preceding baseline DLL is retained as runtime-trial/vmchroma64.previous.dll outside Git.

Next implementation step is a scrollable app-session panel. Native restore and audible-playback verification remain open milestone 1 items.

## Milestones 1-6 build-out (2026-10-30, JAMES)

Everything below was built with MSVC 2022 / Python 3.12 and run against the live Potato 3.1.1.8 trial on JAMES. Every live change was confined to the idle Stereo Input 4 (Strip[3]) or was temporary, and all 120 strip/bus routing and level values matched the morning audit afterwards.

Mixer facts found on the way:
- The Remote API level meters (VBVMR_GetLevel) return 0 on every channel with this trial, even while audio flows. Audio is verified by recording the "Voicemeeter Out B1-B3" endpoints instead (B1-B3 carried the mic at about -35 dBFS).
- "Voicemeeter In 1-5" and "Voicemeeter Out A1-A5" belong to the separately licensed Potato VAIO extension. Strip[n].VAIO is accepted but reset to 0 after an engine restart, In n reaches no strip, and Out A1 records digital silence, so the licence is not active. Activation is the user's purchase (Potato Main menu, "VAIO Extension License").
- A2 (A50 headset), A4/A5 (RTK HDR), Stereo Input 2 (UR22mkII) and Stereo Input 3 (NVIDIA Broadcast) show red, unavailable devices.
- The Remote API caches parameters and updates them on the poll after the one that reports dirty. Profiles.refresh() polls until clean; a single poll after a write reads stale values.
- REAPER is live on ASIO TONEX at 44.1 kHz and 128 samples (5.5 ms output latency). The WASAPI entries in REAPER.ini (in Voicemeeter Out B1, out AUX Input) are a saved alternative. [audioconfig] mode=3 is ASIO here.

Tools (tools\Build-Tools.ps1 builds the native ones into build-tools\Release, which is ignored by Git):
- vmtone.exe: tone on a named render endpoint, or `--meter` a recording endpoint with per-frequency (Goertzel) levels.
- Signal-Check.py `buses` / `proof`: milestone 1 known signal and milestone 4 isolation. Passed: with A2 and B3 added to AUX, a -45 dBFS 700 Hz tone on AUX arrived on B3 at -46.4 dBFS, and a 1300 Hz tone on Voicemeeter Input (A1 only) stayed at the -80 dBFS floor. AUX routing was restored. B3 is the stand-in because A buses can't be recorded without the VAIO extension, and A2 has no device.
- vm-sessions.exe (milestone 2): a separate process opened from the new "App Sessions..." Potato menu entry (0x133B, in audio-shortcuts.patch). It lists Windows sessions per Voicemeeter input strip in a scrollable list with a 0.5 dB-step slider, a readable dB value, Mute, and Sound settings / Volume mixer buttons. Keyboard: Tab, arrows 1 dB, PgUp/PgDn 6 dB, Home 0 dB, End off, Space mute, Esc closes. VAIO-extension groups that hold only an idle System sounds session are hidden. `--dump` prints JSON lines. Test-Sessions.py drives it with its own -70 dBFS sessions: all checks pass.
- Deploy-Trial.ps1 (+ Mixer-Snapshot.py): snapshots the mixer, shuts the trial down through the Remote API, keeps the old DLL as runtime-trial\vmchroma64.rollback.dll, copies the DLL and vm-sessions.exe, restarts, checks the loaded module path and the 120 values. -Rollback restores. Active trial DLL SHA256 01109EF62FC3C6D704173DDA0D026F091B7387C760958240FA17FDD26854D413. The menu was read from the real popup and the entry launched runtime-trial\vm-sessions.exe.
- Profiles.py (milestone 6): versioned profiles (routing, levels, labels, hardware devices by logical name), a per-machine devices.json mapping, preview, conflict handling (unmapped/absent devices, a version saved on two machines), scoped apply (--only Strip[3]) and rollback snapshots. Applies through the Remote API, never native Load. Store: %VM_PROFILES% or Documents\Voicemeeter\profiles, outside Git. Test-Profiles.py: all checks pass.
- Reaper-Routing.py + reaper\routing-presets.json (milestone 3): reads REAPER's live device through reaper\AudioDevice-Info.lua (reaper.exe -nonewinst). It checks record/return, Audio Bridge and insert presets with explicit channels against live routing. FEEDBACK means a return strip routes to the bus REAPER records, or a capture records REAPER's own output. Apply sets only the Voicemeeter side, after a rollback snapshot, and refuses to enable an insert unless REAPER is on the Voicemeeter Potato Insert Virtual ASIO. The insert map is 34 channels (PatchInsert c1-c34): hardware strip n is 2n-1..2n, virtual strips 11-18, 19-26, 27-34. Test-Routing.py: all checks pass.
- Measure-FX.py + reaper\Measure-FX.lua (milestone 5): in a temporary REAPER tab with a silent track, steps N plugin instances through dry, on, bypassed, on, offline while playing (dry recovery) and back online. Reports REAPER process CPU, audio/media xruns, output latency and the chain PDC per phase, then closes the tab and reselects the user's tab. Baseline with 24 stock Cockos FX: 0 xruns in every phase, 5.51 ms output latency, PDC 0. CPU stayed in the 5-26 % range of one core, dominated by REAPER itself.

Still open, and why:
- Physical-mouse and mixed-DPI pointer test: computer control was held by another session. Steps: at 100 % and 90 % window size, drag Stereo Input 4's fader and Comp dial, click its B3 route and the VBAN button. Read each change via the Remote API, then restore.
- Native settings Load (Command.Load) is still untested and needs an isolated Windows environment. Profiles.py avoids needing it.
- VAIO extension (In 1-5 as extra endpoints, a "VAIO 4" style strip, Out A1-A5 recording) needs the licence; then rerun Signal-Check.py with an A-bus meter.
- A2 audible check needs the A50 connected.
- Milestone 5 with the plugins the user actually plans to host: Measure-FX.py --fx "<name>" ...
