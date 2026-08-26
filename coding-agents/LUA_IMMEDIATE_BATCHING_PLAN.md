# LuaOpenGL immediate-mode batching implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Collect each `gl.BeginEnd` block into memory and draw it with one `glDrawArrays`, instead of one `glVertex4f` a vertex.

**Architecture:** A pure accumulator, `LuaImmediateBatch`, holds one vector per attribute and produces a plain `BatchLayout` describing what to bind. A thin `IssueBatch` in `LuaOpenGL.cpp` turns that into `glVertexPointer` and `glDrawArrays` calls. Vertex specification stays fixed-function, so `glTexEnv`, lighting, fog and the legacy shader built-ins keep working. The accumulator never calls GL, so it tests under Catch2 with no context.

**Tech stack:** C++, CMake, Catch2, OpenGL compatibility profile, Lua 5.1 via the engine's own bindings.

**Spec:** [LUA_IMMEDIATE_BATCHING.md](LUA_IMMEDIATE_BATCHING.md)

**Branch:** `macos/lua-immediate-batching`, off `macos/integration`.

**Status, 2026-08-26.** Tasks 1 to 5 done. Task 6 turned out to be unnecessary and task 7 failed for a new reason. Task 8 done.

- Tasks 1 to 4 committed. Both engine targets build, 14 accumulator tests pass
- Task 5 passed. Buffering is indistinguishable from the old path, 50 differing pixels on the build menu against a same-configuration floor of 25
- Task 6 is moot. Buffering already skips the flush and the arity widening, and the engine's own `glBeginBatch` callers turned out not to be where the time goes
- Task 7 failed. Compiled lists reach 31.29 fps against 16.97, but render the build menu horizontally compressed. Reverted
- Task 8 done. Buffering is worth 5.7%. The deferral is worth 1.84x and is now the main lever

Four things the plan did not predict, all fixed or recorded: a colour set in a block that drew nothing was lost, the headless link was missing three vertex array pointer stubs, `Spring.GetProfilerTimeRecord` had two crash bugs that made the probe kill the engine 25 seconds into every run on any branch, and the build menu failure mode for compiled lists is a projection problem rather than a batching one.

## Global constraints

- The vertex pointer size never varies. Position is always `glVertexPointer(4, GL_FLOAT, 0, ...)` and every texture coordinate is always four floats. Varying it corrupts the frame on KosmicKrisp, 30 frames of 30
- `LuaImmediateBatch.{h,cpp}` must not include any GL header, so its test target compiles without glad. Use `uint32_t` for the primitive mode
- `CGlobalRendering::MAX_TEXTURE_UNITS` is 32. That is the texture unit ceiling `gl.MultiTexCoord` already enforces at `LuaOpenGL.cpp:2734`
- Config gate is `LuaImmediateModeBuffering`, default true. Setting it false restores the `glBegin` path exactly
- Commits go feature-first. Tasks 1 to 4 are the portable feature and become the upstream pull request. Tasks 6 and 7 delete integration-only mitigations and stay behind
- No engine runs before task 5. Compiling and running tests and standalone probes is fine

## Where the code goes

| File | Responsibility |
|---|---|
| `rts/Lua/LuaImmediateBatch.h` (new) | `BatchLayout` and the `LuaImmediateBatch` interface. No GL |
| `rts/Lua/LuaImmediateBatch.cpp` (new) | Accumulation, attribute activation, back-fill. No GL |
| `test/engine/Lua/testLuaImmediateBatch.cpp` (new) | Catch2 tests for both of the above |
| `rts/Lua/CMakeLists.txt` | Add the new source |
| `test/CMakeLists.txt` | Register the `LuaImmediateBatch` test target |
| `rts/Lua/LuaOpenGL.cpp` | `IssueBatch`, the config, the attribute funnels, the six block sites, the inside-batch guard |

---

### Task 1: The accumulator, position only

Position-only blocks are the whole of `gl.BeginEnd` for a widget that draws untextured lines, which is the majority case. Getting nesting and reset right here means task 2 only adds attributes.

**Files:**
- Create: `rts/Lua/LuaImmediateBatch.h`
- Create: `rts/Lua/LuaImmediateBatch.cpp`
- Create: `test/engine/Lua/testLuaImmediateBatch.cpp`
- Modify: `rts/Lua/CMakeLists.txt:33`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces: `struct BatchLayout`, `class LuaImmediateBatch` with `bool Active() const`, `void Begin(uint32_t mode)`, `void Vertex(float x, float y, float z, float w)`, `bool End(BatchLayout& out)`

- [ ] **Step 1: Write the failing test**

Create `test/engine/Lua/testLuaImmediateBatch.cpp`:

```cpp
#include "Lua/LuaImmediateBatch.h"

#include <catch_amalgamated.hpp>

TEST_CASE("a block collects its vertices", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;

	CHECK_FALSE(batch.Active());

	batch.Begin(7); // GL_QUADS
	CHECK(batch.Active());

	batch.Vertex(1.0f, 2.0f, 3.0f, 1.0f);
	batch.Vertex(4.0f, 5.0f, 6.0f, 1.0f);

	REQUIRE(batch.End(layout));
	CHECK_FALSE(batch.Active());

	CHECK(layout.mode == 7);
	CHECK(layout.count == 2);
	REQUIRE(layout.pos != nullptr);
	CHECK(layout.pos[0] == 1.0f);
	CHECK(layout.pos[3] == 1.0f);
	CHECK(layout.pos[4] == 4.0f);
	CHECK(layout.pos[6] == 6.0f);
}

TEST_CASE("an empty block reports no vertices", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;

	batch.Begin(1); // GL_LINES
	REQUIRE(batch.End(layout));

	CHECK(layout.count == 0);
}

TEST_CASE("a nested block joins the outer one", "[LuaImmediateBatch]")
{
	// Today a nested gl.BeginEnd errors on the inner glBegin, its vertices join
	// the outer primitive, and the inner glEnd ends the outer one. This keeps the
	// first two behaviours and fixes the third, so a widget that draws something
	// today keeps drawing it.
	LuaImmediateBatch batch;
	BatchLayout layout;

	batch.Begin(4); // GL_TRIANGLES
	batch.Vertex(1.0f, 0.0f, 0.0f, 1.0f);

	batch.Begin(1); // ignored
	batch.Vertex(2.0f, 0.0f, 0.0f, 1.0f);
	CHECK_FALSE(batch.End(layout)); // inner End does not finish the block
	CHECK(batch.Active());

	batch.Vertex(3.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	CHECK(layout.mode == 4);
	CHECK(layout.count == 3);
}

TEST_CASE("a second block does not inherit the first", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;

	batch.Begin(7);
	batch.Vertex(1.0f, 1.0f, 1.0f, 1.0f);
	batch.Vertex(2.0f, 2.0f, 2.0f, 1.0f);
	REQUIRE(batch.End(layout));

	batch.Begin(1);
	batch.Vertex(9.0f, 9.0f, 9.0f, 1.0f);
	REQUIRE(batch.End(layout));

	CHECK(layout.mode == 1);
	CHECK(layout.count == 1);
	CHECK(layout.pos[0] == 9.0f);
}
```

- [ ] **Step 2: Register the test target**

In `test/CMakeLists.txt`, after the `Transform` block that ends at line 257, add:

```cmake
################################################################################
### LuaImmediateBatch
	set(test_name LuaImmediateBatch)
	set(test_src
			"${CMAKE_CURRENT_SOURCE_DIR}/engine/Lua/testLuaImmediateBatch.cpp"
			"${ENGINE_SOURCE_DIR}/Lua/LuaImmediateBatch.cpp"
			${test_Common_sources}
		)

	set(test_libs
			${WINMM_LIBRARY}
		)

	add_spring_test(${test_name} "${test_src}" "${test_libs}" "-DNOT_USING_CREG -DBUILDING_AI")
```

- [ ] **Step 3: Run the test to verify it fails**

```sh
cmake --build build-macos-legacy -j 10 --target test_LuaImmediateBatch
```

Expected: the build fails with `Lua/LuaImmediateBatch.h` not found.

- [ ] **Step 4: Write the header**

Create `rts/Lua/LuaImmediateBatch.h`:

```cpp
/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

#ifndef LUA_IMMEDIATE_BATCH_H
#define LUA_IMMEDIATE_BATCH_H

#include <cstdint>
#include <vector>

// Deliberately no GL header. Everything interesting about a block is decided
// here and tested without a context, and LuaOpenGL turns the result into calls.

// One finished block. Every pointer is either null, meaning the array stays
// disabled and the draw uses the current GL state, or points at count entries.
struct BatchLayout {
	static constexpr int MAX_TEX_UNITS = 32; // CGlobalRendering::MAX_TEXTURE_UNITS

	uint32_t mode = 0;
	int32_t count = 0;

	const float* pos = nullptr;      // 4 floats a vertex, always
	const float* color = nullptr;    // 4
	const float* normal = nullptr;   // 3
	const float* secColor = nullptr; // 3
	const float* fogCoord = nullptr; // 1
	const uint8_t* edgeFlag = nullptr; // 1

	const float* texCoord[MAX_TEX_UNITS] = {}; // 4 each
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
```

- [ ] **Step 5: Write the implementation**

Create `rts/Lua/LuaImmediateBatch.cpp`:

```cpp
/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

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
	out.pos = count > 0 ? pos.data() : nullptr;

	return true;
}
```

- [ ] **Step 6: Run the test to verify it passes**

```sh
cmake --build build-macos-legacy -j 10 --target test_LuaImmediateBatch
build-macos-legacy/test/test_LuaImmediateBatch
```

Expected: `All tests passed`.

- [ ] **Step 7: Add the source to the engine build**

In `rts/Lua/CMakeLists.txt`, after line 26 (`LuaIntro.cpp`), add in alphabetical position:

```cmake
		"${CMAKE_CURRENT_SOURCE_DIR}/LuaImmediateBatch.cpp"
```

- [ ] **Step 8: Verify the engine still builds**

```sh
cmake --build build-macos-legacy -j 10 --target engine-legacy
```

Expected: builds clean. Nothing calls the new class yet.

- [ ] **Step 9: Commit**

```sh
git add rts/Lua/LuaImmediateBatch.h rts/Lua/LuaImmediateBatch.cpp rts/Lua/CMakeLists.txt test/engine/Lua/testLuaImmediateBatch.cpp test/CMakeLists.txt
git commit -m "Add a vertex accumulator for Lua immediate mode

Holds no GL state and calls no GL, so the awkward parts, nesting and reset, are testable without a context. Nothing uses it yet."
```

---

### Task 2: Optional attributes, activation and back-fill

**Files:**
- Modify: `rts/Lua/LuaImmediateBatch.h`
- Modify: `rts/Lua/LuaImmediateBatch.cpp`
- Modify: `test/engine/Lua/testLuaImmediateBatch.cpp`

**Interfaces:**
- Consumes: `BatchLayout` and `LuaImmediateBatch` from task 1
- Produces: `enum class BatchAttr`, `using DefaultFetch = void (*)(BatchAttr, unsigned, float*)`, `void SetDefaultFetch(DefaultFetch)`, `void Color(const float*)`, `void Normal(float, float, float)`, `void TexCoord(unsigned, float, float, float, float)`, `void SecondaryColor(float, float, float)`, `void FogCoord(float)`, `void EdgeFlag(bool)`

An attribute array is enabled only if the block set it. A widget that never calls `gl.Color` leaves `GL_COLOR_ARRAY` disabled and the draw picks up the current colour, which is what immediate mode does.

When a block sets an attribute after it has already emitted vertices, the earlier vertices need a value too. `DefaultFetch` supplies the value the attribute held when the block began. It is a function pointer so tests can stub it and the accumulator stays free of GL.

- [ ] **Step 1: Write the failing tests**

Append to `test/engine/Lua/testLuaImmediateBatch.cpp`:

```cpp
// Stands in for glGetFloatv. Writes a recognisable value so a back-filled entry
// can be told apart from one the block set.
static void StubDefaults(BatchAttr attr, unsigned unit, float* out)
{
	switch (attr) {
		case BatchAttr::Color:          out[0] = 0.5f; out[1] = 0.5f; out[2] = 0.5f; out[3] = 0.5f; break;
		case BatchAttr::Normal:         out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f; break;
		case BatchAttr::SecondaryColor: out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; break;
		case BatchAttr::FogCoord:       out[0] = 0.0f; break;
		case BatchAttr::EdgeFlag:       out[0] = 1.0f; break;
		case BatchAttr::TexCoord:       out[0] = float(unit); out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f; break;
	}
}

TEST_CASE("an untouched attribute stays disabled", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;

	batch.Begin(7);
	batch.Vertex(0.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	CHECK(layout.color == nullptr);
	CHECK(layout.normal == nullptr);
	CHECK(layout.texCoord[0] == nullptr);
}

TEST_CASE("an attribute set before the first vertex needs no back-fill", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;
	batch.SetDefaultFetch(StubDefaults);

	const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

	batch.Begin(7);
	batch.Color(red);
	batch.Vertex(0.0f, 0.0f, 0.0f, 1.0f);
	batch.Vertex(1.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	REQUIRE(layout.color != nullptr);
	CHECK(layout.color[0] == 1.0f);
	CHECK(layout.color[4] == 1.0f); // second vertex holds the same colour
}

TEST_CASE("an attribute set late back-fills the earlier vertices", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;
	batch.SetDefaultFetch(StubDefaults);

	const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

	batch.Begin(7);
	batch.Vertex(0.0f, 0.0f, 0.0f, 1.0f);
	batch.Vertex(1.0f, 0.0f, 0.0f, 1.0f);
	batch.Color(red);
	batch.Vertex(2.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	REQUIRE(layout.color != nullptr);
	CHECK(layout.count == 3);
	CHECK(layout.color[0] == 0.5f);  // back-filled from the stub
	CHECK(layout.color[4] == 0.5f);  // back-filled
	CHECK(layout.color[8] == 1.0f);  // set by the block
}

TEST_CASE("a colour persists until it changes", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;
	batch.SetDefaultFetch(StubDefaults);

	const float red[4]  = { 1.0f, 0.0f, 0.0f, 1.0f };
	const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

	batch.Begin(7);
	batch.Color(red);
	batch.Vertex(0.0f, 0.0f, 0.0f, 1.0f);
	batch.Vertex(1.0f, 0.0f, 0.0f, 1.0f);
	batch.Color(blue);
	batch.Vertex(2.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	CHECK(layout.color[0]  == 1.0f);
	CHECK(layout.color[4]  == 1.0f);
	CHECK(layout.color[10] == 1.0f); // third vertex is blue, index 2 of 4
}

TEST_CASE("texture units are tracked separately", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;
	batch.SetDefaultFetch(StubDefaults);

	batch.Begin(7);
	batch.TexCoord(0, 0.25f, 0.5f, 0.0f, 1.0f);
	batch.TexCoord(3, 0.75f, 0.5f, 0.0f, 1.0f);
	batch.Vertex(0.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	REQUIRE(layout.texCoord[0] != nullptr);
	REQUIRE(layout.texCoord[3] != nullptr);
	CHECK(layout.texCoord[1] == nullptr);
	CHECK(layout.texCoord[0][0] == 0.25f);
	CHECK(layout.texCoord[3][0] == 0.75f);
	CHECK(layout.texCoord[0][3] == 1.0f);
}

TEST_CASE("every attribute kind round-trips", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;
	batch.SetDefaultFetch(StubDefaults);

	const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	batch.Begin(7);
	batch.Color(white);
	batch.Normal(1.0f, 0.0f, 0.0f);
	batch.SecondaryColor(0.1f, 0.2f, 0.3f);
	batch.FogCoord(0.75f);
	batch.EdgeFlag(false);
	batch.Vertex(0.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	REQUIRE(layout.normal   != nullptr);
	REQUIRE(layout.secColor != nullptr);
	REQUIRE(layout.fogCoord != nullptr);
	REQUIRE(layout.edgeFlag != nullptr);

	CHECK(layout.normal[0]   == 1.0f);
	CHECK(layout.secColor[2] == 0.3f);
	CHECK(layout.fogCoord[0] == 0.75f);
	CHECK(layout.edgeFlag[0] == 0);
}

TEST_CASE("attributes do not leak into the next block", "[LuaImmediateBatch]")
{
	LuaImmediateBatch batch;
	BatchLayout layout;
	batch.SetDefaultFetch(StubDefaults);

	const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

	batch.Begin(7);
	batch.Color(red);
	batch.Vertex(0.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	batch.Begin(1);
	batch.Vertex(0.0f, 0.0f, 0.0f, 1.0f);
	REQUIRE(batch.End(layout));

	CHECK(layout.color == nullptr);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```sh
cmake --build build-macos-legacy -j 10 --target test_LuaImmediateBatch
```

Expected: the build fails with `BatchAttr` and `SetDefaultFetch` not declared.

- [ ] **Step 3: Extend the header**

In `rts/Lua/LuaImmediateBatch.h`, add above `class LuaImmediateBatch`:

```cpp
enum class BatchAttr { Color, Normal, SecondaryColor, FogCoord, EdgeFlag, TexCoord };

// Supplies the value an attribute held when the block began, for back-filling
// vertices emitted before the block first set it. Writes up to four floats.
// EdgeFlag writes 0 or 1 into out[0]. Called at most once per attribute a block,
// and only on the late-set path, so the glGetFloatv behind it stays off the hot
// path.
using DefaultFetch = void (*)(BatchAttr attr, unsigned unit, float* out);
```

Replace the body of `class LuaImmediateBatch` with:

```cpp
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
```

- [ ] **Step 4: Extend the implementation**

Replace `rts/Lua/LuaImmediateBatch.cpp` with:

```cpp
/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

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
			edgeBytes.push_back(f != 0.0f ? 1 : 0);

		out.edgeFlag = edgeBytes.data();
	}

	for (unsigned unit: usedTexUnits)
		out.texCoord[unit] = texCoord[unit].data.data();

	return true;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```sh
cmake --build build-macos-legacy -j 10 --target test_LuaImmediateBatch
build-macos-legacy/test/test_LuaImmediateBatch
```

Expected: `All tests passed`.

- [ ] **Step 6: Commit**

```sh
git add rts/Lua/LuaImmediateBatch.h rts/Lua/LuaImmediateBatch.cpp test/engine/Lua/testLuaImmediateBatch.cpp
git commit -m "Track optional attributes in the Lua vertex accumulator

An array is enabled only if the block set it, so a widget that never calls gl.Color pays nothing and the draw picks up the current colour, which is what immediate mode does.

Setting an attribute part way through a block back-fills the vertices already emitted. The value comes through a function pointer rather than a glGetFloatv call so the accumulator stays testable and the sync point stays off the hot path."
```

---

### Task 3: Replay through GL, behind a config

**Files:**
- Modify: `rts/Lua/LuaOpenGL.cpp:86` for the config, `:253-323` for the funnels, `:2264`, `:2287`, `:2412`, `:2464`, `:2952`, `:2966` for the block sites

**Interfaces:**
- Consumes: `LuaImmediateBatch`, `BatchLayout`, `BatchAttr`, `DefaultFetch` from tasks 1 and 2
- Produces: `static LuaImmediateBatch luaBatch`, `static void IssueBatch(const BatchLayout&)`, `static void LuaBatchBegin(GLenum)`, `static void LuaBatchEnd()`, and funnels `LuaColor4fv`, `LuaNormal3f`, `LuaSecondaryColor3f`, `LuaFogCoordf`, `LuaEdgeFlag`

The interception layer is small because `LuaVertexN`, `LuaTexCoordN` and `LuaMultiTexCoordN` are already funnels every caller goes through, including `DrawGroundQuad` at `:2270` and `:2293`. The remaining attributes need funnels of their own.

- [ ] **Step 1: Add the config**

In `rts/Lua/LuaOpenGL.cpp`, after line 86:

```cpp
CONFIG(bool, LuaImmediateModeBuffering).defaultValue(true).description("Collect each gl.BeginEnd block and draw it with one glDrawArrays instead of one call a vertex. Set to false to restore the glBegin path.");
```

Add a file-scope static beside the other LuaOpenGL statics:

```cpp
static bool luaImmediateBuffering = true;
static LuaImmediateBatch luaBatch;
```

Read it in `LuaOpenGL::Init()` at line 332, beside `canUseShaders`:

```cpp
	luaImmediateBuffering = configHandler->GetBool("LuaImmediateModeBuffering");
	luaBatch.SetDefaultFetch(FetchBatchDefault);
```

Add the include at the top with the other Lua headers:

```cpp
#include "LuaImmediateBatch.h"
```

- [ ] **Step 2: Write the default fetch and the issue function**

Add above `LuaTexCoordNarrow` at line 253:

```cpp
// Reads the value an attribute held when a block began, for back-filling. Only
// reached when a block sets an attribute after emitting vertices, so the
// glGetFloatv sync point is off the hot path.
static void FetchBatchDefault(BatchAttr attr, unsigned unit, float* out)
{
	switch (attr) {
		case BatchAttr::Color:          glGetFloatv(GL_CURRENT_COLOR, out); break;
		case BatchAttr::Normal:         glGetFloatv(GL_CURRENT_NORMAL, out); break;
		case BatchAttr::SecondaryColor: glGetFloatv(GL_CURRENT_SECONDARY_COLOR, out); break;
		case BatchAttr::FogCoord:       glGetFloatv(GL_CURRENT_FOG_COORD, out); break;
		case BatchAttr::TexCoord: {
			GLint prevUnit = GL_TEXTURE0;
			glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
			glActiveTexture(GL_TEXTURE0 + unit);
			glGetFloatv(GL_CURRENT_TEXTURE_COORDS, out);
			glActiveTexture(prevUnit);
		} break;
		case BatchAttr::EdgeFlag: {
			GLboolean flag = GL_TRUE;
			glGetBooleanv(GL_EDGE_FLAG, &flag);
			out[0] = (flag == GL_TRUE) ? 1.0f : 0.0f;
		} break;
	}
}

// Turns a finished block into one draw. Every pointer size is fixed, because
// varying glVertexPointer's size between draws corrupts the frame on
// KosmicKrisp, measured at 30 frames of 30.
static void IssueBatch(const BatchLayout& b)
{
	if (b.count == 0)
		return;

	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(4, GL_FLOAT, 0, b.pos);

	if (b.color != nullptr) {
		glEnableClientState(GL_COLOR_ARRAY);
		glColorPointer(4, GL_FLOAT, 0, b.color);
	}
	if (b.normal != nullptr) {
		glEnableClientState(GL_NORMAL_ARRAY);
		glNormalPointer(GL_FLOAT, 0, b.normal);
	}
	if (b.secColor != nullptr) {
		glEnableClientState(GL_SECONDARY_COLOR_ARRAY);
		glSecondaryColorPointer(3, GL_FLOAT, 0, b.secColor);
	}
	if (b.fogCoord != nullptr) {
		glEnableClientState(GL_FOG_COORD_ARRAY);
		glFogCoordPointer(GL_FLOAT, 0, b.fogCoord);
	}
	if (b.edgeFlag != nullptr) {
		glEnableClientState(GL_EDGE_FLAG_ARRAY);
		glEdgeFlagPointer(0, b.edgeFlag);
	}

	int lastTexUnit = -1;

	for (int unit = 0; unit < BatchLayout::MAX_TEX_UNITS; unit++) {
		if (b.texCoord[unit] == nullptr)
			continue;

		glClientActiveTexture(GL_TEXTURE0 + unit);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(4, GL_FLOAT, 0, b.texCoord[unit]);
		lastTexUnit = unit;
	}

	glDrawArrays(b.mode, 0, b.count);

	for (int unit = 0; unit <= lastTexUnit; unit++) {
		if (b.texCoord[unit] == nullptr)
			continue;

		glClientActiveTexture(GL_TEXTURE0 + unit);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}

	if (lastTexUnit >= 0)
		glClientActiveTexture(GL_TEXTURE0);

	if (b.edgeFlag != nullptr) glDisableClientState(GL_EDGE_FLAG_ARRAY);
	if (b.fogCoord != nullptr) glDisableClientState(GL_FOG_COORD_ARRAY);
	if (b.secColor != nullptr) glDisableClientState(GL_SECONDARY_COLOR_ARRAY);
	if (b.normal   != nullptr) glDisableClientState(GL_NORMAL_ARRAY);
	if (b.color    != nullptr) glDisableClientState(GL_COLOR_ARRAY);

	glDisableClientState(GL_VERTEX_ARRAY);
}

// The two calls that replace glBeginBatch and glEnd at the six Lua block sites.
static void LuaBatchBegin(GLenum mode)
{
	if (!luaImmediateBuffering)
		return glBeginBatch(mode);

	luaBatch.Begin(mode);
}

static void LuaBatchEnd()
{
	if (!luaImmediateBuffering)
		return glEnd();

	BatchLayout layout;

	if (!luaBatch.End(layout))
		return;

	IssueBatch(layout);

	// A colour set inside a block persists after it in real GL. One call at the
	// end reproduces that without paying for one a vertex.
	if (layout.color != nullptr)
		glColor4fv(&layout.color[(layout.count - 1) * 4]);
}
```

- [ ] **Step 3: Route the existing funnels through the batch**

In `LuaTexCoordN` at line 271, `LuaMultiTexCoordN` at line 293 and `LuaVertexN` at line 313, add a buffering branch at the top of each. `LuaVertexN` becomes:

```cpp
static inline void LuaVertexN(float x, float y, float z, float w, int arity)
{
	if (luaBatch.Active())
		return luaBatch.Vertex(x, y, z, w);

	// Gated on the measured capability, the same way the flush is, so a driver
	// that renders batches correctly pays nothing. Widening every vertex to four
	// floats is cheap but it is on the hot path for all Lua drawing, and no other
	// platform should carry it for a defect it does not have.
	if (globalRendering->supportImmediateModeBatching)
		return glVertexNarrow(x, y, z, w, arity);

	glVertex4f(x, y, z, w);
}
```

`LuaTexCoordN` gains `if (luaBatch.Active()) return luaBatch.TexCoord(0, x, y, z, w);` and `LuaMultiTexCoordN` gains `if (luaBatch.Active()) return luaBatch.TexCoord(unit - GL_TEXTURE0, x, y, z, w);`, each as the first statement.

- [ ] **Step 4: Add funnels for the remaining attributes**

Add beside the others:

```cpp
static inline void LuaColor4fv(const float* rgba)
{
	if (luaBatch.Active())
		return luaBatch.Color(rgba);

	glColor4fv(rgba);
}

static inline void LuaNormal3f(float x, float y, float z)
{
	if (luaBatch.Active())
		return luaBatch.Normal(x, y, z);

	glNormal3f(x, y, z);
}

static inline void LuaSecondaryColor3f(float r, float g, float b)
{
	if (luaBatch.Active())
		return luaBatch.SecondaryColor(r, g, b);

	glSecondaryColor3f(r, g, b);
}

static inline void LuaFogCoordf(float f)
{
	if (luaBatch.Active())
		return luaBatch.FogCoord(f);

	glFogCoordf(f);
}

static inline void LuaEdgeFlag(bool e)
{
	if (luaBatch.Active())
		return luaBatch.EdgeFlag(e);

	glEdgeFlag(e);
}
```

Replace the call sites:

| Line | Was | Becomes |
|---|---|---|
| 2424 | `glColor4fv(vd.color)` | `LuaColor4fv(vd.color)` |
| 2426 | `glNormal3fv(vd.norm)` | `LuaNormal3f(vd.norm[0], vd.norm[1], vd.norm[2])` |
| 2597 | `glNormal3f(x, y, z)` | `LuaNormal3f(x, y, z)` |
| 2604 | `glNormal3f(x, y, z)` | `LuaNormal3f(x, y, z)` |
| 2838 | `glSecondaryColor3f(x, y, z)` | `LuaSecondaryColor3f(x, y, z)` |
| 2845 | `glSecondaryColor3f(x, y, z)` | `LuaSecondaryColor3f(x, y, z)` |
| 2860 | `glFogCoordf(value)` | `LuaFogCoordf(value)` |
| 2875 | `glEdgeFlag(lua_toboolean(L, 1))` | `LuaEdgeFlag(lua_toboolean(L, 1) != 0)` |
| 3084 | `glColor4fv(color.data())` | `LuaColor4fv(color.data())` |

- [ ] **Step 5: Convert the six block sites**

Replace `glBeginBatch(...)` with `LuaBatchBegin(...)` and its matching `glEnd()` with `LuaBatchEnd()` at these pairs:

| Function | Begin | End |
|---|---|---|
| `DrawGroundQuad`, untextured | 2264 | 2273 |
| `DrawGroundQuad`, textured | 2287 | 2299 |
| `Shape` | 2412 | 2434 |
| `BeginEnd` | 2464 | 2466 |
| `TexRect`, first form | 2952 | 2958 |
| `TexRect`, second form | 2966 | 2972 |

Leave `glBeginBatch` itself alone. It still serves `GuiHandler.cpp`, `HUDDrawer.cpp`, `HAPFSPathDrawer.cpp`, `DynWater.cpp`, `Combiner.cpp` and `GeometryBuffer.cpp`, which are a separate job.

- [ ] **Step 6: Build**

```sh
cmake --build build-macos-legacy -j 10 --target engine-legacy
```

Expected: builds clean.

- [ ] **Step 7: Confirm the accumulator tests still pass**

```sh
cmake --build build-macos-legacy -j 10 --target test_LuaImmediateBatch
build-macos-legacy/test/test_LuaImmediateBatch
```

Expected: `All tests passed`.

- [ ] **Step 8: Commit**

```sh
git add rts/Lua/LuaOpenGL.cpp
git commit -m "Draw each gl.BeginEnd block with one glDrawArrays

Vertex specification stays fixed-function, which is what keeps glTexEnv, lighting, fog and the gl_Vertex and gl_MultiTexCoord0 built-ins working for widgets that bind their own shader.

The pointer sizes are fixed and never vary. Changing glVertexPointer's size between draws corrupts the frame on KosmicKrisp, which is legal GL the driver gets wrong.

LuaImmediateModeBuffering=0 restores the old path, which is how both sides fit inside one measured run."
```

---

### Task 4: Reject state changes inside a block

**Files:**
- Modify: `rts/Lua/LuaOpenGL.cpp:1176`

Today a widget calling `gl.Texture` between two `gl.Vertex` calls gets a silently ignored `GL_INVALID_OPERATION`, so the texture does not change. Buffered, it would change and apply to the whole primitive. This keeps the current behaviour.

**Interfaces:**
- Consumes: `luaBatch` from task 3
- Produces: `static bool CheckNotInsideBatch(lua_State*, const char*)`

- [ ] **Step 1: Add the guard**

Replace `CheckDrawingEnabled` at line 1176 with:

```cpp
inline void LuaOpenGL::CheckDrawingEnabled(lua_State* L, const char* caller)
{
	if (!IsDrawingEnabled(L)) {
		luaL_error(L, "%s(): OpenGL calls can only be used in Draw() "
		              "call-ins, or while creating display lists", caller);
	}
}

// The GL specification allows only per-vertex attribute calls between glBegin
// and glEnd, and rejects everything else with GL_INVALID_OPERATION. Buffering a
// block would otherwise let a state change that has no effect today take effect
// for the whole primitive. Returns true when the caller should do nothing.
inline bool LuaOpenGL::InsideBatch()
{
	return luaBatch.Active();
}
```

- [ ] **Step 2: Declare it**

In `rts/Lua/LuaOpenGL.h` beside `CheckDrawingEnabled` at line 166:

```cpp
		static bool InsideBatch();
```

- [ ] **Step 3: Guard the state-changing entry points**

Add `if (InsideBatch()) return 0;` immediately after `CheckDrawingEnabled` in every `LuaOpenGL::` function that calls it, except these, which the specification allows inside a block and which must stay live:

`Vertex`, `Color`, `Normal`, `TexCoord`, `MultiTexCoord`, `SecondaryColor`, `FogCoord`, `EdgeFlag`, `Shape`, `BeginEnd`

Find the full list with:

```sh
grep -n "CheckDrawingEnabled(L, __func__)" rts/Lua/LuaOpenGL.cpp
```

- [ ] **Step 4: Build**

```sh
cmake --build build-macos-legacy -j 10 --target engine-legacy
```

Expected: builds clean.

- [ ] **Step 5: Commit**

```sh
git add rts/Lua/LuaOpenGL.cpp rts/Lua/LuaOpenGL.h
git commit -m "Ignore state changes inside a gl.BeginEnd block

The driver already ignores them with GL_INVALID_OPERATION, so this keeps what widgets see rather than changing it. Without it, buffering would turn a call that does nothing today into one that applies to a whole primitive.

gl.CallList is rejected too. The specification allows it only when the list holds nothing but per-vertex calls, and modelling that is not worth what it would buy."
```

---

## Blocked on an engine run

Everything above compiles and tests without starting the engine. Everything below needs a run, which has to wait.

### Task 5: Prove the output is identical

Frozen scene, both paths, two games. This is where the real risk is.

```sh
./coding-agents/test-scripts/install-probe.sh --shots 20,40
SPRING_FPS_LOG=5 coding-agents/test-scripts/run-capped.sh 60 ~/dev/spring-testdata/logs/buffered.log
# then again with LuaImmediateModeBuffering=0 in a copied config
./coding-agents/test-scripts/install-probe.sh --remove
```

Repeat with `GAME=` and `SCRIPT=` pointed at Metal Factions. Pass `--config` a copy so the shared `springsettings.cfg` stays clean.

**Check:** the screenshots match between the two paths, on both games.

### Task 6: Retire the flush and the arity widening

Once buffering is on, the Lua path emits no `glBegin` batches, so `LuaVertexN`, `LuaTexCoordN` and `LuaMultiTexCoordN` no longer need to widen and `glBeginBatch` no longer needs its flush for Lua.

**Check:** `install-probe.sh --loops 2000` then `minimap_score.py <log> --amp` scores 0 dirty frames.

### Task 7: Retire the display list deferral

Remove the deferral at `LuaOpenGL.cpp:6217` and the 8192-list cap, and let `gl.CreateList` compile again.

**Check:** scroll the build menu, 0 of 37 frames with damaged content. That is the bar the deferral cleared.

### Task 8: Measure

```sh
SPRING_FPS_LOG=5 coding-agents/test-scripts/run-capped.sh 120 ~/dev/spring-testdata/logs/measured.log
```

**Check:** a frame time figure with the Mesa commit beside it, taken interleaved rather than between runs.
