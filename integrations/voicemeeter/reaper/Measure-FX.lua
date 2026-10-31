-- Milestone 5 measurement, driven by Measure-FX.py. Runs in a temporary project tab so the open
-- project is untouched: one silent track (fader at -inf, FX still process) with N instances of
-- each FX, played on a loop through these phases:
--   dry (FX offline), FX on, FX bypassed, FX on again,
--   taken offline while playing (dry recovery), back online while playing.
-- Per phase it logs audio/media xruns (from GetUnderrunTime), output latency and the FX chain's
-- reported PDC. Config is read from %TEMP%\vm-measure-config.txt, results go to
-- %TEMP%\vm-measure-log.txt. The tab is saved to %TEMP%\vm-measure.rpp and closed at the end.
local temp = os.getenv("TEMP")
local log = io.open(temp .. "\\vm-measure-log.txt", "w")
local function say(s) log:write(s, "\n") log:flush() end

local cfg = { fx = {}, instances = 4, seconds = 6 }
for line in io.lines(temp .. "\\vm-measure-config.txt") do
  local k, v = line:match("^(%w+)=(.*)$")
  if k == "fx" then cfg.fx[#cfg.fx + 1] = v elseif k then cfg[k] = tonumber(v) end
end

if reaper.GetPlayState() ~= 0 then
  say("error the open project is playing or recording; stop it first")
  say("done") log:close() return
end

local repeatWas = reaper.GetSetRepeat(-1)
local userProject = reaper.EnumProjects(-1)
reaper.Main_OnCommand(40859, 0) -- File: New project tab
local proj = reaper.EnumProjects(-1)
reaper.InsertTrackAtIndex(0, false)
local tr = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", "vm-measure (silent)", true)
reaper.SetMediaTrackInfo_Value(tr, "D_VOL", 0)

local function finish()
  reaper.OnStopButton()
  reaper.GetSetRepeat(repeatWas)
  reaper.Main_SaveProjectEx(proj, temp .. "\\vm-measure.rpp", 8)
  reaper.Main_OnCommand(40860, 0) -- File: Close current project tab
  reaper.SelectProjectInstance(userProject) -- closing activates a neighbour tab; go back to the user's
  say("done") log:close()
end

for _ = 1, cfg.instances do
  for _, name in ipairs(cfg.fx) do
    if reaper.TrackFX_AddByName(tr, name, false, -1) < 0 then
      say("error FX not found: " .. name)
      finish() return
    end
  end
end
local n = reaper.TrackFX_GetCount(tr)
local function all(fn, v) for i = 0, n - 1 do fn(tr, i, v) end end
local function pdc()
  local total = 0
  for i = 0, n - 1 do
    local ok, v = reaper.TrackFX_GetNamedConfigParm(tr, i, "pdc")
    if not ok then return -1 end
    total = total + (tonumber(v) or 0)
  end
  return total
end

local phases = {
  { "dry (FX offline)", function() all(reaper.TrackFX_SetOffline, true) end },
  { "FX on", function() all(reaper.TrackFX_SetOffline, false) all(reaper.TrackFX_SetEnabled, true) end },
  { "FX bypassed", function() all(reaper.TrackFX_SetEnabled, false) end },
  { "FX on again", function() all(reaper.TrackFX_SetEnabled, true) end },
  { "taken offline while playing", function() all(reaper.TrackFX_SetOffline, true) end },
  { "back online while playing", function() all(reaper.TrackFX_SetOffline, false) end },
}

say(("setup fx=%d srate=%s bsize=%s"):format(n, select(2, reaper.GetAudioDeviceInfo("SRATE", "")),
                                             select(2, reaper.GetAudioDeviceInfo("BSIZE", ""))))
reaper.GetSet_LoopTimeRange(true, true, 0, 30, false)
reaper.GetSetRepeat(1)
phases[1][2]()
reaper.OnPlayButton()

local index, started = 1, reaper.time_precise()
local audioXr, mediaXr = reaper.GetUnderrunTime()
local xa, xm = 0, 0
say("phase\t" .. phases[1][1])

local function tick()
  local a, m = reaper.GetUnderrunTime()
  if a ~= audioXr then xa, audioXr = xa + 1, a end
  if m ~= mediaXr then xm, mediaXr = xm + 1, m end
  if reaper.time_precise() - started >= cfg.seconds then
    say(("result\t%s\t%d\t%d\t%.2f\t%d"):format(phases[index][1], xa, xm, reaper.GetOutputLatency() * 1000, pdc()))
    index = index + 1
    if index > #phases then finish() return end
    xa, xm = 0, 0
    phases[index][2]()
    started = reaper.time_precise()
    say("phase\t" .. phases[index][1])
  end
  reaper.defer(tick)
end
reaper.defer(tick)
