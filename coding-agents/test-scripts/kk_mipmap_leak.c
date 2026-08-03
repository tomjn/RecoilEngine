// Standalone reproducer: glGenerateMipmap leaks about 23 MiB per call on zink
// over kosmickrisp. Written for an upstream Mesa report, so it deliberately
// depends on nothing but EGL and libGL.
//
// It creates a texture, allocates every mip level, generates the mips, and
// deletes the texture again. It never draws anything and never presents. On
// llvmpipe the footprint is flat. On zink over kosmickrisp it climbs by tens of
// MiB per iteration and does not come back.
//
// build:
//   cc -O2 -o kk_mipmap_leak kk_mipmap_leak.c \
//     -I$MESA_PREFIX/include -L$MESA_PREFIX/lib \
//     -lEGL -Wl,-rpath,$MESA_PREFIX/lib \
//     -Wl,-rpath,/opt/homebrew/opt/vulkan-loader/lib
//
// run:
//   EGL_PLATFORM=surfaceless MESA_LOADER_DRIVER_OVERRIDE=zink \
//   GALLIUM_DRIVER=zink LIBGL_DRIVERS_PATH=$MESA_PREFIX/lib \
//   VK_DRIVER_FILES=$MESA_PREFIX/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
//   ./kk_mipmap_leak 100
//
// It stops itself at 6 GiB. Do not remove that: at this rate an unattended run
// will take a 16 GB machine down hard enough to need a power cycle.

#include <EGL/egl.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;

#define GL_TEXTURE_2D           0x0DE1
#define GL_RGBA8                0x8058
#define GL_RGBA                 0x1908
#define GL_UNSIGNED_BYTE        0x1401
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_RENDERER             0x1F01

#define TEXSIZE 512
#define CEILING_MIB 6144

static void (*p_glGenTextures)(GLsizei, GLuint*);
static void (*p_glDeleteTextures)(GLsizei, const GLuint*);
static void (*p_glBindTexture)(GLenum, GLuint);
static void (*p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
static void (*p_glTexParameteri)(GLenum, GLenum, GLint);
static void (*p_glGenerateMipmap)(GLenum);
static void (*p_glFinish)(void);
static const GLubyte* (*p_glGetString)(GLenum);

#define LOAD(n) \
	p_##n = (void*)eglGetProcAddress(#n); \
	if (!p_##n) { fprintf(stderr, "missing %s\n", #n); return 1; }

// phys_footprint is the only number that means anything here. RSS does not
// count IOAccelerator allocations at all: measured 50 GB of footprint against
// an RSS of 1349 MB.
static unsigned long footprint_mib(void)
{
	task_vm_info_data_t info;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
		return 0;
	return (unsigned long)(info.phys_footprint >> 20);
}

int main(int argc, char** argv)
{
	const int count = (argc > 1) ? atoi(argv[1]) : 100;
	const int genmip = (getenv("NO_GENMIP") == NULL);
	const int sync = (getenv("SYNC") != NULL);

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

	const EGLint surfAttrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surfAttrs);

	const EGLint ctxAttrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE
	};
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "makeCurrent failed\n"); return 1; }

	LOAD(glGenTextures); LOAD(glDeleteTextures); LOAD(glBindTexture);
	LOAD(glTexImage2D); LOAD(glTexParameteri); LOAD(glGenerateMipmap);
	LOAD(glFinish); LOAD(glGetString);

	printf("GL_RENDERER = %s\n", p_glGetString(GL_RENDERER));
	printf("%d textures of %dx%d, glGenerateMipmap %s, footprint at start %lu MiB\n",
	       count, TEXSIZE, TEXSIZE, genmip ? "on" : "off", footprint_mib());
	fflush(stdout);

	unsigned char* pixels = malloc((size_t)TEXSIZE * TEXSIZE * 4);
	memset(pixels, 0x80, (size_t)TEXSIZE * TEXSIZE * 4);

	for (int t = 0; t < count; t++) {
		GLuint tex = 0;
		p_glGenTextures(1, &tex);
		p_glBindTexture(GL_TEXTURE_2D, tex);

		for (int level = 0, w = TEXSIZE, h = TEXSIZE;; level++) {
			p_glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, w, h, 0,
			               GL_RGBA, GL_UNSIGNED_BYTE, level ? NULL : pixels);
			if (w == 1 && h == 1)
				break;
			w = (w > 1) ? w / 2 : 1;
			h = (h > 1) ? h / 2 : 1;
		}
		p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

		if (genmip)
			p_glGenerateMipmap(GL_TEXTURE_2D);

		p_glBindTexture(GL_TEXTURE_2D, 0);
		p_glDeleteTextures(1, &tex);

		// Without this the work is still queued and the footprint lags badly:
		// it reads 236 MiB at texture 90 and then jumps to 2626 MiB at the
		// final glFinish. SYNC=1 makes each iteration's cost land where it is
		// incurred, which is also what makes the ceiling below effective.
		if (sync)
			p_glFinish();

		if ((t % 10) == 0) {
			const unsigned long mib = footprint_mib();
			printf("texture %4d, footprint %lu MiB\n", t, mib);
			fflush(stdout);
			if (mib > CEILING_MIB) {
				printf("stopping at the %d MiB ceiling\n", CEILING_MIB);
				return 2;
			}
		}
	}

	p_glFinish();
	free(pixels);
	printf("done, footprint %lu MiB after %d textures\n", footprint_mib(), count);
	return 0;
}
