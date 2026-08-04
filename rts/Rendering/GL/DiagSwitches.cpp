/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

// TEMP diagnostic, not for merge. See DiagSwitches.h for why this exists.

#include "DiagSwitches.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System/Log/ILog.h"

namespace {
	using DiagSwitches::Switch;

	struct Named {
		const char* name;
		Switch sw;
		const char* env;
	};

	const Named SWITCHES[] = {
		{ "flush",     DiagSwitches::BATCH_FLUSH,    "SPRING_BATCH_FLUSH"    },
		{ "narrow",    DiagSwitches::BATCH_NARROW,   "SPRING_BATCH_NARROW"   },
		{ "noflush",   DiagSwitches::NO_BATCH_FLUSH, "SPRING_NO_BATCH_FLUSH" },
		{ "nopresent", DiagSwitches::NO_PRESENT,     "SPRING_NO_PRESENT"     },
		{ "finish",    DiagSwitches::FRAME_FINISH,   "SPRING_FRAME_FINISH"   },
		{ "throttle",  DiagSwitches::FRAME_THROTTLE, "SPRING_FRAME_THROTTLE" },
	};

	struct Cell {
		std::string label;
		unsigned int mask;
	};

	std::vector<Cell> cells;
	std::string parseError;
	double cellSeconds = 0.0;
	size_t cellIdx = 0;
	unsigned int cycle = 0;
	unsigned int cellFrames = 0;
	unsigned int gpuSamples = 0;
	double gpuMillisSum = 0.0;
	bool announced = false;

	std::chrono::steady_clock::time_point cellStart;

	unsigned int EnvMask()
	{
		unsigned int mask = 0;

		for (const Named& n: SWITCHES) {
			if (getenv(n.env) != nullptr)
				mask |= (1u << n.sw);
		}

		return mask;
	}

	bool LookUp(const std::string& name, unsigned int& mask)
	{
		for (const Named& n: SWITCHES) {
			if (name == n.name) {
				mask |= (1u << n.sw);
				return true;
			}
		}

		return false;
	}

	// An unrecognised switch name discards the whole schedule rather than being
	// skipped. A misspelt cell would otherwise measure the baseline twice and
	// read as "no difference", which is the failure this file exists to avoid.
	bool ParseCells()
	{
		const char* spec = getenv("SPRING_DIAG_CELLS");

		if (spec == nullptr)
			return false;

		const char* colon = strchr(spec, ':');

		if (colon == nullptr) {
			parseError = std::string("expected <seconds>:<cell>[/<cell>...], got ") + spec;
			return false;
		}

		cellSeconds = atof(std::string(spec, colon).c_str());

		if (cellSeconds <= 0.0) {
			parseError = std::string("cell length must be positive, got ") + spec;
			return false;
		}

		const std::string rest(colon + 1);

		for (size_t pos = 0; pos <= rest.size(); ) {
			const size_t slash = rest.find('/', pos);
			const std::string one = rest.substr(pos, (slash == std::string::npos) ? slash : slash - pos);

			if (one.empty()) {
				parseError = std::string("empty cell in ") + spec;
				return false;
			}

			unsigned int mask = 0;

			if (one != "-") {
				for (size_t p = 0; p <= one.size(); ) {
					const size_t comma = one.find(',', p);
					const std::string name = one.substr(p, (comma == std::string::npos) ? comma : comma - p);

					if (!LookUp(name, mask)) {
						parseError = std::string("unknown switch \"") + name + "\" in cell \"" + one + "\"";
						return false;
					}

					if (comma == std::string::npos)
						break;

					p = comma + 1;
				}
			}

			cells.push_back({ one, mask });

			if (slash == std::string::npos)
				break;

			pos = slash + 1;
		}

		return true;
	}

	// Parsed at static init so the first cell is already in force during load,
	// rather than the first present being the point the schedule starts.
	const bool haveSchedule = ParseCells();
}

// Defined after haveSchedule so the schedule wins over any leftover environment
// variable. Both together would let one stale variable contaminate every cell.
unsigned int DiagSwitches::currentMask = haveSchedule ? cells[0].mask : EnvMask();

unsigned long long DiagSwitches::batches = 0;
unsigned long long DiagSwitches::luaVerts = 0;
unsigned long long DiagSwitches::fboBinds = 0;

const char* DiagSwitches::CellName()
{
	if (cells.empty())
		return nullptr;

	return cells[cellIdx].label.c_str();
}

void DiagSwitches::FramePresented(double gpuMillis)
{
	if (!announced) {
		announced = true;
		cellStart = std::chrono::steady_clock::now();

		if (!parseError.empty())
			LOG_L(L_ERROR, "[diag] SPRING_DIAG_CELLS ignored: %s", parseError.c_str());

		if (haveSchedule) {
			LOG("[diag] schedule: %.1fs a cell, %u cells", cellSeconds, (unsigned) cells.size());

			for (const Cell& c: cells)
				LOG("[diag]   cell %s mask 0x%02x", c.label.c_str(), c.mask);

			const unsigned int env = EnvMask();

			if (env != 0u)
				LOG_L(L_WARNING, "[diag] environment mask 0x%02x overridden by the schedule", env);
		}
	}

	if (!haveSchedule)
		return;

	cellFrames++;

	if (gpuMillis >= 0.0) {
		gpuSamples++;
		gpuMillisSum += gpuMillis;
	}

	const auto now = std::chrono::steady_clock::now();
	const double elapsed = std::chrono::duration<double>(now - cellStart).count();

	if (elapsed < cellSeconds)
		return;

	// gpu= is the mean of the frame's own GL timestamp pair, so it is the GPU's
	// time to draw the frame rather than the CPU's time waiting for it.
	//
	// The counters are per frame rather than totals, because what the immediate
	// mode question needs is how much a frame issues, and because a total would
	// grow with the cell length and read differently for no reason.
	LOG("[diag] cycle=%u cell=%s fps=%.2f frames=%u over %.2fs gpu=%.2fms n=%u batches/f=%.0f luaverts/f=%.0f fbobinds/f=%.1f",
		cycle, cells[cellIdx].label.c_str(), cellFrames / elapsed, cellFrames, elapsed,
		(gpuSamples > 0) ? (gpuMillisSum / gpuSamples) : -1.0, gpuSamples,
		(cellFrames > 0) ? (double(batches) / cellFrames) : -1.0,
		(cellFrames > 0) ? (double(luaVerts) / cellFrames) : -1.0,
		(cellFrames > 0) ? (double(fboBinds) / cellFrames) : -1.0);

	cellFrames = 0;
	gpuSamples = 0;
	gpuMillisSum = 0.0;
	batches = 0;
	luaVerts = 0;
	fboBinds = 0;
	cellStart = now;

	if (++cellIdx == cells.size()) {
		cellIdx = 0;
		cycle++;
	}

	currentMask = cells[cellIdx].mask;
}
