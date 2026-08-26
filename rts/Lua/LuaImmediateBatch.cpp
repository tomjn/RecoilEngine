/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaImmediateBatch.h"

void LuaImmediateBatch::Activate(Track& t, BatchAttr attr, unsigned unit, int width)
{
	if (t.active)
		return;

	t.active = true;
	t.data.clear();

	if (count == 0)
		return;

	// The block emitted vertices before it set this attribute, so those vertices
	// carry whatever value was current when the block began.
	float prev[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (fetchDefault != nullptr)
		fetchDefault(attr, unit, prev);

	for (int32_t v = 0; v < count; v++) {
		for (int i = 0; i < width; i++)
			t.data.push_back(prev[i]);
	}
}

void LuaImmediateBatch::PushIfActive(Track& t, int width)
{
	if (!t.active)
		return;

	for (int i = 0; i < width; i++)
		t.data.push_back(t.value[i]);
}

void LuaImmediateBatch::Begin(uint32_t m)
{
	if (depth++ > 0)
		return;

	mode = m;
	count = 0;

	// clear() keeps the capacity, so a steady state of similar blocks stops
	// allocating after the first few frames.
	pos.clear();

	color.active = normal.active = secColor.active = false;
	fogCoord.active = edge.active = false;

	for (unsigned unit: usedTexUnits)
		texCoord[unit].active = false;

	usedTexUnits.clear();
}

void LuaImmediateBatch::Vertex(float x, float y, float z, float w)
{
	if (depth == 0)
		return;

	pos.push_back(x);
	pos.push_back(y);
	pos.push_back(z);
	pos.push_back(w);

	PushIfActive(color, 4);
	PushIfActive(normal, 3);
	PushIfActive(secColor, 3);
	PushIfActive(fogCoord, 1);
	PushIfActive(edge, 1);

	for (unsigned unit: usedTexUnits)
		PushIfActive(texCoord[unit], 4);

	count++;
}

void LuaImmediateBatch::Color(const float* rgba)
{
	if (depth == 0)
		return;

	Activate(color, BatchAttr::Color, 0, 4);

	for (int i = 0; i < 4; i++)
		color.value[i] = rgba[i];
}

void LuaImmediateBatch::Normal(float x, float y, float z)
{
	if (depth == 0)
		return;

	Activate(normal, BatchAttr::Normal, 0, 3);

	normal.value[0] = x;
	normal.value[1] = y;
	normal.value[2] = z;
}

void LuaImmediateBatch::TexCoord(unsigned unit, float s, float t, float r, float q)
{
	if (depth == 0 || unit >= BatchLayout::MAX_TEX_UNITS)
		return;

	Track& track = texCoord[unit];

	if (!track.active) {
		Activate(track, BatchAttr::TexCoord, unit, 4);
		usedTexUnits.push_back(unit);
	}

	track.value[0] = s;
	track.value[1] = t;
	track.value[2] = r;
	track.value[3] = q;
}

void LuaImmediateBatch::SecondaryColor(float r, float g, float b)
{
	if (depth == 0)
		return;

	Activate(secColor, BatchAttr::SecondaryColor, 0, 3);

	secColor.value[0] = r;
	secColor.value[1] = g;
	secColor.value[2] = b;
}

void LuaImmediateBatch::FogCoord(float f)
{
	if (depth == 0)
		return;

	Activate(fogCoord, BatchAttr::FogCoord, 0, 1);

	fogCoord.value[0] = f;
}

void LuaImmediateBatch::EdgeFlag(bool e)
{
	if (depth == 0)
		return;

	Activate(edge, BatchAttr::EdgeFlag, 0, 1);

	edge.value[0] = e ? 1.0f : 0.0f;
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

	// Reported whatever the vertex count, because the colour outlives the block.
	if (color.active) {
		out.colorSet = true;

		for (int i = 0; i < 4; i++)
			out.finalColor[i] = color.value[i];
	}

	if (count == 0)
		return true;

	out.pos = pos.data();

	if (color.active)    out.color    = color.data.data();
	if (normal.active)   out.normal   = normal.data.data();
	if (secColor.active) out.secColor = secColor.data.data();
	if (fogCoord.active) out.fogCoord = fogCoord.data.data();

	if (edge.active) {
		// glEdgeFlagPointer wants GLboolean, so this is the one attribute that
		// cannot hand its float storage straight to GL.
		edgeBytes.clear();
		edgeBytes.reserve(edge.data.size());

		for (float f: edge.data)
			edgeBytes.push_back((f != 0.0f) ? 1 : 0);

		out.edgeFlag = edgeBytes.data();
	}

	for (unsigned unit: usedTexUnits)
		out.texCoord[unit] = texCoord[unit].data.data();

	return true;
}
