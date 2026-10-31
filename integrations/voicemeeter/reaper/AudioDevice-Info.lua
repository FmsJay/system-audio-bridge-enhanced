-- Read-only: writes REAPER's active audio device and channel names to %TEMP%\reaper-audioinfo.txt.
-- Run in the open REAPER with: reaper.exe -nonewinst AudioDevice-Info.lua (Reaper-Routing.py does this).
local out = {}
for _, k in ipairs({ "MODE", "IDENT_IN", "IDENT_OUT", "SRATE", "BSIZE" }) do
  local ok, v = reaper.GetAudioDeviceInfo(k, "")
  out[#out + 1] = k .. "=" .. tostring(ok and v or "")
end
out[#out + 1] = "OUTPUT_LATENCY_MS=" .. string.format("%.2f", reaper.GetOutputLatency() * 1000)
out[#out + 1] = "INPUTS=" .. reaper.GetNumAudioInputs()
out[#out + 1] = "OUTPUTS=" .. reaper.GetNumAudioOutputs()
for i = 0, reaper.GetNumAudioInputs() - 1 do out[#out + 1] = "IN" .. (i + 1) .. "=" .. tostring(reaper.GetInputChannelName(i)) end
for i = 0, reaper.GetNumAudioOutputs() - 1 do out[#out + 1] = "OUT" .. (i + 1) .. "=" .. tostring(reaper.GetOutputChannelName(i)) end
local f = io.open(os.getenv("TEMP") .. "\\reaper-audioinfo.txt", "w")
f:write(table.concat(out, "\n"), "\n")
f:close()
