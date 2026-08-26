/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaImmediateBatch.h"

void LuaImmediateBatch::Begin(uint32_t m)
{
	if (depth++ > 0)
		return;

	mode = m;
	count = 0;

	// clear() keeps the capacity, so a steady state of similar blocks stops
	// allocating after the first few frames.
	pos.clear();
}

void LuaImmediateBatch::Vertex(float x, float y, float z, float w)
{
	if (depth == 0)
		return;

	pos.push_back(x);
	pos.push_back(y);
	pos.push_back(z);
	pos.push_back(w);

	count++;
}

bool LuaImmediateBatch::End(BatchLayout& out)
{
	if (depth == 0)
		return false;

	if (--depth > 0)
		return false;

	out = BatchLayout{};
	out.mode = mode;
	out.count = count;
	out.pos = (count > 0) ? pos.data() : nullptr;

	return true;
}
