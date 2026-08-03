// Does this driver support GL timer queries?
//
// The engine's per-frame GPU time comes from a GL_TIMESTAMP pair that
// CGame::Draw writes and ProfileDrawer reads. Reading it from SwapBuffers
// returned nothing available on 75 consecutive frames, so either the extension
// is absent or the query never completes. Without it there is no way to separate
// the GPU's time to draw a frame from the CPU's time waiting for it, and
// SwapBuffers is 94% of wall clock, so this is worth settling.
//
// Build and run against the same Mesa the engine uses:
//
//   PREFIX=$HOME/dev/mesa-install-premtl4
//   gcc -O1 -I$PREFIX/include -o /tmp/timer_probe timer_probe.c -L$PREFIX/lib -lEGL
//   EGL_PLATFORM=surfaceless MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
//     LIBGL_DRIVERS_PATH=$PREFIX/lib \
//     VK_DRIVER_FILES=$PREFIX/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
//     MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460 /tmp/timer_probe

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
typedef unsigned long long GLuint64;
typedef unsigned char GLubyte;
typedef float GLfloat;

#define GL_COLOR_BUFFER_BIT       0x00004000
#define GL_QUADS                  0x0007
#define GL_VERSION                0x1F02
#define GL_RENDERER               0x1F01
#define GL_EXTENSIONS             0x1F03
#define GL_TIMESTAMP              0x8E28
#define GL_QUERY_RESULT           0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_NO_ERROR               0

#define SIZE 256

static const GLubyte* (*p_glGetString)(GLenum);
static GLenum (*p_glGetError)(void);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glBegin)(GLenum);
static void (*p_glEnd)(void);
static void (*p_glVertex2f)(GLfloat, GLfloat);
static void (*p_glFinish)(void);
static void (*p_glGenQueries)(GLsizei, GLuint*);
static void (*p_glQueryCounter)(GLuint, GLenum);
static void (*p_glGetQueryObjectiv)(GLuint, GLenum, GLint*);
static void (*p_glGetQueryObjectui64v)(GLuint, GLenum, GLuint64*);

#define LOAD(name) do { \
	p_##name = (void*) eglGetProcAddress(#name); \
	if (p_##name == NULL) printf("MISSING entry point: %s\n", #name); \
} while (0)

int main(void)
{
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

	// Compatibility profile, the same as the engine asks for
	const EGLint ctxAttrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
		EGL_NONE
	};

	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "makeCurrent failed\n"); return 1; }

	LOAD(glGetString); LOAD(glGetError); LOAD(glClearColor); LOAD(glClear);
	LOAD(glViewport); LOAD(glBegin); LOAD(glEnd); LOAD(glVertex2f); LOAD(glFinish);
	LOAD(glGenQueries); LOAD(glQueryCounter);
	LOAD(glGetQueryObjectiv); LOAD(glGetQueryObjectui64v);

	printf("GL_RENDERER = %s\n", (const char*) p_glGetString(GL_RENDERER));
	printf("GL_VERSION  = %s\n", (const char*) p_glGetString(GL_VERSION));

	const char* ext = (const char*) p_glGetString(GL_EXTENSIONS);
	printf("GL_ARB_timer_query advertised: %s\n",
		(ext != NULL && strstr(ext, "GL_ARB_timer_query") != NULL) ? "yes" : "no");
	printf("GL_EXT_disjoint_timer_query advertised: %s\n",
		(ext != NULL && strstr(ext, "GL_EXT_disjoint_timer_query") != NULL) ? "yes" : "no");

	if (p_glGenQueries == NULL || p_glQueryCounter == NULL) {
		printf("\nno entry points, so the engine cannot time the GPU this way\n");
		return 0;
	}

	GLuint q[2] = { 0, 0 };
	p_glGenQueries(2, q);
	printf("\nglGenQueries gave %u %u, error 0x%04x\n", q[0], q[1], p_glGetError());

	// A name from glGenQueries is not a query object until something writes to
	// it. Ask about an unwritten one first, because that is the state the engine
	// hits during loading and it must not be waited on.
	GLint avail = -1;
	p_glGetQueryObjectiv(q[0], GL_QUERY_RESULT_AVAILABLE, &avail);
	printf("unwritten query: available=%d error=0x%04x\n", avail, p_glGetError());

	p_glViewport(0, 0, SIZE, SIZE);
	p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	for (int frame = 0; frame < 3; frame++) {
		p_glQueryCounter(q[0], GL_TIMESTAMP);
		const GLenum errStamp = p_glGetError();

		p_glClear(GL_COLOR_BUFFER_BIT);
		for (int i = 0; i < 2000; i++) {
			p_glBegin(GL_QUADS);
			p_glVertex2f(-1.0f, -1.0f); p_glVertex2f(1.0f, -1.0f);
			p_glVertex2f(1.0f, 1.0f);   p_glVertex2f(-1.0f, 1.0f);
			p_glEnd();
		}

		p_glQueryCounter(q[1], GL_TIMESTAMP);
		p_glFinish();

		avail = -1;
		p_glGetQueryObjectiv(q[1], GL_QUERY_RESULT_AVAILABLE, &avail);
		const GLenum errAvail = p_glGetError();

		GLuint64 t0 = 0, t1 = 0;
		p_glGetQueryObjectui64v(q[0], GL_QUERY_RESULT, &t0);
		p_glGetQueryObjectui64v(q[1], GL_QUERY_RESULT, &t1);
		const GLenum errRead = p_glGetError();

		printf("frame %d: stamp err 0x%04x, available=%d err 0x%04x, read err 0x%04x, "
			"t0=%llu t1=%llu delta=%.3fms\n",
			frame, errStamp, avail, errAvail, errRead, t0, t1, (double)(t1 - t0) * 1.0e-6);
	}

	return 0;
}
