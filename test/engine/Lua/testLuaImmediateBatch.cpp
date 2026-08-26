/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

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
	CHECK(layout.color[0] == 0.5f); // back-filled from the stub
	CHECK(layout.color[4] == 0.5f); // back-filled
	CHECK(layout.color[8] == 1.0f); // set by the block
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
