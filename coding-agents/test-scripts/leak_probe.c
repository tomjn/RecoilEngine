// Does a plain GL draw loop leak memory on this driver?
//
// The engine grows about 5 GiB/s of phys_footprint on Zink over KosmicKrisp and
// is flat on llvmpipe with the same binary and content. Growth continues through
// load phases that issue no GL of their own, and disabling the present readback
// makes it worse rather than better, so the suspicion is ordinary drawing.
//
// This draws frames in a loop and prints a marker every 100 so an external
// sampler can line footprint up against frame count. Nothing is presented and
// nothing is read back.
//
// build:
//   gcc -o leak_probe leak_probe.c \
//     -I$HOME/dev/mesa-install/include -L$HOME/dev/mesa-install/lib \
//     -lEGL -Wl,-rpath,$HOME/dev/mesa-install/lib \
//     -Wl,-rpath,/opt/homebrew/opt/vulkan-loader/lib
//
// usage: leak_probe [frames] [batches-per-frame]

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef float GLfloat;

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_QUADS            0x0007
#define GL_RENDERER         0x1F01
#define GL_DEPTH_TEST       0x0B71
#define GL_MODELVIEW        0x1700
#define GL_PROJECTION       0x1701

#define SIZE 512

static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glBegin)(GLenum);
static void (*p_glEnd)(void);
static void (*p_glVertex2f)(GLfloat, GLfloat);
static void (*p_glColor3f)(GLfloat, GLfloat, GLfloat);
static void (*p_glTexCoord2f)(GLfloat, GLfloat);
static void (*p_glFinish)(void);
static void (*p_glFlush)(void);
static void (*p_glDisable)(GLenum);
static void (*p_glMatrixMode)(GLenum);
static void (*p_glLoadIdentity)(void);
static const GLubyte* (*p_glGetString)(GLenum);

#define LOAD(n) \
	p_##n = (void*)eglGetProcAddress(#n); \
	if (!p_##n) { fprintf(stderr, "missing %s\n", #n); return 1; }

int main(int argc, char** argv)
{
	const int frames = (argc > 1) ? atoi(argv[1]) : 100000;
	const int batches = (argc > 2) ? atoi(argv[2]) : 200;

	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major, minor;
	if (!eglInitialize(dpy, &major, &minor)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
	eglBindAPI(EGL_OPENGL_API);

	const EGLint cfgAttrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig cfg; EGLint numCfg = 0;
	eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &numCfg);

	const EGLint surfAttrs[] = { EGL_WIDTH, SIZE, EGL_HEIGHT, SIZE, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surfAttrs);

	const EGLint ctxAttrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
		EGL_NONE
	};
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "makeCurrent failed\n"); return 1; }

	LOAD(glClearColor); LOAD(glClear); LOAD(glViewport); LOAD(glBegin); LOAD(glEnd);
	LOAD(glVertex2f); LOAD(glColor3f); LOAD(glTexCoord2f); LOAD(glFinish); LOAD(glFlush);
	LOAD(glDisable); LOAD(glMatrixMode); LOAD(glLoadIdentity); LOAD(glGetString);

	printf("GL_RENDERER = %s\n", p_glGetString(GL_RENDERER));
	printf("frames=%d batches=%d, pid %d\n", frames, batches, (int)getpid());
	fflush(stdout);

	p_glViewport(0, 0, SIZE, SIZE);
	p_glDisable(GL_DEPTH_TEST);
	p_glMatrixMode(GL_PROJECTION); p_glLoadIdentity();
	p_glMatrixMode(GL_MODELVIEW);  p_glLoadIdentity();

	for (int f = 0; f < frames; f++) {
		p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		p_glClear(GL_COLOR_BUFFER_BIT);

		// the shape the engine draws: a run of batches, some carrying a texcoord
		for (int b = 0; b < batches; b++) {
			const float x = -1.0f + 2.0f * ((b % 16) / 16.0f);
			const float y = -1.0f + 2.0f * ((b / 16) / 16.0f);

			p_glBegin(GL_QUADS);
				if (b & 1) p_glTexCoord2f(0.5f, 0.5f);
				p_glColor3f(1.0f, 0.2f, 0.2f);
				p_glVertex2f(x, y);
				p_glVertex2f(x + 0.1f, y);
				p_glVertex2f(x + 0.1f, y + 0.1f);
				p_glVertex2f(x, y + 0.1f);
			p_glEnd();
		}

		// SPRING_PROBE_NO_FINISH tests the theory that the driver's threaded
		// context queues without bound when nothing synchronises per frame. The
		// engine has no glFinish, and its only sync was the present readback,
		// which is exactly what disabling made worse.
		static int noFinish = -1;
		if (noFinish < 0) noFinish = (getenv("SPRING_PROBE_NO_FINISH") != NULL);
		if (!noFinish) p_glFinish();

		if ((f % 100) == 0) {
			printf("frame %d\n", f);
			fflush(stdout);
		}
	}

	return 0;
}
