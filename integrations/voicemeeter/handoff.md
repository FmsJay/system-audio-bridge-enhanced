# Native Voicemeeter integration handoff

Initialize the pinned Chroma source with git submodule update --init. On Windows, install Visual Studio 2022 C++ Build Tools with CMake and the Windows SDK, then run Build-Chroma.ps1 in this directory. The script builds the x64 Release DLL into the submodule out directory. It does not deploy it.

Validated using CMake 3.31.6, MSVC 19.44 and SDK 10.0.26100.0. The local DLL exported dummy_export at ordinal 1. Start-PotatoMod.ps1 checks the hashes of the installed upstream trial on Potato 3.1.1.8. A locally compiled DLL requires a separate runtime trial. Next test: verify mouse targeting on a dial, fader, route and dialog after resizing, then verify restoration of settings.

The preferred REAPER routing implementation extends this repository's existing plugin. Inspect Source/Wasapi.cpp and Source/PluginProcessor.cpp before designing selectable endpoints and shared send/return presets. Future milestones cover app sessions and Windows shortcuts, DAW routing, independent Windows speaker endpoints and VAIO 4, VST performance tests, and portable device profiles.

## Local runtime trial

The workspace trial needs three files together: stock voicemeeter8x64.exe, the release-patched voicemeeter8x64_vmchroma.exe, and locally built vmchroma64.dll. Set the working directory to the installed Voicemeeter folder. Omitting the adjacent stock EXE caused a fatal Chroma startup dialog. Keep these machine-local binaries outside Git.

Verified that the workspace DLL was loaded and the Potato mixer rendered. At original size and 90%, native mouse messages on unused strip 4 changed gain, compressor and B3 routing, with Remote API readback. Each value and the window dimensions were restored. At both sizes the VBAN button opened its network configuration window. These were synthetic messages, not physical mouse or mixed-DPI tests. Audio playback, stability over time and settings restore are still unverified. The workspace trial is the active mixer at this checkpoint. To return to the installed release, shut down the trial from its menu, then use Start-PotatoMod.ps1.

Next: validate settings export/restore in isolation and check audible playback before declaring milestone 1 complete.
