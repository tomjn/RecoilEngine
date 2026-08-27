-- Draws one figure twice in the same frame, once through a display list and once
-- directly, so the two can be compared without a second run.
--
-- Both sides in one frame removes run to run drift, which on a live scene is
-- larger than the artefact being looked for. The two halves are identical by
-- construction, so any difference between them is the list path.
--
-- Every element is opaque and a different saturated colour, on an opaque
-- backdrop. A transparent panel over terrain hides subtle breakage and lets map
-- texture show through as a false positive, which is how the build menu misled a
-- whole afternoon. With flat clashing colours a wrong draw is obvious and the
-- colour says which one it was.
--
-- Element 4 sets its colour part way through the block on purpose. That is the
-- back-fill path in LuaImmediateBatch, where the value for the vertices already
-- emitted is read with glGetFloatv. Inside glNewList(GL_COMPILE) a glColor is
-- recorded rather than executed, so that read can return a stale colour, and this
-- is the element that would show it.
--
-- The list is rebuilt every frame, which is not what a widget would do but is
-- what makes the artefact appear. The build menu only broke while it was being
-- scrolled, because scrolling is what made it recompile, and a frozen scene read
-- as clean. A probe that compiles once at Initialize would report nothing.
--
-- ROWS draws the pair many times a frame for the same reason the loop amplifier
-- draws two thousand circles. If the list path fails some of the time, one trial
-- a frame turns the result into "did it happen", where twelve turns it into "how
-- often", which is far stronger for the same number of frames.

function widget:GetInfo()
	return {
		name    = "List probe",
		desc    = "Draws a figure through a display list and directly, side by side",
		author  = "",
		date    = "",
		license = "GPL v2 or later",
		layer   = 3000,
		enabled = true,
	}
end

-- Clear of the build menu on the left, the minimap top left, and the probe's own
-- echo text along the bottom. All three overlapped the grid on the first attempt
-- and hid the copy being compared. Coordinates are the drawable, 3024x1832 here,
-- not logical points.
local ORIGIN_X = 1250
local ORIGIN_Y = 850
local W        = 260   -- width of one copy
local H        = 120
local GAP      = 40
local ROWS     = 6
local ROW_STEP = H + 10

-- Rebuild the list every frame. Compiling is what breaks, not replaying.
local RECOMPILE_EACH_FRAME = false

-- Draw the rows bottom to top instead of top to bottom.
local REVERSE_ROWS = true

-- Draw the direct copy before the list copy.
local DIRECT_FIRST = true

local theList

local function Quad(x1, y1, x2, y2)
	gl.Vertex(x1, y1, 0)
	gl.Vertex(x2, y1, 0)
	gl.Vertex(x2, y2, 0)
	gl.Vertex(x1, y2, 0)
end

-- Drawn at the origin. The caller translates, so the list is compiled with an
-- identity matrix and positioned at replay.
-- 1: colour set outside the block, no colour array, uses current colour
local function E1()
	gl.Color(1, 0, 0, 1)
	gl.BeginEnd(GL.QUADS, Quad, 10, 10, 60, 110)
end

-- 2: same again in a second colour, to catch a batch inheriting the first
local function E2()
	gl.Color(0, 1, 0, 1)
	gl.BeginEnd(GL.QUADS, Quad, 70, 10, 120, 110)
end

-- 3: per vertex colours set before the first vertex, so no back-fill
local function E3()
	gl.BeginEnd(GL.QUADS, function()
		gl.Color(0, 0, 1, 1) gl.Vertex(130, 10, 0)
		gl.Color(0, 0, 1, 1) gl.Vertex(180, 10, 0)
		gl.Color(1, 1, 0, 1) gl.Vertex(180, 110, 0)
		gl.Color(1, 1, 0, 1) gl.Vertex(130, 110, 0)
	end)
end

-- 4: colour set part way through, which forces the back-fill path. The first two
-- vertices must come out magenta, the last two white.
local function E4()
	gl.Color(1, 0, 1, 1)
	gl.BeginEnd(GL.QUADS, function()
		gl.Vertex(190, 10, 0)
		gl.Vertex(240, 10, 0)
		gl.Color(1, 1, 1, 1)
		gl.Vertex(240, 110, 0)
		gl.Vertex(190, 110, 0)
	end)
end

-- Elements 1 and 2 pass arguments through gl.BeginEnd, 3 and 4 pass none. Drawing
-- them out of coordinate order separates "the first two batches drawn" from "the
-- two that pass arguments", which are the same pair in the natural order.
local ORDER = { E1, E2, E3, E4 }

local function Figure()
	for i = 1, #ORDER do
		ORDER[i]()
	end

	gl.Color(1, 1, 1, 1)
end

function widget:Initialize()
	theList = gl.CreateList(Figure)

	if theList == 0 then
		Spring.Echo("[listprobe] gl.CreateList returned 0, nothing to compare")
	else
		Spring.Echo(string.format("[listprobe] list %d created", theList))
	end
end

function widget:Shutdown()
	if theList then gl.DeleteList(theList) end
end

function widget:DrawScreen()
	if RECOMPILE_EACH_FRAME then
		if theList and theList ~= 0 then
			gl.DeleteList(theList)
		end
		theList = gl.CreateList(Figure)
	end

	if not theList or theList == 0 then
		return
	end

	-- Opaque backdrop covering every row, so nothing behind them can be mistaken
	-- for a difference. gl.Rect goes through glRectf, not a batch.
	--
	-- Orange rather than black on purpose. A quad drawn with a wrong or zeroed
	-- colour comes out black, which on a black backdrop is indistinguishable from
	-- one that was never drawn at all. That cost an hour here.
	-- Striped, not flat. A quad that never drew shows the stripes through it, a
	-- quad drawn in a stale colour shows solid. On a flat backdrop those two are
	-- the same picture, which is what made this take three attempts to read.
	local bx1 = ORIGIN_X - 10
	local bx2 = ORIGIN_X + (W * 2) + GAP + 10
	local by1 = ORIGIN_Y - 10
	local by2 = ORIGIN_Y + (ROWS * ROW_STEP) + 10

	-- Flat, deliberately. Striping it took the artefact from 4658 differing pixels
	-- to 1471, so the fifty extra gl.Rect calls were changing what was being
	-- measured. One rect keeps the reproduction.
	gl.Color(1.0, 0.45, 0.0, 1)
	gl.Rect(bx1, by1, bx2, by2)

	-- REVERSE_ROWS separates "first call after the list was built" from "bottom of
	-- the screen". The first row drawn loses two batches, and those two things are
	-- the same row until the order is flipped.
	for i = 0, ROWS - 1 do
		local row = REVERSE_ROWS and (ROWS - 1 - i) or i
		local y = ORIGIN_Y + (row * ROW_STEP)

		-- Sentinel. A quad that ignores the gl.Color set inside the list comes out
		-- cyan, one that was never drawn leaves the orange backdrop showing. Those
		-- two look identical if the sentinel matches the backdrop, which is how an
		-- ignored colour got read as a dropped batch twice in a row.
		gl.Color(0, 1, 1, 1)

		-- DIRECT_FIRST decides which copy is drawn first. The list copy was always
		-- first until now, so "the list is broken" and "whatever is drawn first is
		-- broken" have never been told apart. If the artefact follows the order
		-- rather than the list, this is not about display lists at all.
		local function DrawList()
			gl.PushMatrix()
			gl.Translate(ORIGIN_X, y, 0)
			gl.CallList(theList)
			gl.PopMatrix()
		end

		local function DrawDirect()
			gl.PushMatrix()
			gl.Translate(ORIGIN_X + W + GAP, y, 0)
			Figure()
			gl.PopMatrix()
		end

		if DIRECT_FIRST then
			DrawDirect()
			DrawList()
		else
			DrawList()
			DrawDirect()
		end
	end

	gl.Color(1, 1, 1, 1)
end
