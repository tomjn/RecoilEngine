// Narrows fmt_probe's G1 result. G1 (a run of no-texcoord batches followed by a
// run of texcoord batches) fails. G2 (alternating every batch) and G5 (every
// batch carrying a texcoord) pass. The working theory from reading Mesa's
// vbo_exec_draw.c is that the corruption depends on the byte offset the second
// format's batch lands at inside the persistently mapped immediate-mode buffer,
// which is exec->vtx.buffer_offset and grows by whatever earlier batches wrote.
//
// So: draw N zero-area warm-up quads in the first format, then the visible grid
// in the second, and sweep N. If the theory holds the failures are periodic in
// N. Zero-area quads light no pixels but still consume buffer space.
//
// Build against the instrumented Mesa and set VBO_TRACE=1 to see the offset,
// stride and misalignment Mesa hands the driver for every immediate-mode draw.

#include <EGL/egl.h>
#include <EGL/eglext.h>
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

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_QUADS            0x0007
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401
#define GL_DEPTH_TEST       0x0B71
#define GL_BLEND            0x0BE2
#define GL_CULL_FACE        0x0B44
#define GL_MODELVIEW        0x1700
#define GL_PROJECTION       0x1701
#define GL_VERSION          0x1F02
#define GL_RENDERER         0x1F01

static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
static const GLubyte* (*p_glGetString)(GLenum);
static GLenum (*p_glGetError)(void);
static void (*p_glBegin)(GLenum);
static void (*p_glEnd)(void);
static void (*p_glVertex2f)(GLfloat, GLfloat);
static void (*p_glColor3f)(GLfloat, GLfloat, GLfloat);
static void (*p_glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glTexCoord2f)(GLfloat, GLfloat);
static void (*p_glNormal3f)(GLfloat, GLfloat, GLfloat);
static void (*p_glMatrixMode)(GLenum);
static void (*p_glLoadIdentity)(void);
static void (*p_glDisable)(GLenum);
static void (*p_glFlush)(void);
static void (*p_glFinish)(void);
static void (*p_glVertex3f)(GLfloat, GLfloat, GLfloat);
static void (*p_glVertex4f)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glGetFloatv)(GLenum, GLfloat*);
static void (*p_glPushMatrix)(void);
static void (*p_glPopMatrix)(void);
static void (*p_glEnable)(GLenum);
static GLuint (*p_glGenLists)(GLsizei);
static void (*p_glNewList)(GLuint, GLenum);
static void (*p_glEndList)(void);
static void (*p_glCallList)(GLuint);

#define GL_COMPILE 0x1300

static void (*p_glRectf)(GLfloat, GLfloat, GLfloat, GLfloat);

#define GL_CURRENT_COLOR          0x0B00
#define GL_CURRENT_NORMAL         0x0B02
#define GL_CURRENT_TEXTURE_COORDS 0x0B03

#define LOAD(name) \
	p_##name = (void*)eglGetProcAddress(#name); \
	if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 1; }

#define SIZE 256
#define CELLS 6
#define MARGIN 6

static const int cell = SIZE / CELLS;

static void CellRect(int cx, int cy, float* nx0, float* ny0, float* nx1, float* ny1)
{
	const float x0 = (float)(cx * cell + MARGIN);
	const float y0 = (float)(cy * cell + MARGIN);
	const float x1 = (float)((cx + 1) * cell - MARGIN);
	const float y1 = (float)((cy + 1) * cell - MARGIN);

	*nx0 = (x0 / SIZE) * 2.0f - 1.0f;
	*ny0 = (y0 / SIZE) * 2.0f - 1.0f;
	*nx1 = (x1 / SIZE) * 2.0f - 1.0f;
	*ny1 = (y1 / SIZE) * 2.0f - 1.0f;
}

static int Verify(const char* label, int quiet)
{
	static GLubyte pixels[SIZE * SIZE * 4];
	p_glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

	int strayLit = 0;
	int missingFill = 0;
	int litTotal = 0;

	for (int y = 0; y < SIZE; ++y) {
		for (int x = 0; x < SIZE; ++x) {
			const int lit = pixels[(y * SIZE + x) * 4] > 32;
			const int inCellX = (x % cell) >= MARGIN && (x % cell) < (cell - MARGIN);
			const int inCellY = (y % cell) >= MARGIN && (y % cell) < (cell - MARGIN);
			const int expected = inCellX && inCellY;

			litTotal += lit;
			if (lit && !expected) strayLit++;
			if (!lit && expected) missingFill++;
		}
	}

	const int bad = (strayLit != 0 || missingFill != 0);

	if (!quiet) {
		printf("%-38s lit=%-6d stray=%-6d unfilled=%-6d err=0x%04x  %s\n",
			label, litTotal, strayLit, missingFill, p_glGetError(), bad ? "WRONG" : "OK");
	}

	return bad;
}

static void ResetFrame(void)
{
	p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	p_glClear(GL_COLOR_BUFFER_BIT);
}

// one visible quad. txcd, color and norm say which optional attributes each
// vertex carries. lead says whether the batch emits them all once before its
// first vertex, which is what normalising the format looks like.
static void Quad(float x0, float y0, float x1, float y1,
                 int txcd, int color, int norm, int lead)
{
	const float xs[4] = { x0, x1, x1, x0 };
	const float ys[4] = { y0, y0, y1, y1 };

	p_glColor3f(1.0f, 1.0f, 1.0f);
	p_glBegin(GL_QUADS);

	if (lead) {
		p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		p_glTexCoord2f(0.0f, 0.0f);
		p_glNormal3f(0.0f, 0.0f, 1.0f);
	}

	for (int v = 0; v < 4; ++v) {
		if (color) p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		if (txcd)  p_glTexCoord2f(0.0f, 0.0f);
		if (norm)  p_glNormal3f(0.0f, 0.0f, 1.0f);
		p_glVertex2f(xs[v], ys[v]);
	}

	p_glEnd();
}

// a batch that writes vertices but lights no pixels
// E24 widens only the grid, so arity still varies between batches. The engine
// widens every Lua vertex, so the analogue has to widen the warm-up as well.
static int g_wideVerts = 0;

static void DegenerateQuad(int txcd, int color, int norm, int lead, int verts)
{
	p_glColor3f(1.0f, 1.0f, 1.0f);
	p_glBegin(GL_QUADS);

	if (lead) {
		p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		p_glTexCoord2f(0.0f, 0.0f);
		p_glNormal3f(0.0f, 0.0f, 1.0f);
	}

	for (int v = 0; v < verts; ++v) {
		if (color) p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		if (txcd)  p_glTexCoord2f(0.0f, 0.0f);
		if (norm)  p_glNormal3f(0.0f, 0.0f, 1.0f);
		if (g_wideVerts) p_glVertex4f(-1.0f, -1.0f, 0.0f, 1.0f);
		else             p_glVertex2f(-1.0f, -1.0f);
	}

	p_glEnd();
}

// the grid, one batch per cell, all in the same format
static void Grid(int txcd, int color, int norm, int lead)
{
	for (int cy = 0; cy < CELLS; ++cy) {
		for (int cx = 0; cx < CELLS; ++cx) {
			float nx0, ny0, nx1, ny1;
			CellRect(cx, cy, &nx0, &ny0, &nx1, &ny1);
			Quad(nx0, ny0, nx1, ny1, txcd, color, norm, lead);
		}
	}
}

int main(int argc, char** argv)
{
	const char* only = (argc > 1) ? argv[1] : "all";

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

	const EGLint surfAttrs[] = { EGL_WIDTH, SIZE, EGL_HEIGHT, SIZE, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surfAttrs);

	const EGLint ctxAttrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
		EGL_NONE
	};

	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "makeCurrent failed\n"); return 1; }

	LOAD(glClearColor); LOAD(glClear); LOAD(glViewport); LOAD(glReadPixels);
	LOAD(glGetString); LOAD(glGetError); LOAD(glBegin); LOAD(glEnd);
	LOAD(glVertex2f); LOAD(glColor3f); LOAD(glColor4f);
	LOAD(glTexCoord2f); LOAD(glNormal3f);
	LOAD(glMatrixMode); LOAD(glLoadIdentity); LOAD(glDisable);
	LOAD(glFlush); LOAD(glFinish); LOAD(glVertex3f); LOAD(glVertex4f); LOAD(glGetFloatv);
	LOAD(glPushMatrix); LOAD(glPopMatrix); LOAD(glEnable);
	LOAD(glGenLists); LOAD(glNewList); LOAD(glEndList); LOAD(glCallList);
	LOAD(glRectf);

	printf("GL_RENDERER = %s\n", (const char*)p_glGetString(GL_RENDERER));
	printf("GL_VERSION  = %s\n\n", (const char*)p_glGetString(GL_VERSION));

	p_glViewport(0, 0, SIZE, SIZE);
	p_glDisable(GL_DEPTH_TEST);
	p_glDisable(GL_BLEND);
	p_glDisable(GL_CULL_FACE);
	p_glMatrixMode(GL_PROJECTION); p_glLoadIdentity();
	p_glMatrixMode(GL_MODELVIEW);  p_glLoadIdentity();

	// A: the three known fmt_probe results, restated so a trace can be read
	// against them without the other twelve tests in the way
	if (!strcmp(only, "all") || !strcmp(only, "known")) {
		fprintf(stderr, "\n=== A1 run of no-texcoord then run of texcoord (fmt_probe G1) ===\n");
		ResetFrame();
		for (int cy = 0; cy < CELLS; ++cy)
			for (int cx = 0; cx < CELLS; cx += 2) {
				float a, b, c, d; CellRect(cx, cy, &a, &b, &c, &d);
				Quad(a, b, c, d, 0, 0, 0, 0);
			}
		for (int cy = 0; cy < CELLS; ++cy)
			for (int cx = 1; cx < CELLS; cx += 2) {
				float a, b, c, d; CellRect(cx, cy, &a, &b, &c, &d);
				Quad(a, b, c, d, 1, 0, 0, 0);
			}
		Verify("A1 no-texcoord run then texcoord run", 0);

		fprintf(stderr, "\n=== A2 format alternating every batch (fmt_probe G2) ===\n");
		ResetFrame();
		for (int cy = 0; cy < CELLS; ++cy)
			for (int cx = 0; cx < CELLS; ++cx) {
				float a, b, c, d; CellRect(cx, cy, &a, &b, &c, &d);
				Quad(a, b, c, d, (cx + cy) & 1, 0, 0, 0);
			}
		Verify("A2 alternating every batch", 0);

		fprintf(stderr, "\n=== A3 every batch leads with all attributes (fmt_probe G6) ===\n");
		ResetFrame();
		for (int pass = 0; pass < 2; ++pass)
			for (int cy = 0; cy < CELLS; ++cy)
				for (int cx = pass; cx < CELLS; cx += 2) {
					float a, b, c, d; CellRect(cx, cy, &a, &b, &c, &d);
					Quad(a, b, c, d, pass, pass, 0, 1);
				}
		Verify("A3 all batches normalised", 0);
	}

	// B: the sweep. N zero-area no-texcoord quads, then the grid with texcoords.
	// Only N varies, so a periodic failure pattern means the byte offset the
	// grid's first batch lands at is what decides.
	if (!strcmp(only, "all") || !strcmp(only, "sweep")) {
		printf("\n-- sweep: N warm-up quads (no texcoord), then the grid with texcoords --\n");
		int bad = 0;
		for (int n = 0; n <= 32; ++n) {
			ResetFrame();
			for (int i = 0; i < n; ++i)
				DegenerateQuad(0, 0, 0, 0, 4);
			Grid(1, 0, 0, 0);

			char label[64];
			snprintf(label, sizeof(label), "  N=%d", n);
			bad += Verify(label, 0);
		}
		printf("  sweep: %d of 33 wrong\n", bad);
	}

	// B2: one chosen N, so a VBO_TRACE run shows only the draws that matter
	if (!strcmp(only, "one")) {
		const int n = (argc > 2) ? atoi(argv[2]) : 2;
		fprintf(stderr, "\n=== warm-up N=%d then the grid with texcoords ===\n", n);
		ResetFrame();
		for (int i = 0; i < n; ++i)
			DegenerateQuad(0, 0, 0, 0, 4);
		fprintf(stderr, "--- warm-up done, grid starts ---\n");
		Grid(1, 0, 0, 0);
		fprintf(stderr, "--- grid done ---\n");

		char label[64];
		snprintf(label, sizeof(label), "N=%d", n);
		Verify(label, 0);
	}

	// C: the same sweep with the warm-up batch varying in vertex count rather
	// than batch count, which moves the offset in finer steps
	if (!strcmp(only, "all") || !strcmp(only, "verts")) {
		printf("\n-- sweep: one warm-up batch of V vertices, then the grid with texcoords --\n");
		int bad = 0;
		for (int v = 0; v <= 32; v += 4) {
			ResetFrame();
			if (v > 0)
				DegenerateQuad(0, 0, 0, 0, v);
			Grid(1, 0, 0, 0);

			const int r = Verify("", 1);
			bad += r;
			printf("  V=%-3d %s\n", v, r ? "WRONG" : "OK");
		}
		printf("  vert sweep: %d wrong\n", bad);
	}

	// D: candidate mitigations run against the failing A1 pattern
	if (!strcmp(only, "all") || !strcmp(only, "mit")) {
		printf("\n-- mitigations against the A1 pattern --\n");

		// M1 glFlush after every batch
		ResetFrame();
		for (int pass = 0; pass < 2; ++pass)
			for (int cy = 0; cy < CELLS; ++cy)
				for (int cx = pass; cx < CELLS; cx += 2) {
					float a, b, c, d; CellRect(cx, cy, &a, &b, &c, &d);
					Quad(a, b, c, d, pass, 0, 0, 0);
					p_glFlush();
				}
		Verify("M1 glFlush after every batch", 0);

		// M2 glFinish after every batch
		ResetFrame();
		for (int pass = 0; pass < 2; ++pass)
			for (int cy = 0; cy < CELLS; ++cy)
				for (int cx = pass; cx < CELLS; cx += 2) {
					float a, b, c, d; CellRect(cx, cy, &a, &b, &c, &d);
					Quad(a, b, c, d, pass, 0, 0, 0);
					p_glFinish();
				}
		Verify("M2 glFinish after every batch", 0);

		// M3 normalise the format, then sweep the warm-up count to check the
		// result is the rule and not the luck of one offset
		int bad = 0;
		for (int n = 0; n <= 32; ++n) {
			ResetFrame();
			for (int i = 0; i < n; ++i)
				DegenerateQuad(0, 0, 0, 1, 4);
			Grid(1, 1, 0, 1);
			bad += Verify("", 1);
		}
		printf("M3 normalised format across 33 warm-up counts   %d wrong\n", bad);
	}

	// E: the shape LuaOpenGL actually produces, and the candidate fixes, each
	// run across a range of preceding batch counts so a pass means a rule and
	// not the luck of one buffer state
	if (!strcmp(only, "all") || !strcmp(only, "engine")) {
		printf("\n-- LuaOpenGL-shaped batches, each fix swept over 0..16 warm-up batches --\n");

		static const char* names[] = {
			"E0 naive, as the engine draws today   ",
			"E1 re-emit current values BEFORE begin",
			"E2 re-emit current values AFTER begin ",
			"E3 fixed values BEFORE begin          ",
			"E4 E3 plus a pinned 3f vertex arity   ",
			"E5 naive plus glFlush after every end ",
			"E6 naive plus glFlush before every begin",
			"E7 naive plus push/pop matrix before begin",
			"E8 naive but the vertex arity never varies",
			"E9 naive but a colour on every vertex     ",
			"E10 naive but a texcoord on every vertex  ",
			"E11 naive plus a blend toggle before begin",
			"E12 no churn at all, one fixed vertex format",
			"E13 E12 plus the full attribute set after begin",
			"E14 fixed arity plus the set after begin",
			"E15 colour and texcoord on every vertex, arity churns",
			"E16 E13 but the warm-up batches are normalised too",
			"E17 only the warm-up is normalised, grid draws naively",
			"E18 E16 plus a glColor3f outside every batch",
			"E19 naive plus a glColor3f outside every batch",
			"E20 M3's exact calls, run inside this harness",
			"E21 attribute set after every begin, churn left alone",
			"E22 E21 plus a fixed vertex arity, colour and tex churn",
			"E23 E21 plus colour and tex on every vertex, arity churns",
			"E24 naive, but every vertex is a wide glVertex4f",
			"E25 E24 but the warm-up batches are wide too",
		};

		const int fixLo = (argc > 2) ? atoi(argv[2]) : 0;
		const int fixHi = (argc > 2) ? atoi(argv[2]) : 25;
		const int nLo = (argc > 3) ? atoi(argv[3]) : 0;
		const int nHi = (argc > 3) ? atoi(argv[3]) : 16;

		for (int fix = fixLo; fix <= fixHi; ++fix) {
			int bad = 0;
			int firstBad = -1;

			for (int n = nLo; n <= nHi; ++n) {
				ResetFrame();

				// a run of plain batches, the way the load screen draws before
				// it reaches the panel
				const int leadWarmup = (fix == 16 || fix == 17 || fix == 20 || fix == 21 || fix == 22 || fix == 23);
				g_wideVerts = (fix == 25);
				for (int i = 0; i < n; ++i)
					DegenerateQuad(0, 0, 0, leadWarmup, 4);

				if (fix == 20)
					Grid(1, 1, 0, 1);

				for (int cy = 0; cy < CELLS && fix != 20; ++cy) {
					for (int cx = 0; cx < CELLS; ++cx) {
						float x0, y0, x1, y1;
						CellRect(cx, cy, &x0, &y0, &x1, &y1);

						const float xs[4] = { x0, x1, x1, x0 };
						const float ys[4] = { y0, y0, y1, y1 };

						GLfloat col[4], txc[4], nrm[3];

						if (fix == 1 || fix == 2) {
							p_glGetFloatv(GL_CURRENT_COLOR, col);
							p_glGetFloatv(GL_CURRENT_TEXTURE_COORDS, txc);
							p_glGetFloatv(GL_CURRENT_NORMAL, nrm);
						}

						if (fix == 1) {
							p_glColor4f(col[0], col[1], col[2], col[3]);
							p_glTexCoord2f(txc[0], txc[1]);
							p_glNormal3f(nrm[0], nrm[1], nrm[2]);
						}
						if (fix == 3 || fix == 4) {
							p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
							p_glTexCoord2f(0.0f, 0.0f);
							p_glNormal3f(0.0f, 0.0f, 1.0f);
						}

						if (fix == 6)
							p_glFlush();

						if (fix == 18 || fix == 19)
							p_glColor3f(1.0f, 1.0f, 1.0f);

						// a real state change, so Mesa issues what is
						// accumulated without submitting the command stream
						if (fix == 11) {
							p_glEnable(GL_BLEND);
							p_glDisable(GL_BLEND);
						}

						// a matrix push/pop is a state change, so it makes Mesa
						// issue whatever is accumulated without also flushing
						// the command stream the way glFlush does
						if (fix == 7) {
							p_glPushMatrix();
							p_glPopMatrix();
						}

						p_glBegin(GL_QUADS);

						if (fix == 2) {
							p_glColor4f(col[0], col[1], col[2], col[3]);
							p_glTexCoord2f(txc[0], txc[1]);
							p_glNormal3f(nrm[0], nrm[1], nrm[2]);
						}

						// M3 fixes this completely by giving every batch the same
						// vertex format. It does that with a fixed attribute set
						// right after begin and a constant arity. E2 has the set
						// but keeps the churn, so the pair has never been tried.
						if (fix == 13 || fix == 14 || fix == 16 || fix == 18 || fix == 21 || fix == 22 || fix == 23) {
							p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
							p_glTexCoord2f(0.0f, 0.0f);
							p_glNormal3f(0.0f, 0.0f, 1.0f);
						}

						// the churn a widget produces: colour on some vertices,
						// a texcoord on one, and a mixed vertex arity
						const int arityChurn = (fix != 8 && fix != 12 && fix != 13 && fix != 14 && fix != 16 && fix != 18 && fix != 22);
						const int colorChurn = (fix != 9 && fix != 12 && fix != 13 && fix != 15 && fix != 16 && fix != 18 && fix != 23);
						const int texChurn = (fix != 10 && fix != 12 && fix != 13 && fix != 15 && fix != 16 && fix != 18 && fix != 23);

						for (int v = 0; v < 4; ++v) {
							if (!colorChurn || (v & 1)) p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
							if (!texChurn || v == 2) p_glTexCoord2f(0.0f, 0.0f);

							if (fix == 24 || fix == 25)
								p_glVertex4f(xs[v], ys[v], 0.0f, 1.0f);
							else if (fix == 4)
								p_glVertex3f(xs[v], ys[v], 0.0f);
							else if (arityChurn && v == 3)
								p_glVertex3f(xs[v], ys[v], 0.0f);
							else
								p_glVertex2f(xs[v], ys[v]);
						}

						p_glEnd();

						if (fix == 5)
							p_glFlush();
					}
				}

				char vlabel[64];
				snprintf(vlabel, sizeof(vlabel), "    fix=%d N=%d", fix, n);

				if (Verify(vlabel, (argc > 2) ? 0 : 1)) {
					bad++;
					if (firstBad < 0) firstBad = n;
				}
			}

			printf("  %s  %2d of 17 wrong", names[fix], bad);
			if (firstBad >= 0)
				printf(", first at N=%d", firstBad);
			printf("\n");
		}
	}

	// D: the resource bar's shape. The same churny batch, but compiled into a
	// display list and replayed, which is what gl.CreateList plus gl.CallList
	// produce. That replays through Mesa's vbo_save rather than vbo_exec, so
	// flushing at glBegin time cannot reach it.
	if (!strcmp(only, "all") || !strcmp(only, "list")) {
		printf("\n-- the same batches compiled into a display list --\n");

		const GLuint list = p_glGenLists(1);

		p_glNewList(list, GL_COMPILE);
		for (int cy = 0; cy < CELLS; ++cy) {
			for (int cx = 0; cx < CELLS; ++cx) {
				float x0, y0, x1, y1;
				CellRect(cx, cy, &x0, &y0, &x1, &y1);

				const float xs[4] = { x0, x1, x1, x0 };
				const float ys[4] = { y0, y0, y1, y1 };

				p_glColor3f(1.0f, 1.0f, 1.0f);
				p_glBegin(GL_QUADS);
				for (int v = 0; v < 4; ++v) {
					if (v & 1) p_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
					if (v == 2) p_glTexCoord2f(0.0f, 0.0f);
					if (v == 3) p_glVertex3f(xs[v], ys[v], 0.0f);
					else        p_glVertex2f(xs[v], ys[v]);
				}
				p_glEnd();
			}
		}
		p_glEndList();

		static const char* dnames[] = {
			"D0 call the list, no mitigation       ",
			"D1 glFlush before the call            ",
			"D2 glFlush before every warm-up batch ",
		};

		for (int fix = 0; fix <= 2; ++fix) {
			int bad = 0;
			int firstBad = -1;

			for (int n = 0; n <= 16; ++n) {
				ResetFrame();

				for (int i = 0; i < n; ++i) {
					if (fix == 2)
						p_glFlush();
					DegenerateQuad(0, 0, 0, 0, 4);
				}

				if (fix == 1)
					p_glFlush();

				p_glCallList(list);

				if (Verify("", 1)) {
					bad++;
					if (firstBad < 0) firstBad = n;
				}
			}

			printf("  %s  %2d of 17 wrong", dnames[fix], bad);
			if (firstBad >= 0)
				printf(", first at N=%d", firstBad);
			printf("\n");
		}
	}

	// R: gl.Rect, which is glRectf. Mesa expands that into glBegin/glVertex/glEnd
	// inside the driver, so a wrapper around the engine's own glBegin calls
	// cannot reach it. The resource bar uses gl.Rect.
	if (!strcmp(only, "all") || !strcmp(only, "rect")) {
		printf("\n-- gl.Rect, which reaches glBegin inside Mesa --\n");

		static const char* rnames[] = {
			"R0 glRectf grid, no mitigation        ",
			"R1 glFlush before every glRectf       ",
		};

		for (int fix = 0; fix <= 1; ++fix) {
			int bad = 0;
			int firstBad = -1;

			for (int n = 0; n <= 16; ++n) {
				ResetFrame();

				for (int i = 0; i < n; ++i)
					DegenerateQuad(0, 0, 0, 0, 4);

				for (int cy = 0; cy < CELLS; ++cy) {
					for (int cx = 0; cx < CELLS; ++cx) {
						float x0, y0, x1, y1;
						CellRect(cx, cy, &x0, &y0, &x1, &y1);

						if (fix == 1)
							p_glFlush();

						p_glColor3f(1.0f, 1.0f, 1.0f);
						p_glRectf(x0, y0, x1, y1);
					}
				}

				if (Verify("", 1)) {
					bad++;
					if (firstBad < 0) firstBad = n;
				}
			}

			printf("  %s  %2d of 17 wrong", rnames[fix], bad);
			if (firstBad >= 0)
				printf(", first at N=%d", firstBad);
			printf("\n");
		}
	}

	eglDestroyContext(dpy, ctx);
	eglDestroySurface(dpy, surf);
	eglTerminate(dpy);
	return 0;
}
