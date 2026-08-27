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

	const float* pos = nullptr;        // 4 floats a vertex, always
	const float* color = nullptr;      // 4
	const float* normal = nullptr;     // 3
	const float* secColor = nullptr;   // 3
	const float* fogCoord = nullptr;   // 1
	const uint8_t* edgeFlag = nullptr; // 1

	const float* texCoord[MAX_TEX_UNITS] = {}; // 4 each

	// A colour set inside a block stays current after it in real GL, so it has to
	// be reported apart from the vertex array. A block can set a colour and then
	// emit no vertices, and that colour still has to survive.
	bool colorSet = false;
	float finalColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
};

enum class BatchAttr { Color, Normal, SecondaryColor, FogCoord, EdgeFlag, TexCoord };

// Supplies the value an attribute held when the block began, for back-filling
// vertices emitted before the block first set it. Writes up to four floats, and
// EdgeFlag writes 0 or 1 into out[0].
//
// Called at most once per attribute a block, and only on the late-set path, so
// the glGetFloatv behind it stays off the hot path.
using DefaultFetch = void (*)(BatchAttr attr, unsigned unit, float* out);

class LuaImmediateBatch {
public:
	bool Active() const { return depth > 0; }

	void SetDefaultFetch(DefaultFetch fn) { fetchDefault = fn; }

	// A nested Begin is ignored and its vertices join the outer block.
	void Begin(uint32_t mode);

	void Vertex(float x, float y, float z, float w);

	void Color(const float* rgba);
	void Normal(float x, float y, float z);
	void TexCoord(unsigned unit, float s, float t, float r, float q);
	void SecondaryColor(float r, float g, float b);
	void FogCoord(float f);
	void EdgeFlag(bool e);

	// Returns true and fills out only when this ends the outermost block.
	bool End(BatchLayout& out);

private:
	// One optional attribute. Holds the value the block last set, and grows its
	// vector by one entry a vertex once the block has touched it.
	struct Track {
		bool active = false;
		float value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		std::vector<float> data;
	};

	// Marks an attribute active and back-fills the vertices already emitted.
	void Activate(Track& t, BatchAttr attr, unsigned unit, int width);
	void PushIfActive(Track& t, int width);

	int depth = 0;
	uint32_t mode = 0;
	int32_t count = 0;

	DefaultFetch fetchDefault = nullptr;

	std::vector<float> pos;

	Track color;
	Track normal;
	Track secColor;
	Track fogCoord;
	Track edge;
	Track texCoord[BatchLayout::MAX_TEX_UNITS];

	// Which texture units this block touched, so End does not walk all 32.
	std::vector<unsigned> usedTexUnits;

	// Edge flags are stored as floats alongside the others and converted in End,
	// because glEdgeFlagPointer takes GLboolean and nothing else does.
	std::vector<uint8_t> edgeBytes;
};

#endif
