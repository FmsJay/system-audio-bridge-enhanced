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

Real-pointer test (desktop control, real mouse input, on Stereo Input 4, every value read back through the Remote API and reset):
- 100 % (1645x770): fader drag 0 -> -20.1 dB, Comp dial drag 0 -> 4.0, B3 click 0 -> 1, VBAN button opened "VB-Audio Network Configuration", which closed from its own close button.
- 90 % reached by dragging the resize corner (1482x694): fader -> -20.5 dB, Comp -> 4.0, B3 -> 1, VBAN dialog opened and closed. 75 % after a programmatic resize: B3 click hit.
- Bug found and fixed: resizes that skip WM_SIZING (Windows Snap, SetWindowPos from another tool) left Potato's child windows (app list, recorder, EQ panels) at their old positions, drawn over the scaled layout. The WM_SIZE hook now calls resize_child_windows() unless minimized (in audio-shortcuts.patch). Verified at 75 %.
- Deploy-Trial.ps1 had stopped the mixer and then failed to replace a vm-sessions.exe locked by an open panel, which left the mixer down for about 30 s until restarted by hand. It now closes the panel first and restarts the mixer in a finally block. Rerun with the panel open: clean.
- Active trial DLL SHA256 566FC20F6554C44BF10D0571CC6CDD7AF9130F91BFDBF7208EC73031A23452E3. Mixed DPI (the mixer moved to the second monitor at a different scaling) was not tested.

## VAIO extension activated, native Load verified (2026-10-30, later)

The user bought and activated the Potato VAIO extension licence. Strip[n].VAIO now stays on, and "Voicemeeter In n+1" feeds hardware strip n. Bus[n].VAIO publishes A(n+1) post-fader on "Voicemeeter Out A(n+1)". Both are off on this machine except while a test runs.
- Milestone 4 on hardware buses (Signal-Check.py proof-a): passed. Using A1 instead of A2, since there is no A50. Stereo Input 4 was fed from "Voicemeeter In 4", 700 Hz at -45 dBFS. Routed to A1 it reached Out A1 at -54.7 dB (A1 fader -9.25 dB) and not Out A3. Routed to A3 it reached Out A3 at -42.8 dB (A3 fader +2.75 dB) and left A1 (-122 dB). The 1300 Hz tone on Voicemeeter Input stayed on Out A1 (-55 dB) and never reached Out A3 (below -100 dB). All switches and routes were restored.
- Endpoint budget with the extension: three virtual inputs (Voicemeeter Input / AUX / VAIO3 = strips 6-8) plus In 1-5 into hardware strips 1-5. Strips 1-3 have hardware inputs, so In 4 and In 5 (strips 4-5) are the two extra independent Windows endpoints that displace nothing, routable to any of A1-A5/B1-B3. They are the "VAIO 4" substitute; Potato has no fourth virtual strip. A2/A4/A5 currently have no device (A50 and RTK HDR absent).
- Native-Settings.py test: passed. Steps: export (137,057 bytes), mark Stereo Input 4 (-6 dB, A3, label LoadTest), Command.Load the export. Result: the mark was gone and routing/levels/labels/devices equalled the export. A re-export was identical line for line (0 differing lines), so the duplicated opening tag found by the audit does not stop Potato loading its own files. Load restarts the engine for a moment. Do not log in to the Remote API again while logged in; load() now only refreshes. Test exports are in D:\Agent Workspace\VoicemeeterMod\restore-test-20261030b (private, outside Git).
- Profiles.py saves and applies Strip/Bus VAIO switches. Reaper-Routing.py maps "Voicemeeter In n" to hardware strip n-1, checks the strip's VAIO switch, and has a new bridge-send-in4 preset: REAPER mix on its own strip, Stereo Input 4, leaving AUX and VAIO3 to Windows. Its check reports what is needed (VAIO on, A1 route); it was previewed, not applied.
- The tools are registered in the workstation brain (brain_mcp register_tool) and appear in CLAUDE.md on both PCs.

Shortcuts (at the user's request): the taskbar pin, the Start menu "Voicemeeter Potato x64" (ProgramData) and the Startup "Voicemeeter (VB-Audio)" entry now launch runtime-trial\voicemeeter8x64_vmchroma.exe, with the working directory C:\Program Files (x86)\VB\Voicemeeter and the stock icon. The duplicate Startup entry "voicemeeter8x64.exe - Shortcut" was moved out. The originals, with their locations, are in D:\Agent Workspace\VoicemeeterMod\shortcut-backup-20261030; copy them back to undo. These shortcuts bypass Start-PotatoMod.ps1's hash check. A Voicemeeter reinstall or update may reset the Start menu entry.

Still open:
- Mixed DPI (the mixer on the second monitor at a different scaling): the user accepted it as is.
- Milestone 5 with real plugins: the user will test through the Audio Bridge (Measure-FX.py --fx "<name>").
- Audio Bridge 1.1.0 REAPER checks and release: the user will test later.

## Softphone jump-in built into the mod (2026-11-01; installed on the laptop)

Requested by James through ITS WORK on JAMES-MAIN, to replace the floating Laptop-jump.ps1 window. Status: installed at 18:42 on 11/1 with James's OK, setting on in the laptop's vmchroma.yaml. Trial DLL SHA256 716320102AA48ECAF0972C956E9ECBBCCA2F44B6ABEB0C0742168DD85D2AC12F; rollback DLL is the previous 566FC20F build. A restart of Voicemeeter still needs James's OK at that moment, because he may be on a call.

Checked on the installed version with no call up: the 121-value restart comparison showed no differences; vban.instream[0].quality stayed 2 and was saved as NQ='2'; the banner draws in the real window ("Mic off | Alert not running | F9 to talk", which was true: call_monitor.py wasn't publishing, last write 18:25); a real F9 opened the mic (host talk, Bus[7] unmuted, stream on, red banner and border, taskbar badge) and closed it (Bus[7] muted, host listen). Both menu entries are present, read from the popup's menu data, but they sit below App Sessions at the bottom of the menu and were off the visible part of the screen on the laptop's display.

Not yet proven: exit order with the helper running (Bus[7] restored before settings are saved), a crash leaving the host in listen, show-without-focus while minimized, an automatic open from a real call, and the moved live tests.

Found on the installed version and fixed in the engine, not yet installed: opening and closing by hand with no call up left vban.outstream[0].on=1 with Bus[7] muted (silent) because no hang-up comes to clear it. The running helper still has this until a restart; the build in build-tools\Release has the fix. Deploy-Trial.ps1 now keeps the rollback DLL when the DLL being deployed is unchanged.

Shape:
- tools\softphone\softphone_engine.hpp: the mic rules as a pure module, ported from Laptop-jump.ps1 as of 16:30 on 11/1 (mixer and host are ports, time is passed in).
- tools\softphone\vm-softphone.cpp: hidden helper started by the DLL, with no window, console or startup item. It holds the hotkey on a message-only window, reads live-state.txt on its own thread, sends VBAN-Text, drives the Remote API and publishes status to named shared memory. It's a process so a stalled share or fault can't reach the mixer's threads, and so it can still tell the host to listen if the mixer dies. It logs mixer and host actions to Documents\Voicemeeter\softphone.log, never the transcript.
- DLL (audio-shortcuts.patch): softphone_ui in vmchroma.cpp starts and watches the helper (relaunches after 5 s; terminates it if its heartbeat is over 10 s old, and its safe startup closes the mic), reads status without locks or I/O, and draws the banner through softphone_overlay.hpp in window_manager::render. Menu entries "Softphone: Automatic Mic" (0x133C, state in HKCU) and "Softphone: Open or Close Mic" (0x133D, the fallback if the hotkey is taken). It raises the mixer without focus when the mic opens automatically.
- Taskbar: the helper turns the mixer's taskbar button red and adds a red badge (ITaskbarList3, verified from another process against the live mixer window) while the mic may be live, because the Potato window is often minimized or covered.
- Setting: a softphone section in Documents\Voicemeeter\vmchroma.yaml with enabled, hostIp, stateFile, hotkey, sendBus, micStrip. Absent means off, so JAMES-MAIN's copy isn't affected. hostIp stays an address because the host's Command1 slot filters on the laptop's Tailscale source.

Decisions agreed with ITS WORK and James:
- A hand-opened mic closes when the state reaches call.
- Host commands go out twice, 100 ms apart, and a pending repeat is replaced so a late talk can't follow a listen.
- Mixer changes are read back and retried; a change unconfirmed after 2 s shows MIC STATE UNKNOWN. Close and stop-stream go in one batch, mute first, to avoid the dropped second batch.
- A failed or partial read is "no new data"; an unknown state proves the monitor is alive but changes nothing.
- Automatic off: nothing arms, opens or closes from person/machine; the hotkey arms and opens; idle/off/research still close; turning it off closes an automatically opened mic.
- A 5 s share stall closes an automatically opened mic mid-call and a returning "person" reopens it. James accepted this as specified.
- Additions beyond Laptop-jump: muting Bus[7] by hand in Potato counts as a hand close; an unexpected unmute while closed gets re-muted; nothing on the mixer is touched until setup is verified (vban.Enable, stream names, mic strip routed to the send bus), otherwise the banner says not set up; the transcript updates on every line.

Tests that need no call, mixer or network: build-tools\Release\softphone-tests.exe (14 rule groups with a simulated mixer, including dropped batches) and Test-Softphone.py (real helper with --stub-mixer, a localhost UDP host and a temp state file: VBAN-Text bytes, status block, a real F24 press, a taken key picked up later, a stalled share simulated with a pipe, shutdown, mixer death, not set up, single instance). overlay-preview.exe renders every banner state over a saved mixer picture.

Guards added because Stereo Input 4 now carries the call and B3 the mic:
- Mixer_Guard.py refuses a strip with a device, routing or an incoming VBAN stream. Test-Profiles, Test-Routing, Native-Settings test and Signal-Check proof-a moved to the free Stereo Input 5 / In 5. Signal-Check proof is refused on a bridge mixer.
- The preset bridge-send-in4 is now bridge-send-in5. The earlier section's Stereo Input 4 endpoint budget is out of date: only In 5 is free now.
- Native-Settings restore/test need --no-call-in-progress; Deploy-Trial.ps1 needs -NoCallInProgress. Profiles.py never sets Bus[7].Mute on a bridge mixer.
- These guards' refusals were checked, but the moved tests have not been re-run live: not during ITS WORK's test.

Cutover (James's OK at that moment, no call up, ITS WORK's test finished): tools\softphone\Softphone-Cutover.ps1 -NoCallInProgress. It reports any Laptop-jump shortcut in shell:startup without removing it, closes the floating window gracefully, checks the hotkey is free, adds the yaml section with a backup, and deploys. Undo: restore the yaml backup, Deploy-Trial.ps1 -NoCallInProgress -Rollback, start Laptop-jump.cmd. The restart comparison also covers vban.instream[0].quality (2, Medium, set by bridge-laptop.ps1 setup; saved settings still held NQ='1' on 2026-11-01 because Voicemeeter writes them only on a clean exit). If the comparison reports it changed, set it back to 2 through the Remote API. The helper's setup check does not look at quality, so any value counts as set up.

Needs a restart, James or a real call:
- The DLL side has only been compiled: helper launch and supervision, banner in the real window, menu entries, restore without focus while minimized.
- The real RemoteMixer path in the helper: Bus[7] and stream read-back, setup check.
- Exit order: whether the Bus[7] restore lands before Voicemeeter saves its settings, given the DLL waits up to 1.5 s in WM_DESTROY with the UI thread blocked (the Remote API may need that thread). Check Bus[7] in LastSettings after a clean exit. Also whether a mixer crash leaves the host in listen.
- Real F9 once the floating window is closed; what else on this PC wants F9 (it is consumed system-wide while enabled).
- A real call: person/machine switching, audio reaching the softphone, Laptop mic blocked in listen, and whether the banner and badge are hard enough to miss.

Suggestions only, not built: a short chime when the mic opens or closes; a host acknowledgement over VBAN-Text so the banner can show the host really switched; recent transcript lines in the App Sessions panel; one-click bridge repair from the not-set-up banner; a click on the banner to flip the mic.

