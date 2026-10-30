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
