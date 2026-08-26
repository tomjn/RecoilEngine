/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef LUA_IMMEDIATE_BATCH_H
#define LUA_IMMEDIATE_BATCH_H

#include <cstdint>
#include <vector>

// Collects one gl.BeginEnd block so it can be drawn with a single glDrawArrays
// instead of one call a vertex.
//
// Deliberately includes no GL header. Everything worth getting wrong, nesting,
// reset, and which arrays a block actually needs, is decided here and tested
// without a context. LuaOpenGL turns the result into calls.

// One finished block. Every pointer is either null, meaning the array stays
// disabled and the draw picks up the current GL state, or points at count
// entries.
struct BatchLayout {
	static constexpr int MAX_TEX_UNITS = 32; // CGlobalRendering::MAX_TEXTURE_UNITS

	uint32_t mode = 0;
	int32_t count = 0;

	const float* pos = nullptr; // 4 floats a vertex, always
};

class LuaImmediateBatch {
public:
	bool Active() const { return depth > 0; }

	// A nested Begin is ignored and its vertices join the outer block.
	void Begin(uint32_t mode);

	void Vertex(float x, float y, float z, float w);

	// Returns true and fills out only when this ends the outermost block.
	bool End(BatchLayout& out);

private:
	int depth = 0;
	uint32_t mode = 0;
	int32_t count = 0;

	std::vector<float> pos;
};

#endif
