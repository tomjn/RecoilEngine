// Does the KosmicKrisp batch merging defect also affect consecutive glDrawArrays?
//
// The engine separates every immediate-mode batch with a glFlush because two
// consecutive glBegin batches otherwise merge, and the second primitive is drawn
// joined to the first. That flush costs about 7.5% of the frame. The proposed fix
// is for LuaOpenGL to buffer each gl.BeginEnd block and replay it as one
// glDrawArrays, which only helps if glDrawArrays does not merge the same way.
// Nobody has tested that. This does, without an engine run.
//
// Draws a grid of 2000 identical LINE_LOOP squares with a gap between them. A
// merged batch draws a connecting line across a gap, so it lights pixels the
// correct image leaves black. Ground truth is the driver's own output with a
// flush between batches, which is the configuration measured clean.
//
// Read the positive control first. If "immediate, no separator" does not differ
// from the reference, the defect is not being exercised and nothing else in the
// run means anything.
//
//   PREFIX=$HOME/dev/mesa-install-premtl4
//   gcc -O1 -I$PREFIX/include -o /tmp/batch_merge_probe batch_merge_probe.c -L$PREFIX/lib -lEGL
//   DYLD_LIBRARY_PATH=/opt/homebrew/lib EGL_PLATFORM=surfaceless \
//     MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
//     LIBGL_DRIVERS_PATH=$PREFIX/lib \
//     VK_DRIVER_FILES=$PREFIX/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
//     MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460 \
//     /tmp/batch_merge_probe

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef void GLvoid;

#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_LINE_LOOP          0x0002
#define GL_QUADS              0x0007
#define GL_FLOAT              0x1406
#define GL_RGBA               0x1908
#define GL_UNSIGNED_BYTE      0x1401
#define GL_PACK_ALIGNMENT     0x0D05
#define GL_VERTEX_ARRAY       0x8074
#define GL_RENDERER           0x1F01
#define GL_VERSION            0x1F02
#define GL_DEPTH_TEST         0x0B71
#define GL_BLEND              0x0BE2
#define GL_CULL_FACE          0x0B44
#define GL_SRC_ALPHA          0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_COMPILE            0x1300
#define GL_TEXTURE_COORD_ARRAY 0x8078

static const GLubyte* (*p_glGetString)(GLenum);
static GLenum (*p_glGetError)(void);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glBegin)(GLenum);
static void (*p_glEnd)(void);
static void (*p_glVertex2f)(GLfloat, GLfloat);
static void (*p_glVertex4f)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glFinish)(void);
static void (*p_glFlush)(void);
static void (*p_glDisable)(GLenum);
static void (*p_glEnable)(GLenum);
static void (*p_glBlendFunc)(GLenum, GLenum);
static void (*p_glEnableClientState)(GLenum);
static void (*p_glDisableClientState)(GLenum);
static void (*p_glVertexPointer)(GLint, GLenum, GLsizei, const GLvoid*);
static void (*p_glDrawArrays)(GLenum, GLint, GLsizei);
static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
static void (*p_glPixelStorei)(GLenum, GLint);
static void (*p_glRectf)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glTexCoordPointer)(GLint, GLenum, GLsizei, const GLvoid*);
static GLuint (*p_glGenLists)(GLsizei);
static void (*p_glNewList)(GLuint, GLenum);
static void (*p_glEndList)(void);
static void (*p_glCallList)(GLuint);

#define LOAD(name) do { \
	p_##name = (void*) eglGetProcAddress(#name); \
	if (p_##name == NULL) printf("MISSING entry point: %s\n", #name); \
} while (0)

// The artefact amplifier's grid, which is the configuration already measured
// positive: 2000 identical 16 segment LINE_LOOP circles of radius 7, spaced 22
// pixels apart. Every number here is copied from widget_loop_amp.lua.
//
// The spacing carries the measurement. A merged pair draws a segment between two
// circle centres, so at 22 pixels it is long and cannot be confused with the
// circles. The measured median was 28 stray pixels a frame, which is about one
// merge event per frame, so the defect fires roughly once per 2000 adjacent
// pairs. That rate is why the grid has to be this large to see anything.
#define DIVS     16
#define RADIUS   7.0f
#define SPACING  22
#define MARGIN   20
#define COLS     55
#define BATCHES  2000
#define ROWS     ((BATCHES + COLS - 1) / COLS)

#define SURF_W 1280
#define SURF_H 900

// Two copies of the same geometry. The 4 float form is what the engine emits
// since the arity mitigation, the 2 float form is what it emitted before.
static float vert2[BATCHES * DIVS * 2];
static float vert4[BATCHES * DIVS * 4];

// One batch worth, refilled per draw for MODE_ARRAYS_REUSED.
static float scratch[DIVS * 4];

// A texture coordinate array to bind alongside the vertex array, because every
// case in this probe until now bound a vertex array and nothing else, while the
// UI drawing being hunted is textured. IssueBatch binds a texcoord array at size
// 4 whenever a block calls gl.TexCoord, which the game's RectRound does.
//
// Deliberately not the vertex data. It is a ring at 0.9 NDC, so if a draw ever
// takes its positions from this array instead of the vertex array, the frame
// gains a huge ring rather than a subtly wrong circle.
static float tex4[DIVS * 4];

static int withTexCoordArray = 0;

// Let the compiled draw point at its own storage instead of the shared scratch,
// so the shared buffer can be ruled in or out as a required ingredient.
static int midstreamOwnArray = 0;

// Radius of the ring in tex4, in NDC. A variable so the corruption can be shown
// to track it: if the wrecked geometry moves when this moves, the draw is taking
// its positions from the texture coordinate array, which is a demonstration
// rather than an inference.
static float texRingRadius = 0.9f;

// Rebind the texcoord pointer before every live draw rather than once for the
// run. IssueBatch already does exactly this, so if it fixes the mixed case the
// engine has a workaround, and if it does not the engine cannot help itself.
static int rebindTexPerDraw = 0;

// Flush either side of glCallList and nowhere else. This is the only placement
// the engine can actually implement, because LuaOpenGL::CallList is the one site
// it controls. A flush before every draw is not available to it.
static int flushAroundCallList = 0;

static unsigned char* reference;
static unsigned char* current;

static float ndcX(float px) { return (px / (float) SURF_W) * 2.0f - 1.0f; }
static float ndcY(float py) { return (py / (float) SURF_H) * 2.0f - 1.0f; }

static void buildGeometry(void)
{
	float cosines[DIVS];
	float sines[DIVS];

	for (int d = 0; d < DIVS; d++) {
		const float a = ((d + 1) / (float) DIVS) * 3.14159265358979f * 2.0f;
		cosines[d] = cosf(a) * RADIUS;
		sines[d]   = sinf(a) * RADIUS;
	}

	for (int d = 0; d < DIVS; d++) {
		tex4[d * 4 + 0] = (cosines[d] / RADIUS) * texRingRadius;
		tex4[d * 4 + 1] = (sines[d]   / RADIUS) * texRingRadius;
		tex4[d * 4 + 2] = 0.0f;
		tex4[d * 4 + 3] = 1.0f;
	}

	for (int b = 0; b < BATCHES; b++) {
		const float cx = MARGIN + RADIUS + (b % COLS) * SPACING;
		const float cy = MARGIN + RADIUS + (b / COLS) * SPACING;

		for (int d = 0; d < DIVS; d++) {
			const int i = b * DIVS + d;

			const float x = ndcX(cx + cosines[d]);
			const float y = ndcY(cy + sines[d]);

			vert2[i * 2 + 0] = x;
			vert2[i * 2 + 1] = y;

			vert4[i * 4 + 0] = x;
			vert4[i * 4 + 1] = y;
			vert4[i * 4 + 2] = 0.0f;
			vert4[i * 4 + 3] = 1.0f;
		}
	}
}

enum Mode {
	MODE_IMMEDIATE,        // glBegin per batch, every vertex widened to 4 floats
	MODE_IMMEDIATE_ARITY,  // glBegin per batch, arity alternating 2 and 4
	MODE_ARRAYS,           // one glDrawArrays per batch, pointer size always 4
	MODE_ARRAYS_ARITY,     // one glDrawArrays per batch, pointer size alternating 2 and 4

	// The combination the engine actually produces once Lua drawing is buffered.
	// gl.Rect goes through glRectf, which is an immediate-mode batch and never
	// went through glBeginBatch, so it never got the flush. Everything else is
	// glDrawArrays. Neither of the two cases above covers that mix, and a run
	// where one immediate-mode batch preceded the buffered draws lost the first
	// two of them.
	MODE_MIXED,            // alternating glBegin and glDrawArrays batches
	MODE_ONE_IMMEDIATE,    // a single glBegin batch first, then all glDrawArrays

	// One buffer refilled per batch rather than a separate array each. This is
	// what LuaImmediateBatch does, since it clears and reuses its vectors, and it
	// is the closest untested difference between this probe and the engine. If the
	// driver reads client array data lazily rather than at the draw, a batch gets
	// the next batch's vertices.
	MODE_ARRAYS_REUSED,

	// glRectf for the first batch, arrays for the rest. gl.Rect goes through
	// glRectf rather than glBegin, and the engine loses exactly the two batches
	// that follow one. glBegin followed by arrays is already clean, so if this is
	// not, the difference is glRectf specifically.
	MODE_RECTF_FIRST,

	// One display list a batch, all replayed, no direct drawing at all. Control
	// for the case below.
	MODE_LIST_EACH,

	// Alternating glCallList and direct glDrawArrays, which is the frame the
	// engine actually produces once gl.CreateList compiles: 47 widgets replay
	// lists while every other widget and the engine itself draws directly.
	//
	// IssueBatch fixes its own glVertexPointer size at 4 because varying it
	// corrupts the frame. It cannot fix the size a list replay uses internally,
	// because glVertexPointer is never compiled into a list. If the driver
	// replays with a different size, every direct draw beside a list gets the
	// corruption the arity mitigation exists to prevent.
	MODE_LIST_INTERLEAVED,

	// A live draw, then a glNewList opened straight after it, and the compiled
	// draw refills the very buffer the live draw was handed. This is what
	// gui_tooltip.lua does every frame a tooltip is up: RectRound draws live,
	// then gl.CreateList wraps another RectRound, and both go through the one
	// LuaImmediateBatch vector.
	//
	// Every earlier case had the list on its own. Refilling a shared buffer is
	// clean, and compiling a list is clean, but the two have never been put in
	// this order. If opening a list changes when the driver consumes the client
	// array data of a draw already issued, the live draw renders the list's
	// geometry instead of its own.
	MODE_LIST_MIDSTREAM
};

// The engine loses GL_QUADS, and every result here so far is GL_LINE_LOOP.
// Primitive assembly is a different path, so it gets its own reference.
static GLenum primType = GL_LINE_LOOP;

// Draw batch 0 with glRectf instead of the mode's usual path. A flag rather than
// a mode so the reference can draw it the same way and only the following
// batches vary, which is the whole comparison.
static int rectfFirstBatch = 0;

// Change the current colour between batches with no colour array bound, which is
// what a widget doing gl.Color then gl.BeginEnd produces. A batch that inherits
// the previous colour is the defect the amplifier's --alt mode found for
// immediate mode, and it has never been checked for arrays.
//
// A flag rather than a mode, because the comparison needs its own reference. The
// alternating colours are the expected output here, not a defect, so the
// reference has to be immediate mode drawn the same way.
static int recolourEachBatch = 0;

// Compile the whole run into a display list and replay it, rather than drawing
// directly. This is the one cell of the matrix never tested. gl.CreateList is
// deferred on this branch precisely because compiling it destroys the build
// menu, and the engine's buffered path hands glDrawArrays a pointer into a
// vector that the next block clears and refills.
//
// The GL specification says a DrawArrays compiled into a list dereferences its
// array data at compile time. LIST_CLOBBER checks that claim against the driver
// rather than trusting it: it overwrites the shared buffer after glEndList and
// before glCallList, so a driver that kept the pointer draws the clobber
// geometry instead of the batch that was compiled.
enum ListMode {
	LIST_NONE,
	LIST_COMPILE,  // compile and replay, buffer untouched in between
	LIST_CLOBBER   // compile, overwrite the shared buffer, then replay
};

static enum ListMode listMode = LIST_NONE;
static GLuint dlist = 0;

// One list a batch, for the two per-batch list modes. Compiled once, because
// recompiling 2000 lists a frame measures list compilation rather than replay.
static GLuint listBase = 0;

static void buildPerBatchLists(void)
{
	p_glEnableClientState(GL_VERTEX_ARRAY);

	if (withTexCoordArray) {
		p_glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		p_glTexCoordPointer(4, GL_FLOAT, 0, tex4);
	}

	for (int b = 0; b < BATCHES; b++) {
		// Size 4, the only size IssueBatch ever uses. Whatever the replay does
		// with that is the driver's choice, not the caller's.
		p_glVertexPointer(4, GL_FLOAT, 0, &vert4[b * DIVS * 4]);

		p_glNewList(listBase + b, GL_COMPILE);
		p_glDrawArrays(primType, 0, DIVS);
		p_glEndList();
	}

	if (withTexCoordArray)
		p_glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	p_glDisableClientState(GL_VERTEX_ARRAY);
}

// A ring at 0.9 NDC, nothing like the 7 pixel circles the batches draw. If a
// replayed list picks this up the frame gains thousands of extra pixels in a
// shape no correct run can produce, so the result cannot be read as drift.
static void clobberScratch(void)
{
	for (int d = 0; d < DIVS; d++) {
		const float a = ((d + 1) / (float) DIVS) * 3.14159265358979f * 2.0f;

		scratch[d * 4 + 0] = cosf(a) * 0.9f;
		scratch[d * 4 + 1] = sinf(a) * 0.9f;
		scratch[d * 4 + 2] = 0.0f;
		scratch[d * 4 + 3] = 1.0f;
	}
}

enum Separator {
	SEP_NONE,
	SEP_FLUSH,
	SEP_FINISH
};

static void separate(enum Separator sep)
{
	switch (sep) {
		case SEP_FLUSH:  p_glFlush();  break;
		case SEP_FINISH: p_glFinish(); break;
		default: break;
	}
}

static void render(enum Mode mode, enum Separator sep)
{
	p_glClear(GL_COLOR_BUFFER_BIT);
	p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	const int anyArrays = (mode == MODE_ARRAYS || mode == MODE_ARRAYS_ARITY ||
	                       mode == MODE_MIXED  || mode == MODE_ONE_IMMEDIATE ||
	                       mode == MODE_ARRAYS_REUSED || mode == MODE_RECTF_FIRST ||
	                       mode == MODE_LIST_INTERLEAVED || mode == MODE_LIST_MIDSTREAM);

	if (anyArrays)
		p_glEnableClientState(GL_VERTEX_ARRAY);

	// Bound once for the run rather than per batch. IssueBatch rebinds it every
	// batch, but the address never changes, so the difference is call count only
	// and this keeps the case comparable to the vertex-only ones beside it.
	if (anyArrays && withTexCoordArray) {
		p_glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		p_glTexCoordPointer(4, GL_FLOAT, 0, tex4);
	}

	// Client array state is never compiled into a list, only the draws are, so
	// the enable above sits outside on purpose. That is also what the engine
	// does: IssueBatch's enables run at compile time and are gone at replay.
	if (listMode != LIST_NONE)
		p_glNewList(dlist, GL_COMPILE);

	for (int b = 0; b < BATCHES; b++) {
		// A narrow batch is every other one, so a defect that needs two unlike
		// neighbours has them, and one that needs two like neighbours also has them.
		const int narrow = (b & 1);

		// Which path this particular batch takes.
		int usesArrays = anyArrays;

		if (mode == MODE_MIXED)
			usesArrays = (b & 1);
		else if (mode == MODE_ONE_IMMEDIATE || mode == MODE_RECTF_FIRST)
			usesArrays = (b > 0);

		separate(sep);

		// Odd batches are consumed by the even batch before them, which draws
		// itself live and then compiles this one inside a list off the same
		// buffer. Two batches an iteration, so the picture still matches a
		// reference that draws every batch once.
		if (mode == MODE_LIST_MIDSTREAM) {
			if (narrow)
				continue;

			memcpy(scratch, &vert4[b * DIVS * 4], sizeof(float) * DIVS * 4);
			p_glVertexPointer(4, GL_FLOAT, 0, scratch);
			p_glDrawArrays(primType, 0, DIVS);

			// No separator. The widget has none either, and the point is what
			// glNewList does to the draw that has just been issued.
			if (b + 1 < BATCHES) {
				p_glNewList(dlist, GL_COMPILE);

				if (midstreamOwnArray) {
					p_glVertexPointer(4, GL_FLOAT, 0, &vert4[(b + 1) * DIVS * 4]);
				} else {
					memcpy(scratch, &vert4[(b + 1) * DIVS * 4], sizeof(float) * DIVS * 4);
					p_glVertexPointer(4, GL_FLOAT, 0, scratch);
				}

				p_glDrawArrays(primType, 0, DIVS);
				p_glEndList();
				p_glCallList(dlist);
			}

			continue;
		}

		// Every batch replayed from its own list, nothing drawn directly.
		if (mode == MODE_LIST_EACH) {
			p_glCallList(listBase + b);
			continue;
		}

		// Half from lists, half direct, which is the mix a real frame has.
		if (mode == MODE_LIST_INTERLEAVED && narrow) {
			if (flushAroundCallList) p_glFlush();
			p_glCallList(listBase + b);
			if (flushAroundCallList) p_glFlush();
			continue;
		}

		// glRectf is its own immediate-mode primitive and never passes through
		// glBeginBatch, so nothing in the engine ever put a flush in front of it.
		if (rectfFirstBatch && b == 0) {
			const float x0 = ndcX(MARGIN);
			const float y0 = ndcY(MARGIN);
			const float x1 = ndcX(MARGIN + 2.0f * RADIUS);
			const float y1 = ndcY(MARGIN + 2.0f * RADIUS);

			p_glRectf(x0, y0, x1, y1);
			continue;
		}

		// Alternate the current colour with no colour array bound, so a batch that
		// inherits its neighbour's colour draws a whole circle in the wrong one
		// rather than a few stray pixels.
		if (recolourEachBatch) {
			if (narrow)
				p_glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
			else
				p_glColor4f(0.0f, 1.0f, 1.0f, 1.0f);
		}

		if (usesArrays) {
			// The engine's buffered path would set the pointer once per gl.BeginEnd
			// block and issue one draw, so the probe does the same per batch.
			if (mode == MODE_ARRAYS_ARITY && narrow) {
				p_glVertexPointer(2, GL_FLOAT, 0, &vert2[b * DIVS * 2]);
			}
			else if (mode == MODE_ARRAYS_REUSED) {
				// Refill one buffer instead of pointing at a fresh array, the way
				// LuaImmediateBatch reuses its vectors.
				memcpy(scratch, &vert4[b * DIVS * 4], sizeof(float) * DIVS * 4);
				p_glVertexPointer(4, GL_FLOAT, 0, scratch);
			}
			else {
				p_glVertexPointer(4, GL_FLOAT, 0, &vert4[b * DIVS * 4]);
			}

			if (withTexCoordArray && rebindTexPerDraw) {
				p_glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				p_glTexCoordPointer(4, GL_FLOAT, 0, tex4);
			}

			p_glDrawArrays(primType, 0, DIVS);
			continue;
		}

		p_glBegin(primType);
		for (int c = 0; c < DIVS; c++) {
			const float* v = &vert4[(b * DIVS + c) * 4];

			if (mode == MODE_IMMEDIATE_ARITY && narrow)
				p_glVertex2f(v[0], v[1]);
			else
				p_glVertex4f(v[0], v[1], v[2], v[3]);
		}
		p_glEnd();
	}

	if (listMode != LIST_NONE) {
		p_glEndList();

		if (listMode == LIST_CLOBBER)
			clobberScratch();

		p_glCallList(dlist);
	}

	if (anyArrays && withTexCoordArray)
		p_glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	if (anyArrays)
		p_glDisableClientState(GL_VERTEX_ARRAY);

	p_glFinish();
}

static void readback(unsigned char* dst)
{
	p_glReadPixels(0, 0, SURF_W, SURF_H, GL_RGBA, GL_UNSIGNED_BYTE, dst);
}

// A defect's pixel count says how much is wrong, never what it looks like. Two
// faults with the same count can be a scatter of stray dots and a screen filling
// shape, and only one of those is a candidate for the artefact being hunted.
static void dumpPPM(const char* path, const unsigned char* src)
{
	FILE* f = fopen(path, "wb");

	if (f == NULL) { fprintf(stderr, "cannot write %s\n", path); return; }

	fprintf(f, "P6\n%d %d\n255\n", SURF_W, SURF_H);

	// glReadPixels hands back bottom row first, PPM wants top row first.
	for (int y = SURF_H - 1; y >= 0; y--)
		for (int x = 0; x < SURF_W; x++)
			fwrite(&src[(y * SURF_W + x) * 4], 1, 3, f);

	fclose(f);
	printf("wrote %s\n", path);
}

// Counts pixels that differ from the reference, split by direction. Extra means
// the configuration lit a pixel the reference leaves black, which is the shape a
// merged batch makes. Missing means it failed to draw something.
//
// Reported per frame rather than as a total, because the defect is probabilistic
// at roughly one event per 2000 adjacent pairs, and the published figures are
// "stray in N of M frames, median 28 pixels". A frame count is comparable to
// those, a sum is not.
static void compare(const char* label, enum Mode mode, enum Separator sep, int iterations)
{
	int dirtyFrames = 0;
	int totalExtra = 0;
	int totalMissing = 0;
	int worstExtra = 0;
	int totalMiscoloured = 0;

	for (int it = 0; it < iterations; it++) {
		int miscoloured = 0;

		render(mode, sep);
		readback(current);

		int extra = 0;
		int missing = 0;

		for (int p = 0; p < SURF_W * SURF_H; p++) {
			const int refLit = (reference[p * 4] | reference[p * 4 + 1] | reference[p * 4 + 2]) != 0;
			const int curLit = (current[p * 4] | current[p * 4 + 1] | current[p * 4 + 2]) != 0;

			if (curLit && !refLit) extra++;
			if (refLit && !curLit) missing++;

			// Lit in both but a different colour. Scoring only lit against unlit
			// meant a batch drawn in its neighbour's colour counted as a pass,
			// which is exactly the defect this probe was later asked to find.
			if (refLit && curLit) {
				if (reference[p * 4 + 0] != current[p * 4 + 0] ||
				    reference[p * 4 + 1] != current[p * 4 + 1] ||
				    reference[p * 4 + 2] != current[p * 4 + 2])
					miscoloured++;
			}
		}

		if (extra > 0 || missing > 0 || miscoloured > 0) dirtyFrames++;
		if (extra > worstExtra) worstExtra = extra;

		totalExtra += extra;
		totalMissing += missing;
		totalMiscoloured += miscoloured;
	}

	printf("%-34s  dirty %3d/%-3d  extra %8d  missing %8d  wrong colour %8d   err 0x%04x\n",
		label, dirtyFrames, iterations, totalExtra, totalMissing, totalMiscoloured, p_glGetError());
}

int main(int argc, char** argv)
{
	// 30 frames by default, so a case is directly comparable to the published
	// "stray in 31 of 31 frames" and "0 of 30" figures.
	const int iterations = (argc > 1) ? atoi(argv[1]) : 30;

	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;

	if (!eglInitialize(dpy, &major, &minor)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
	eglBindAPI(EGL_OPENGL_API);

	const EGLint cfgAttrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};

	EGLConfig cfg;
	EGLint numCfg = 0;
	eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &numCfg);

	const EGLint surfAttrs[] = { EGL_WIDTH, SURF_W, EGL_HEIGHT, SURF_H, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surfAttrs);

	const EGLint ctxAttrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
		EGL_NONE
	};

	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "makeCurrent failed\n"); return 1; }

	LOAD(glGetString); LOAD(glGetError); LOAD(glClearColor); LOAD(glClear);
	LOAD(glViewport); LOAD(glBegin); LOAD(glEnd); LOAD(glVertex2f); LOAD(glVertex4f);
	LOAD(glColor4f); LOAD(glFinish); LOAD(glFlush); LOAD(glDisable);
	LOAD(glEnable); LOAD(glBlendFunc);
	LOAD(glEnableClientState); LOAD(glDisableClientState); LOAD(glVertexPointer);
	LOAD(glDrawArrays); LOAD(glReadPixels); LOAD(glPixelStorei); LOAD(glRectf);
	LOAD(glGenLists); LOAD(glNewList); LOAD(glEndList); LOAD(glCallList);
	LOAD(glTexCoordPointer);

	printf("GL_RENDERER = %s\n", (const char*) p_glGetString(GL_RENDERER));
	printf("GL_VERSION  = %s\n", (const char*) p_glGetString(GL_VERSION));
	printf("%d LINE_LOOP batches on a %dx%d surface, %d iterations a case\n\n",
		BATCHES, SURF_W, SURF_H, iterations);

	p_glViewport(0, 0, SURF_W, SURF_H);
	p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	p_glDisable(GL_DEPTH_TEST);
	p_glDisable(GL_CULL_FACE);
	p_glPixelStorei(GL_PACK_ALIGNMENT, 1);

	// The amplifier draws inside gl.DrawScreen, where the engine has blending on.
	// Matching it costs nothing and removes one difference from the configuration
	// that is known to reproduce.
	p_glEnable(GL_BLEND);
	p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	buildGeometry();

	reference = malloc(SURF_W * SURF_H * 4);
	current   = malloc(SURF_W * SURF_H * 4);

	// Ground truth is immediate mode with a flush between batches and uniform
	// vertex arity, which is what the engine ships and what measured 0 of 30
	// frames stray.
	render(MODE_IMMEDIATE, SEP_FLUSH);
	readback(reference);

	// batch_merge_probe <iterations> --dump <dir> writes each interesting case as
	// a PPM and stops. Scoring tells you a case is dirty, this tells you whether
	// it is dirty in the shape you are hunting.
	if (argc > 3 && strcmp(argv[2], "--dump") == 0) {
		char path[512];

		snprintf(path, sizeof(path), "%s/ref-immediate-flush.ppm", argv[3]);
		dumpPPM(path, reference);

		render(MODE_ARRAYS_ARITY, SEP_NONE);
		readback(current);
		snprintf(path, sizeof(path), "%s/arrays-varying-pointer-size.ppm", argv[3]);
		dumpPPM(path, current);

		render(MODE_IMMEDIATE, SEP_NONE);
		readback(current);
		snprintf(path, sizeof(path), "%s/immediate-no-separator.ppm", argv[3]);
		dumpPPM(path, current);

		primType = GL_QUADS;
		render(MODE_ARRAYS_ARITY, SEP_NONE);
		readback(current);
		snprintf(path, sizeof(path), "%s/quads-varying-pointer-size.ppm", argv[3]);
		dumpPPM(path, current);
		primType = GL_LINE_LOOP;

		// The case that reproduces the wedge. Needs a list opened straight after
		// a live draw, a texcoord array, and the shared buffer, all together.
		dlist = p_glGenLists(1);
		withTexCoordArray = 1;
		render(MODE_LIST_MIDSTREAM, SEP_NONE);
		readback(current);
		snprintf(path, sizeof(path), "%s/midstream-list-textured.ppm", argv[3]);
		dumpPPM(path, current);

		primType = GL_QUADS;
		render(MODE_LIST_MIDSTREAM, SEP_NONE);
		readback(current);
		snprintf(path, sizeof(path), "%s/midstream-list-textured-quads.ppm", argv[3]);
		dumpPPM(path, current);

		// Shrink the texcoord ring and redraw. If the wrecked geometry shrinks
		// with it, the positions are coming from the texture coordinate array.
		texRingRadius = 0.25f;
		buildGeometry();
		render(MODE_LIST_MIDSTREAM, SEP_NONE);
		readback(current);
		snprintf(path, sizeof(path), "%s/midstream-texring-0.25.ppm", argv[3]);
		dumpPPM(path, current);
		texRingRadius = 0.9f;
		buildGeometry();

		primType = GL_LINE_LOOP;
		withTexCoordArray = 0;

		return 0;
	}

	printf("controls\n");
	compare("immediate, flush   (reference)", MODE_IMMEDIATE, SEP_FLUSH, iterations);
	compare("arrays,    finish  (2nd truth)", MODE_ARRAYS,    SEP_FINISH, iterations);

	printf("\npositive control, must show extra pixels\n");
	compare("immediate, no separator",        MODE_IMMEDIATE, SEP_NONE, iterations);

	printf("\nthe questions\n");
	compare("arrays,    no separator",        MODE_ARRAYS,       SEP_NONE, iterations);
	compare("arrays,    flush",               MODE_ARRAYS,       SEP_FLUSH, iterations);
	compare("arrays,    varying pointer size", MODE_ARRAYS_ARITY, SEP_NONE, iterations);

	// Never asked before. If a flush cleans this the way it cleans batch merging,
	// then varying pointer size is the same separator problem wearing a different
	// hat, and no display list can ever be safe from it: glFlush executes during
	// compilation and is never recorded, so a list has no separator to replay.
	compare("arrays,    varying size, flush",  MODE_ARRAYS_ARITY, SEP_FLUSH, iterations);
	compare("arrays,    varying size, finish", MODE_ARRAYS_ARITY, SEP_FINISH, iterations);
	compare("immediate, varying arity, flush", MODE_IMMEDIATE_ARITY, SEP_FLUSH, iterations);

	printf("\nmixing the two, which is what the engine now emits\n");
	compare("mixed immediate and arrays",      MODE_MIXED,         SEP_NONE, iterations);
	compare("mixed, flush",                    MODE_MIXED,         SEP_FLUSH, iterations);
	compare("one immediate then all arrays",   MODE_ONE_IMMEDIATE, SEP_NONE, iterations);
	compare("one immediate then all, flush",   MODE_ONE_IMMEDIATE, SEP_FLUSH, iterations);

	printf("\none buffer refilled per batch, as LuaImmediateBatch does\n");
	compare("arrays, reused buffer",           MODE_ARRAYS_REUSED, SEP_NONE, iterations);
	compare("arrays, reused buffer, flush",    MODE_ARRAYS_REUSED, SEP_FLUSH, iterations);

	// Compiled into a display list and replayed, which is what gl.CreateList does
	// once the deferral comes off. Same reference: a list that replays correctly
	// is indistinguishable from drawing directly.
	printf("\ncompiled into a display list, then replayed\n");
	dlist = p_glGenLists(1);

	listMode = LIST_COMPILE;
	compare("list, immediate",                 MODE_IMMEDIATE,     SEP_NONE, iterations);
	compare("list, arrays, own array a batch", MODE_ARRAYS,        SEP_NONE, iterations);
	compare("list, arrays, reused buffer",     MODE_ARRAYS_REUSED, SEP_NONE, iterations);

	// A list whose draws do not all use the same vertex pointer size. This is the
	// combination a real widget produces: IssueBatch draws position at size 4, the
	// font renderer draws it at size 3, and gl.Text inside a gl.CreateList puts
	// both in one list. Outside a list the engine can separate them. Inside one it
	// cannot, because no separator is ever recorded.
	compare("list, arrays, varying size",      MODE_ARRAYS_ARITY,  SEP_NONE, iterations);
	compare("list, arrays, varying size, flush", MODE_ARRAYS_ARITY, SEP_FLUSH, iterations);

	// The same three with the shared buffer overwritten between glEndList and
	// glCallList. Only the reused case can notice, and only if the driver kept
	// the pointer instead of dereferencing at compile time.
	printf("\nsame, with the shared buffer overwritten before replay\n");
	listMode = LIST_CLOBBER;
	compare("list, arrays, own array a batch", MODE_ARRAYS,        SEP_NONE, iterations);
	compare("list, arrays, reused buffer",     MODE_ARRAYS_REUSED, SEP_NONE, iterations);
	listMode = LIST_NONE;

	// One list a batch rather than one list for the run, and then the mix that
	// matters: list replays and direct draws alternating in the same frame.
	printf("\none list a batch, replayed beside direct draws\n");
	listBase = p_glGenLists(BATCHES);
	buildPerBatchLists();

	compare("every batch from its own list",   MODE_LIST_EACH,        SEP_NONE, iterations);
	compare("lists and direct draws alternating", MODE_LIST_INTERLEAVED, SEP_NONE, iterations);
	compare("lists and direct, flush",         MODE_LIST_INTERLEAVED, SEP_FLUSH, iterations);

	// Are many small textured lists enough on their own, without any compiling
	// happening mid-stream? These lists are compiled once, up front, and only
	// replayed in the loop. If they are clean, the fault is in compiling a list
	// beside live drawing, not in replaying one.
	printf("\nthe same per-batch lists, textured\n");
	withTexCoordArray = 1;
	buildPerBatchLists();

	compare("textured, every batch from a list", MODE_LIST_EACH,        SEP_NONE, iterations);
	compare("textured, lists and direct mixed",  MODE_LIST_INTERLEAVED, SEP_NONE, iterations);

	// Can the caller defend itself? These are the three things engine code can
	// do around a list replay without changing what any widget draws.
	// The candidate engine fix, on its own, with no separator anywhere else.
	flushAroundCallList = 1;
	compare("same, flush around glCallList",     MODE_LIST_INTERLEAVED, SEP_NONE,  iterations);
	flushAroundCallList = 0;

	compare("same, flush only",                  MODE_LIST_INTERLEAVED, SEP_FLUSH, iterations);
	compare("same, finish only",                 MODE_LIST_INTERLEAVED, SEP_FINISH, iterations);

	rebindTexPerDraw = 1;
	compare("same, texcoord pointer rebound",    MODE_LIST_INTERLEAVED, SEP_NONE,  iterations);
	compare("same, rebound and flushed",         MODE_LIST_INTERLEAVED, SEP_FLUSH, iterations);
	compare("same, rebound and finished",        MODE_LIST_INTERLEAVED, SEP_FINISH, iterations);
	rebindTexPerDraw = 0;

	withTexCoordArray = 0;
	buildPerBatchLists();

	// The order gui_tooltip.lua actually produces: draw live, then open a list
	// and refill the buffer that live draw was handed.
	printf("\na list opened straight after a live draw, sharing its buffer\n");
	compare("live draw, then compile off it",  MODE_LIST_MIDSTREAM, SEP_NONE,  iterations);
	compare("same, flush between",             MODE_LIST_MIDSTREAM, SEP_FLUSH, iterations);

	withTexCoordArray = 1;
	compare("same, with texcoords",            MODE_LIST_MIDSTREAM, SEP_NONE,  iterations);

	// Does a separator fix it, the way it fixes batch merging and varying
	// pointer size? If so the engine has a one line mitigation available.
	compare("same, texcoords, flush",          MODE_LIST_MIDSTREAM, SEP_FLUSH, iterations);
	compare("same, texcoords, finish",         MODE_LIST_MIDSTREAM, SEP_FINISH, iterations);

	// Is the shared buffer required, or is opening a list after a textured draw
	// enough on its own? Separates "give each batch its own storage" from
	// "separate the draw from the glNewList" as the fix.
	midstreamOwnArray = 1;
	compare("same, list has its own array",    MODE_LIST_MIDSTREAM, SEP_NONE,  iterations);
	midstreamOwnArray = 0;

	withTexCoordArray = 0;

	printf("\nsame as GL_QUADS, which is what RectRound draws\n");
	primType = GL_QUADS;
	render(MODE_IMMEDIATE, SEP_FLUSH);
	readback(reference);

	compare("quads, live draw then compile",   MODE_LIST_MIDSTREAM, SEP_NONE,  iterations);
	withTexCoordArray = 1;
	compare("quads, textured, same",           MODE_LIST_MIDSTREAM, SEP_NONE,  iterations);
	withTexCoordArray = 0;

	primType = GL_LINE_LOOP;
	render(MODE_IMMEDIATE, SEP_FLUSH);
	readback(reference);

	// A texture coordinate array bound alongside the vertex array. Nothing is
	// textured here, so a correct driver draws the same picture as the cases
	// above and the reference still applies. A driver that muddles which array
	// feeds which attribute when baking a list draws the texcoord ring instead.
	//
	// This is the probe's oldest blind spot. Every case before it bound a vertex
	// array and nothing else, while the drawing being hunted is a textured
	// GL_QUADS batch from the game's RectRound.
	printf("\na texcoord array bound too, as a textured batch has\n");
	withTexCoordArray = 1;

	compare("arrays, texcoords",               MODE_ARRAYS,        SEP_NONE, iterations);
	compare("arrays, texcoords, reused buffer", MODE_ARRAYS_REUSED, SEP_NONE, iterations);

	listMode = LIST_COMPILE;
	compare("list, texcoords",                 MODE_ARRAYS,        SEP_NONE, iterations);
	compare("list, texcoords, reused buffer",  MODE_ARRAYS_REUSED, SEP_NONE, iterations);
	compare("list, texcoords, varying size",   MODE_ARRAYS_ARITY,  SEP_NONE, iterations);
	listMode = LIST_NONE;

	printf("\nsame, as GL_QUADS, which is what RectRound draws\n");
	primType = GL_QUADS;
	withTexCoordArray = 0;
	render(MODE_IMMEDIATE, SEP_FLUSH);
	readback(reference);
	withTexCoordArray = 1;

	compare("quads, arrays, texcoords",        MODE_ARRAYS,        SEP_NONE, iterations);
	listMode = LIST_COMPILE;
	compare("quads, list, texcoords",          MODE_ARRAYS,        SEP_NONE, iterations);
	compare("quads, list, texcoords, reused",  MODE_ARRAYS_REUSED, SEP_NONE, iterations);
	listMode = LIST_NONE;

	primType = GL_LINE_LOOP;
	withTexCoordArray = 0;

	// Its own reference, because the alternating colours are the wanted output.
	printf("\ncurrent colour changed between batches, no colour array\n");
	recolourEachBatch = 1;
	render(MODE_IMMEDIATE, SEP_FLUSH);
	readback(reference);

	compare("immediate, flush   (reference)",  MODE_IMMEDIATE, SEP_FLUSH, iterations);
	compare("immediate, no separator",         MODE_IMMEDIATE, SEP_NONE, iterations);
	compare("arrays,    no separator",         MODE_ARRAYS,    SEP_NONE, iterations);
	compare("arrays,    reused buffer",        MODE_ARRAYS_REUSED, SEP_NONE, iterations);
	recolourEachBatch = 0;

	// GL_QUADS, which is what the engine loses. Its own reference, because the
	// shapes differ from the line loops above.
	printf("\nGL_QUADS instead of GL_LINE_LOOP\n");
	primType = GL_QUADS;
	render(MODE_IMMEDIATE, SEP_FLUSH);
	readback(reference);

	compare("quads, immediate, flush  (ref)",   MODE_IMMEDIATE, SEP_FLUSH, iterations);
	compare("quads, immediate, no separator",   MODE_IMMEDIATE, SEP_NONE, iterations);
	compare("quads, arrays, no separator",      MODE_ARRAYS,    SEP_NONE, iterations);

	// glRectf first, then arrays, in quads. This is the engine's exact sequence.
	printf("\nglRectf then arrays, the engine's own sequence\n");
	rectfFirstBatch = 1;
	render(MODE_IMMEDIATE, SEP_FLUSH);
	readback(reference);

	compare("rectf then immediate, flush (ref)", MODE_IMMEDIATE, SEP_FLUSH, iterations);
	compare("rectf then arrays, no separator",   MODE_ARRAYS,    SEP_NONE, iterations);
	compare("rectf then arrays, flush",          MODE_ARRAYS,    SEP_FLUSH, iterations);

	// Does a plain state change between them do the job a flush does? Engine code
	// always sets some state before LuaUI draws, so if this is clean the 17
	// glRectf sites in the engine need nothing.
	recolourEachBatch = 1;
	render(MODE_IMMEDIATE, SEP_FLUSH);
	readback(reference);

	compare("rectf, colour, immediate (ref)",    MODE_IMMEDIATE, SEP_FLUSH, iterations);
	compare("rectf, colour, then arrays",        MODE_ARRAYS,    SEP_NONE, iterations);
	recolourEachBatch = 0;

	rectfFirstBatch = 0;
	primType = GL_LINE_LOOP;

	return 0;
}
