/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

// TEMP diagnostic, not for merge. Runtime-readable renderer diagnostic switches.
//
// The switches used to be read into `static const bool` locals, so a switch was
// fixed for the whole process and every A/B had to compare two runs. On this
// platform two runs differ by more than the switches do: window focus alone
// moved a frame rate by 20%, and three of four runs in one comparison were
// unattended. Reading the switches per frame instead lets one run cycle a
// schedule of configurations, so both sides see one scene, one focus state and
// one driver state.
//
// SPRING_DIAG_CELLS=<seconds>:<cell>[/<cell>...] sets the schedule, where a cell
// is `-` for no switches or a comma-separated list of switch names. So
//
//     SPRING_DIAG_CELLS=10:-/nopresent/finish
//
// spends 10 seconds a cell, cycling for as long as the run lasts, and logs the
// frame rate of each cell as it ends. Without it every switch keeps its old
// meaning as a plain environment variable, so nothing changes unless a schedule
// is asked for.

#pragma once

namespace DiagSwitches {
	enum Switch {
		BATCH_FLUSH = 0,   // SPRING_BATCH_FLUSH, glFlush before every glBegin
		BATCH_NARROW,      // SPRING_BATCH_NARROW, the old varying vertex arity
		NO_BATCH_FLUSH,    // SPRING_NO_BATCH_FLUSH, no mitigation at all
		NO_PRESENT,        // SPRING_NO_PRESENT, skip the readback and present
		FRAME_FINISH,      // SPRING_FRAME_FINISH, glFinish once per frame
		FRAME_THROTTLE,    // SPRING_FRAME_THROTTLE, sleep to 60 fps
		SWITCH_COUNT
	};

	// The mask for the frame being drawn. Read directly rather than through a
	// call because LuaVertexN consults it per vertex.
	extern unsigned int currentMask;

	inline bool On(Switch s) { return (currentMask & (1u << s)) != 0u; }

	// Call once per present. Advances the schedule and logs the cell that ended.
	// gpuMillis is the frame's GPU time, or negative when it is not available.
	void FramePresented(double gpuMillis);

	// The current cell's label, or nullptr when no schedule is running. Used to
	// tag captures so frames can be scored per cell.
	const char* CellName();
}
