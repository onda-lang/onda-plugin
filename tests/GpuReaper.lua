-- Real DAW side of scripts/test-gpu-reaper.py. Only the isolated test project
-- and two muted tracks are touched. API: https://www.reaper.fm/sdk/reascript/reascripthelp.html
local root = assert(os.getenv('ONDA_GPU_REAPER_TEST_DIR'))
local instances = {}
for index, name in ipairs({'OndaSynth', 'OndaFX'}) do
  reaper.InsertTrackAtIndex(index - 1, true)
  local track = reaper.GetTrack(0, index - 1)
  reaper.SetMediaTrackInfo_Value(track, 'B_MAINSEND', 0)
  local fx = reaper.TrackFX_AddByName(track, name, false, 1)
  assert(fx >= 0, 'Could not load ' .. name)
  if not os.getenv('ONDA_GPU_REAPER_DROP_TEST') then
    assert(reaper.TrackFX_SetPreset(track, fx, root .. '/' .. name .. '.vstpreset'), 'Could not restore test preset')
  end
  instances[index] = {track=track, fx=fx}
  if index == 1 then reaper.TrackFX_Show(track, fx, 3) end
end
local function reply(id)
  local file = assert(io.open(root .. '/response.tmp', 'w'))
  file:write(id .. '\n' .. reaper.GetAppVersion() .. '\n')
  for _, instance in ipairs(instances) do
    local _, formatted = reaper.TrackFX_GetFormattedParamValue(instance.track, instance.fx, 0)
    file:write(tostring(reaper.TrackFX_GetParamNormalized(instance.track, instance.fx, 0)) .. '\n' .. formatted .. '\n')
  end
  file:close()
  os.rename(root .. '/response.tmp', root .. '/response')
end
reply('ready')
local previous
local started = reaper.time_precise()
local function tick()
  local file = io.open(root .. '/request', 'r')
  if file then
    local id = file:read('*l')
    local action = file:read('*l')
    file:close()
    if id ~= previous then
      previous = id
      if action == 'hide' or action == 'show' then
        for _, instance in ipairs(instances) do
          reaper.TrackFX_Show(instance.track, instance.fx, action == 'hide' and 2 or 3)
        end
      elseif action == 'OndaSynth' or action == 'OndaFX' then
        for index, instance in ipairs(instances) do
          local active = (action == 'OndaSynth' and index == 1) or (action == 'OndaFX' and index == 2)
          reaper.TrackFX_Show(instance.track, instance.fx, active and 3 or 2)
        end
      elseif action == 'automate' then
        for _, instance in ipairs(instances) do
          reaper.TrackFX_SetParamNormalized(instance.track, instance.fx, 0, 0.25)
        end
      elseif action == 'done' then
        for index=#instances,1,-1 do reaper.DeleteTrack(instances[index].track) end
        instances = {}
        reply(id)
        return
      else
        assert(action == 'query', 'Unknown test action: ' .. tostring(action))
      end
      reply(id)
    end
  end
  assert(reaper.time_precise() - started < 120, 'GPU DAW test driver timed out')
  reaper.defer(tick)
end
reaper.defer(tick)
