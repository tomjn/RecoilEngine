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
