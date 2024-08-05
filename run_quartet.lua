local quartet_inputs = require("inputs")

local quartet_directions = { "Up", "Right", "Down", "Left" }
local quartet_direction = -1

for i,v in ipairs(quartet_inputs) do
	while true do
		emu.frameadvance()
		-- unwind a frame if the frame we advanced to is not a lag frame
		if (not emu.islagged()) then
			tastudio.setplayback(emu.framecount() - 1)
			break
		end
	end

	local delay = v
	while (delay > 0) do
		emu.frameadvance()
		delay = delay - 1
	end

	-- first turn is special
	if (i == 1) then
		tastudio.submitinputchange(emu.framecount(), "A", true)
	-- in these cases, we need change direction to avoid consecutive input lag
	elseif (i & 3) == 2 then
		quartet_direction = (quartet_direction + 1) & 3
		tastudio.submitinputchange(emu.framecount(), quartet_directions[quartet_direction + 1], true)
	-- in these cases, we can't change direction, so we must go through consecutive input lag
	else
		emu.frameadvance()
		tastudio.submitinputchange(emu.framecount(), quartet_directions[quartet_direction + 1], true)
	end

	tastudio.applyinputchanges()
	emu.frameadvance()
end
