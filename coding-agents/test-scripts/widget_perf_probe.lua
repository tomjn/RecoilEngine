-- Freezes the scene and reports where the frame time goes.
--
-- Copy or symlink into the throwaway game's LuaUI/Widgets/. install-probe.sh
-- does that, and keeps the source in the repo so it is not lost with the copy.
--
-- Why a widget rather than an engine change: Spring.GetProfilerRecordNames and
-- Spring.GetProfilerTimeRecord already expose every SCOPED_TIMER to Lua, and
-- "debug 1 0" turns the profiler on with its on-screen overlay off, so the whole
-- per-timer breakdown is reachable without touching the engine.
--
-- Why it freezes the scene: a live sim draws something different every run, and
-- differences between runs on this platform have already been large enough to
-- swamp what was being measured. Pausing at a fixed sim frame and re-applying
-- one camera state every frame makes the drawn scene the same from that point on,
-- for the whole run and between runs.

function widget:GetInfo()
	return {
		name    = "Perf probe",
		desc    = "Freezes the scene at a fixed sim frame and logs the profiler breakdown",
		author  = "",
		date    = "",
		license = "GPL v2 or later",
		layer   = 1000,
		enabled = true,
	}
end

local LOCK_FRAME = 90   -- sim frame to freeze at, 3 seconds in
local INTERVAL   = 5    -- seconds between reports
local TOP_N      = 16   -- timers reported, by share of wall clock

local lockedCam  = nil
local drawFrames = 0
local lastReport = nil

-- Forward declared rather than left as an implicit global, because some widget
-- handlers refuse writes to new globals.
local reportProfile

function widget:Initialize()
	-- Profiler on, overlay off. The overlay is itself a per-frame draw cost and
	-- would be measuring the measurement.
	Spring.SendCommands("debug 1 0")
	lastReport = Spring.GetTimer()
	Spring.Echo("[probe] profiler enabled, freezing at sim frame " .. LOCK_FRAME)
end

function widget:GameFrame(f)
	if f ~= LOCK_FRAME or lockedCam ~= nil then
		return
	end

	lockedCam = Spring.GetCameraState()
	Spring.SendCommands("pause 1")
	Spring.Echo("[probe] scene frozen at sim frame " .. f)
end

function widget:DrawScreen()
	drawFrames = drawFrames + 1
end

function widget:Update()
	-- Every frame, not once: this also holds the camera against edge scroll and a
	-- knocked mouse, either of which would change the scene mid-measurement.
	if lockedCam ~= nil then
		Spring.SetCameraState(lockedCam, 0)
	end

	local now = Spring.GetTimer()
	local dt = Spring.DiffTimers(now, lastReport)

	if dt < INTERVAL then
		return
	end

	Spring.Echo(string.format("[probe] fps=%.2f drawframes=%d over %.2fs", drawFrames / dt, drawFrames, dt))

	drawFrames = 0
	lastReport = now

	-- Guarded, because an error in a widget removes the widget, and removing this
	-- one would quietly unfreeze the scene mid-run. That already happened once.
	local ok, err = pcall(reportProfile)
	if not ok then
		Spring.Echo("[probe] profiler read failed, scene stays frozen: " .. tostring(err))
	end
end

reportProfile = function()
	local rows = {}
	for _, name in ipairs(Spring.GetProfilerRecordNames()) do
		-- The second argument has to be passed. GetProfilerTimeRecord pushes its
		-- five results before reading it with luaL_optboolean(L, 2), so on a
		-- one-argument call index 2 is the pushed total and the call errors with
		-- "boolean expected, got number". An engine bug, not a Lua one.
		local _, current, maxdt, pct = Spring.GetProfilerTimeRecord(name, false)
		rows[#rows + 1] = { name = name, pct = pct or 0, current = current or 0, maxdt = maxdt or 0 }
	end
	table.sort(rows, function(a, b) return a.pct > b.pct end)

	for i = 1, math.min(TOP_N, #rows) do
		local r = rows[i]
		Spring.Echo(string.format("[probe]   %5.1f%% max %6.2fms  %s", r.pct * 100, r.maxdt, r.name))
	end
end
