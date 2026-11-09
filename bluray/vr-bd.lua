-- vr-bd: seek keys. The video comes from a pipe (not seekable): the keys ask vr-bd
-- to restart the decoder at the new position.
local function seek(d) return function() mp.commandv("script-message", "3dbd-seek", tostring(d)) end end
local keys = {RIGHT = 10, LEFT = -10, UP = 60, DOWN = -60, PGUP = 600, PGDWN = -600,
              ["Shift+RIGHT"] = 5, ["Shift+LEFT"] = -5}
for k, d in pairs(keys) do
  mp.add_forced_key_binding(k, "3dbd-seek-" .. k, seek(d), {repeatable = false})
end
