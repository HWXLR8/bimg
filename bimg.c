#define _GNU_SOURCE
// bimg — Wayland image viewer that becomes a cropper on `c`
// usage: bimg [-f] [-o OUT] [-l|--jpeg-lossless] <file-or-dir>...
// view keys: ←/→ s/t nav, +/-/wheel zoom, drag pan, 1 1:1, 0 fit, f fullscreen,
//            c crop, q/Esc quit
// crop keys: left-drag crop/move/resize, right-drag pan, wheel zoom, 0 fit,
//            1 1:1, 4 4:3, 5 free, 6 16:9, o original, r reset, ? help,
//            f fullscreen, Enter save, c/Esc exit crop
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "image.h"
#include "xdg-shell-protocol.h"

enum drag_mode {
    DRAG_NONE,
    DRAG_NEW,
    DRAG_MOVE,
    DRAG_PAN,
    DRAG_LEFT = 1 << 4,
    DRAG_RIGHT = 1 << 5,
    DRAG_TOP = 1 << 6,
    DRAG_BOTTOM = 1 << 7,
};

struct rect {
    double x, y, w, h;
};

struct bimg {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *compositor;
	struct wl_surface *surface;
	struct wl_egl_window *ewin;
	struct xdg_wm_base *wm_base;
	struct xdg_surface *xdg;
	struct xdg_toplevel *top;
	struct wl_seat *seat;
	struct wl_keyboard *kb;
	struct wl_pointer *ptr;
	struct wl_output *out;

	EGLDisplay edpy;
	EGLSurface esurf;
	EGLContext ectx;
	EGLConfig ecfg;

	int32_t w, h; // window size, logical px
	int32_t scale; // output scale
	bool have_geo;
	int32_t geo_w, geo_h; // output geometry, physical px
	int max_tex;

	char **files;
	int nfiles;
	int cur; // -1 = none loaded
	struct image img;
	uint32_t *tex_px; // display-sized scratch (halved if oversize)
	int iw, ih; // display image size, logical px

	double zoom, px, py; // screen = img * zoom + p (logical px)
	bool fit;
	double mx, my;
	enum drag_mode drag; // crop-mode drag; DRAG_NONE when idle
	int lbtn; // 0 = up, 1 = down (view-mode left drag)
	struct rect crop; // in original image px
	struct rect drag_crop;
	double drag_x, drag_y;
	double drag_anchor_x, drag_anchor_y;
	double aspect; // > 0 = locked
	bool crop_mode;
	bool fullscreen;
	bool show_help;
	char status[96];
	const char *out_path;
	bool force;
	bool lossless;

	bool redraw;
	bool have_img;
	bool configured;
	bool running;
	int anim_fd;
	int exit_status;

	GLuint prog, vbo, tex, wtex, ftex;
	GLuint a_pos, a_uv, u_win, u_tex, u_wtex, u_ftex, u_tint;
};

static struct bimg B;

static void __attribute__((format(printf, 1, 2))) die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("bimg: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void mark_redraw(void)
{
	B.redraw = true;
}

static void handle_key(uint32_t key);
static void sync_animation(void);

// ---------- view ----------

static void refit(void)
{
	if (B.w <= 0 || B.h <= 0)
		return;
	double z = fmin((double)B.w / B.iw, (double)B.h / B.ih);
	B.zoom = z;
	B.px = (B.w - B.iw * z) / 2.0;
	B.py = (B.h - B.ih * z) / 2.0;
	B.fit = true;
	B.redraw = true;
}

static void one_to_one(void)
{
	B.zoom = 1.0;
	B.px = (B.w - B.iw) / 2.0;
	B.py = (B.h - B.ih) / 2.0;
	B.fit = false;
	B.redraw = true;
}

static void zoom_at(double x, double y, double f)
{
	double minimum, maximum, nz;
	if (B.crop_mode) {
		// zoom is relative to the displayed (possibly halved) image
		minimum = fmin((double)B.w / B.iw, (double)B.h / B.ih) * 0.1;
		maximum = 64.0;
	} else {
		minimum = 0.001;
		maximum = 512.0;
	}
	nz = clampd(B.zoom * f, minimum, maximum);
	B.px = x - (x - B.px) * (nz / B.zoom);
	B.py = y - (y - B.py) * (nz / B.zoom);
	B.zoom = nz;
	B.fit = false;
	B.redraw = true;
}

static void screen_to_image(double sx, double sy, double *ix, double *iy)
{
	*ix = (sx - B.px) / B.zoom;
	*iy = (sy - B.py) / B.zoom;
}

// ---------- image files ----------

static bool is_img_name(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot)
		return false;
	dot++;
	static const char *exts[] = {"png", "jpg", "jpeg", "gif", "webp"};
	for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
		if (strcasecmp(dot, exts[i]) == 0)
			return true;
	return false;
}

static void files_push(const char *path)
{
	char **nf = realloc(B.files, (B.nfiles + 1) * sizeof *B.files);
	if (!nf)
		die("out of memory");
	B.files = nf;
	B.files[B.nfiles++] = strdup(path);
}

static int cmp_name(const void *a, const void *b)
{
	return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void add_dir(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) {
		fprintf(stderr, "bimg: %s: %s\n", dir, strerror(errno));
		return;
	}
	size_t cap = 16, n = 0;
	char **names = malloc(cap * sizeof *names);
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !is_img_name(e->d_name))
			continue;
		size_t len = strlen(dir) + 1 + strlen(e->d_name) + 1;
		char *p = malloc(len);
		snprintf(p, len, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
			free(p);
			continue;
		}
		if (n == cap) {
			cap *= 2;
			char **nn = realloc(names, cap * sizeof *names);
			if (!nn)
				die("out of memory");
			names = nn;
		}
		names[n++] = p;
	}
	closedir(d);
	qsort(names, n, sizeof *names, cmp_name);
	for (size_t i = 0; i < n; i++) {
		files_push(names[i]);
		free(names[i]);
	}
	free(names);
}

// ---------- GL ----------

static const char vs_src[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec2 u_win;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4((2.0*a_pos.x - u_win.x)/u_win.x,\n"
    "                     (u_win.y - 2.0*a_pos.y)/u_win.y, 0.0, 1.0);\n"
    "}\n";

// u_tint.a selects the texture: < 0.5 image (opaque, drawn with blending
// off), < 1.5 white texel tinted, else font luminance
static const char fs_src[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_wtex;\n"
    "uniform sampler2D u_ftex;\n"
    "uniform vec4 u_tint;\n"
    "void main() {\n"
    "  vec4 c = u_tint.a < 0.5 ? texture2D(u_tex, v_uv)\n"
    "           : u_tint.a < 1.5 ? vec4(texture2D(u_wtex, v_uv).rgb, 1.0)\n"
    "           : texture2D(u_ftex, v_uv);\n"
    "  gl_FragColor = c * u_tint;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		die("shader: %s", log);
	}
	return s;
}

// 5x7 glyphs, 5 bits per row (col 0 = bit 4)
static const uint8_t font_cpu[128][7] = {
    [' '] = {0, 0, 0, 0, 0, 0, 0},
    ['+'] = {0, 4, 4, 31, 4, 4, 0},
    [','] = {0, 0, 0, 0, 0, 6, 4},
    ['-'] = {0, 0, 0, 31, 0, 0, 0},
    ['.'] = {0, 0, 0, 0, 0, 6, 6},
    ['/'] = {1, 2, 2, 4, 8, 8, 16},
    ['0'] = {14, 17, 19, 21, 25, 17, 14},
    ['1'] = {4, 12, 4, 4, 4, 4, 14},
    ['2'] = {14, 17, 1, 2, 4, 8, 31},
    ['3'] = {30, 1, 1, 14, 1, 1, 30},
    ['4'] = {2, 6, 10, 18, 31, 2, 2},
    ['5'] = {31, 16, 16, 30, 1, 1, 30},
    ['6'] = {14, 16, 16, 30, 17, 17, 14},
    ['7'] = {31, 1, 2, 4, 8, 8, 8},
    ['8'] = {14, 17, 17, 14, 17, 17, 14},
    ['9'] = {14, 17, 17, 15, 1, 1, 14},
    [':'] = {0, 6, 6, 0, 6, 6, 0},
    ['?'] = {14, 17, 1, 2, 4, 0, 4},
    ['A'] = {14, 17, 17, 31, 17, 17, 17},
    ['B'] = {30, 17, 17, 30, 17, 17, 30},
    ['C'] = {14, 17, 16, 16, 16, 17, 14},
    ['D'] = {30, 17, 17, 17, 17, 17, 30},
    ['E'] = {31, 16, 16, 30, 16, 16, 31},
    ['F'] = {31, 16, 16, 30, 16, 16, 16},
    ['G'] = {14, 17, 16, 23, 17, 17, 15},
    ['H'] = {17, 17, 17, 31, 17, 17, 17},
    ['I'] = {14, 4, 4, 4, 4, 4, 14},
    ['J'] = {7, 2, 2, 2, 2, 18, 12},
    ['K'] = {17, 18, 20, 24, 20, 18, 17},
    ['L'] = {16, 16, 16, 16, 16, 16, 31},
    ['M'] = {17, 27, 21, 21, 17, 17, 17},
    ['N'] = {17, 25, 21, 19, 17, 17, 17},
    ['O'] = {14, 17, 17, 17, 17, 17, 14},
    ['P'] = {30, 17, 17, 30, 16, 16, 16},
    ['Q'] = {14, 17, 17, 17, 21, 18, 13},
    ['R'] = {30, 17, 17, 30, 20, 18, 17},
    ['S'] = {15, 16, 16, 14, 1, 1, 30},
    ['T'] = {31, 4, 4, 4, 4, 4, 4},
    ['U'] = {17, 17, 17, 17, 17, 17, 14},
    ['V'] = {17, 17, 17, 17, 17, 10, 4},
    ['W'] = {17, 17, 17, 21, 21, 21, 10},
    ['X'] = {17, 17, 10, 4, 10, 17, 17},
    ['Y'] = {17, 17, 10, 4, 4, 4, 4},
    ['Z'] = {31, 1, 2, 4, 8, 16, 31},
};

static void gl_init(void)
{
	B.prog = glCreateProgram();
	glAttachShader(B.prog, compile_shader(GL_VERTEX_SHADER, vs_src));
	glAttachShader(B.prog, compile_shader(GL_FRAGMENT_SHADER, fs_src));
	glLinkProgram(B.prog);
	GLint ok;
	glGetProgramiv(B.prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(B.prog, sizeof log, NULL, log);
		die("link: %s", log);
	}
	B.a_pos = (GLuint)glGetAttribLocation(B.prog, "a_pos");
	B.a_uv = (GLuint)glGetAttribLocation(B.prog, "a_uv");
	B.u_win = glGetUniformLocation(B.prog, "u_win");
	B.u_tex = glGetUniformLocation(B.prog, "u_tex");
	B.u_wtex = glGetUniformLocation(B.prog, "u_wtex");
	B.u_ftex = glGetUniformLocation(B.prog, "u_ftex");
	B.u_tint = glGetUniformLocation(B.prog, "u_tint");
	glGenBuffers(1, &B.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, B.vbo);
	glBufferData(GL_ARRAY_BUFFER, 16 * 4, NULL, GL_STREAM_DRAW);
	glGenTextures(1, &B.wtex);
	glBindTexture(GL_TEXTURE_2D, B.wtex);
	uint8_t white = 255;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 1, 1, 0, GL_LUMINANCE,
		     GL_UNSIGNED_BYTE, &white);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	// font texture: 128 glyphs x 5 px wide, 7 px tall
	uint8_t fb[640 * 7];
	for (int r = 0; r < 7; r++)
		for (int ch = 0; ch < 128; ch++)
			for (int c = 0; c < 5; c++)
				fb[r * 640 + ch * 5 + c] =
				    (font_cpu[ch][r] >> (4 - c)) & 1 ? 255 : 0;
	glGenTextures(1, &B.ftex);
	glBindTexture(GL_TEXTURE_2D, B.ftex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 640, 7, 0, GL_LUMINANCE,
		     GL_UNSIGNED_BYTE, fb);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, (GLint *)&B.max_tex);
}

// u_tint.a < 0.5: image texel (opaque, blending off); < 1.5: white texel;
// else font
static void quad(float x0, float y0, float x1, float y1, float u0, float v0,
		 float u1, float v1, float r, float g, float bl, float a)
{
	static const float ix[4] = {0, 1, 0, 1};
	static const float iy[4] = {0, 0, 1, 1};
	static const float iu[4] = {0, 1, 0, 1};
	static const float iv[4] = {0, 0, 1, 1};
	float v[4][4];
	for (int i = 0; i < 4; i++) {
		v[i][0] = ix[i] * (x1 - x0) + x0;
		v[i][1] = iy[i] * (y1 - y0) + y0;
		v[i][2] = iu[i] * (u1 - u0) + u0;
		v[i][3] = iv[i] * (v1 - v0) + v0;
	}
	glBindBuffer(GL_ARRAY_BUFFER, B.vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
	glEnableVertexAttribArray(B.a_pos);
	glVertexAttribPointer(B.a_pos, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
	glEnableVertexAttribArray(B.a_uv);
	glVertexAttribPointer(B.a_uv, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
	float tint[4] = {r, g, bl, a};
	glUniform4f(B.u_tint, tint[0], tint[1], tint[2], tint[3]);
	if (a < 0.5f)
		glDisable(GL_BLEND);
	else
		glEnable(GL_BLEND);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// one quad per 5x7 glyph, 6 px advance
static void draw_text(const char *text, float x, float y, float ps, float a)
{
	for (; *text; text++, x += 6.f * ps) {
		unsigned char ch = (unsigned char)*text;
		if (ch >= 'a' && ch <= 'z')
			ch = (unsigned char)(ch - 'a' + 'A');
		if (ch >= 128)
			ch = '?';
		float u0 = (float)(ch * 5) / 640.f;
		float u1 = (float)(ch * 5 + 5) / 640.f;
		quad(x, y, x + 5.f * ps, y + 7.f * ps, u0, 0.f, u1, 1.f, 1.f, 1.f, 1.f,
		     a);
	}
}

// ---------- image loading ----------

static void free_display_image(void)
{
	if (B.tex) {
		glDeleteTextures(1, &B.tex);
		B.tex = 0;
	}
	free(B.tex_px);
	B.tex_px = NULL;
}

// image pixels are 0xAARRGGBB; GL_RGBA/UNSIGNED_BYTE wants RGBA bytes
static uint32_t argb_to_rgba(uint32_t p)
{
	return (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
}

// build the display texture from B.img; halves it until it fits max_tex
static void build_texture(void)
{
	int ow = B.img.width, oh = B.img.height;
	double ds = 1.0;
	while (ow / ds > B.max_tex || oh / ds > B.max_tex)
		ds *= 2.0;
	B.iw = (int)(ow / ds);
	B.ih = (int)(oh / ds);
	free_display_image();
	B.tex_px = malloc((size_t)B.iw * B.ih * 4);
	if (!B.tex_px)
		die("out of memory");
	for (int y = 0; y < B.ih; y++) {
		int sy = (int)((double)y * ds);
		const uint32_t *sp = image_pixels(&B.img) + (size_t)sy * ow;
		uint32_t *dp = B.tex_px + (size_t)y * B.iw;
		for (int x = 0; x < B.iw; x++) {
			int sx = (int)((double)x * ds);
			dp[x] = argb_to_rgba(sp[sx]);
		}
	}
	GLuint t = 0;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, B.iw, B.ih, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, B.tex_px);
	B.tex = t;
}

// load a file into B.img and rebuild the texture; frees the previous image
static bool load_image_path(const char *path)
{
	struct image tmp = {0};
	char err[256];
	if (image_load(&tmp, path, err, sizeof err) != 0) {
		fprintf(stderr, "bimg: %s: %s\n", path, err);
		return false;
	}
	if (B.have_img)
		image_free(&B.img);
	B.img = tmp;
	build_texture();
	B.have_img = true;
	return true;
}

static bool load_current(int idx)
{
	return load_image_path(B.files[idx]);
}

static bool next_image(int dir)
{
	if (B.nfiles == 0)
		return false;
	for (int step = 1; step <= B.nfiles; step++) {
		int idx;
		if (B.cur < 0)
			idx = dir > 0 ? 0 : B.nfiles - 1;
		else {
			idx = (B.cur + dir * step) % B.nfiles;
			if (idx < 0)
				idx += B.nfiles;
		}
		if (idx == B.cur)
			continue;
		if (load_current(idx)) {
			B.cur = idx;
			B.crop_mode = false;
			B.show_help = false;
			B.status[0] = '\0';
			if (B.w > 0)
				refit();
			sync_animation();
			return true;
		}
	}
	return false;
}

// ---------- crop ----------

static void snap_lossless_origin(struct rect *rect)
{
	int mw, mh;
	if (!B.lossless || B.img.format != IMAGE_JPEG)
		return;
	image_jpeg_mcu(&B.img, &mw, &mh);
	double max_x = B.img.width - rect->w;
	double max_y = B.img.height - rect->h;
	double snapped_x = round(rect->x / mw) * mw;
	double snapped_y = round(rect->y / mh) * mh;
	double last_x = floor(max_x / mw) * mw;
	double last_y = floor(max_y / mh) * mh;
	rect->x = clampd(snapped_x, 0.0, fmax(0.0, last_x));
	rect->y = clampd(snapped_y, 0.0, fmax(0.0, last_y));
}

static void apply_aspect(double aspect)
{
	double cx = B.crop.x + B.crop.w / 2.0;
	double cy = B.crop.y + B.crop.h / 2.0;
	double width = B.crop.w;
	double height = B.crop.h;

	B.aspect = aspect;
	if (aspect <= 0.0) {
		mark_redraw();
		return;
	}
	if (width / height > aspect)
		width = height * aspect;
	else
		height = width / aspect;
	B.crop.w = fmax(1.0, width);
	B.crop.h = fmax(1.0, height);
	B.crop.x = clampd(cx - B.crop.w / 2.0, 0.0, B.img.width - B.crop.w);
	B.crop.y = clampd(cy - B.crop.h / 2.0, 0.0, B.img.height - B.crop.h);
	snap_lossless_origin(&B.crop);
	mark_redraw();
}

static enum drag_mode hit_test(double x, double y)
{
	double left = B.px + B.crop.x * B.zoom;
	double top = B.py + B.crop.y * B.zoom;
	double right = left + B.crop.w * B.zoom;
	double bottom = top + B.crop.h * B.zoom;
	const double threshold = 9.0;
	enum drag_mode mode = DRAG_NONE;

	if (fabs(x - left) <= threshold && y >= top - threshold && y <= bottom + threshold)
		mode = (enum drag_mode)(mode | DRAG_LEFT);
	else if (fabs(x - right) <= threshold && y >= top - threshold &&
		 y <= bottom + threshold)
		mode = (enum drag_mode)(mode | DRAG_RIGHT);
	if (fabs(y - top) <= threshold && x >= left - threshold && x <= right + threshold)
		mode = (enum drag_mode)(mode | DRAG_TOP);
	else if (fabs(y - bottom) <= threshold && x >= left - threshold &&
		 x <= right + threshold)
		mode = (enum drag_mode)(mode | DRAG_BOTTOM);
	if (mode != DRAG_NONE)
		return mode;
	if (x >= left && x <= right && y >= top && y <= bottom)
		return DRAG_MOVE;
	return DRAG_NEW;
}

static void fixed_rect_from_corner(double ax, double ay, double px, double py,
				   struct rect *rect)
{
	double sign_x = px >= ax ? 1.0 : -1.0;
	double sign_y = py >= ay ? 1.0 : -1.0;
	double width = fabs(px - ax);
	double height = fabs(py - ay);
	double max_width = sign_x > 0 ? B.img.width - ax : ax;
	double max_height = sign_y > 0 ? B.img.height - ay : ay;

	if (width / fmax(height, 0.0001) > B.aspect)
		height = width / B.aspect;
	else
		width = height * B.aspect;
	width = fmin(width, max_width);
	height = width / B.aspect;
	if (height > max_height) {
		height = max_height;
		width = height * B.aspect;
	}
	width = fmax(width, 1.0);
	height = fmax(height, 1.0);
	rect->x = sign_x > 0 ? ax : ax - width;
	rect->y = sign_y > 0 ? ay : ay - height;
	rect->w = width;
	rect->h = height;
}

static void drag_fixed_edge(double ix, double iy, struct rect *rect)
{
	struct rect old = B.drag_crop;

	if (B.drag & (DRAG_LEFT | DRAG_RIGHT)) {
		double anchor_x = B.drag & DRAG_LEFT ? old.x + old.w : old.x;
		double width = fabs(ix - anchor_x);
		double height = width / B.aspect;
		double center_y = old.y + old.h / 2.0;
		double max_width = B.drag & DRAG_LEFT ? anchor_x : B.img.width - anchor_x;
		double max_height = 2.0 * fmin(center_y, B.img.height - center_y);
		width = fmin(fmax(width, 1.0), fmin(max_width, max_height * B.aspect));
		height = width / B.aspect;
		rect->x = B.drag & DRAG_LEFT ? anchor_x - width : anchor_x;
		rect->y = center_y - height / 2.0;
		rect->w = width;
		rect->h = height;
	} else {
		double anchor_y = B.drag & DRAG_TOP ? old.y + old.h : old.y;
		double height = fabs(iy - anchor_y);
		double width = height * B.aspect;
		double center_x = old.x + old.w / 2.0;
		double max_height = B.drag & DRAG_TOP ? anchor_y : B.img.height - anchor_y;
		double max_width = 2.0 * fmin(center_x, B.img.width - center_x);
		height = fmin(fmax(height, 1.0), fmin(max_height, max_width / B.aspect));
		width = height * B.aspect;
		rect->x = center_x - width / 2.0;
		rect->y = B.drag & DRAG_TOP ? anchor_y - height : anchor_y;
		rect->w = width;
		rect->h = height;
	}
}

static void update_drag(double x, double y)
{
	double ix, iy;
	struct rect rect = B.drag_crop;

	if (B.drag == DRAG_NONE)
		return;
	if (B.drag == DRAG_PAN) {
		B.px += x - B.drag_x;
		B.py += y - B.drag_y;
		B.drag_x = x;
		B.drag_y = y;
		B.fit = false;
		mark_redraw();
		return;
	}

	screen_to_image(x, y, &ix, &iy);
	ix = clampd(ix, 0.0, B.img.width);
	iy = clampd(iy, 0.0, B.img.height);
	if (B.drag == DRAG_MOVE) {
		double dx = (x - B.drag_x) / B.zoom;
		double dy = (y - B.drag_y) / B.zoom;
		rect.x = clampd(B.drag_crop.x + dx, 0.0, B.img.width - rect.w);
		rect.y = clampd(B.drag_crop.y + dy, 0.0, B.img.height - rect.h);
	} else if (B.drag == DRAG_NEW) {
		if (B.aspect > 0.0) {
			fixed_rect_from_corner(B.drag_anchor_x, B.drag_anchor_y, ix, iy,
					       &rect);
		} else {
			rect.x = fmin(B.drag_anchor_x, ix);
			rect.y = fmin(B.drag_anchor_y, iy);
			rect.w = fmax(1.0, fabs(ix - B.drag_anchor_x));
			rect.h = fmax(1.0, fabs(iy - B.drag_anchor_y));
		}
	} else if (B.aspect > 0.0) {
		bool horizontal = (B.drag & (DRAG_LEFT | DRAG_RIGHT)) != 0;
		bool vertical = (B.drag & (DRAG_TOP | DRAG_BOTTOM)) != 0;
		if (horizontal && vertical) {
			double ax = B.drag & DRAG_LEFT ? B.drag_crop.x + B.drag_crop.w
						       : B.drag_crop.x;
			double ay = B.drag & DRAG_TOP ? B.drag_crop.y + B.drag_crop.h
						      : B.drag_crop.y;
			fixed_rect_from_corner(ax, ay, ix, iy, &rect);
		} else {
			drag_fixed_edge(ix, iy, &rect);
		}
	} else {
		double left = rect.x;
		double right = rect.x + rect.w;
		double top = rect.y;
		double bottom = rect.y + rect.h;
		if (B.drag & DRAG_LEFT)
			left = fmin(ix, right - 1.0);
		if (B.drag & DRAG_RIGHT)
			right = fmax(ix, left + 1.0);
		if (B.drag & DRAG_TOP)
			top = fmin(iy, bottom - 1.0);
		if (B.drag & DRAG_BOTTOM)
			bottom = fmax(iy, top + 1.0);
		rect.x = left;
		rect.y = top;
		rect.w = right - left;
		rect.h = bottom - top;
	}
	rect.w = clampd(rect.w, 1.0, B.img.width);
	rect.h = clampd(rect.h, 1.0, B.img.height);
	rect.x = clampd(rect.x, 0.0, B.img.width - rect.w);
	rect.y = clampd(rect.y, 0.0, B.img.height - rect.h);
	snap_lossless_origin(&rect);
	B.crop = rect;
	mark_redraw();
}

static void enter_crop(void)
{
	B.crop_mode = true;
	B.crop = (struct rect){0, 0, (double)B.img.width, (double)B.img.height};
	B.aspect = 0.0;
	B.show_help = false;
	B.status[0] = '\0';
	refit();
}

// ---------- save ----------

// output format of a crop: animated GIF becomes WebP, everything else keeps
// the input format
static enum image_format save_format(void)
{
	if (B.img.format == IMAGE_GIF && image_animated(&B.img))
		return IMAGE_WEBP;
	return B.img.format;
}

static void build_save_path(char *buf, size_t n)
{
	if (B.out_path) {
		snprintf(buf, n, "%s", B.out_path);
		return;
	}
	const char *path = B.files[B.cur];
	const char *dot = strrchr(path, '.');
	size_t len = dot && dot > path ? (size_t)(dot - path) : strlen(path);
	const char *ext;
	enum image_format f = save_format();
	if (dot && dot > path && B.img.format != IMAGE_GIF)
		ext = dot + 1;
	else
		ext = image_format_name(f);
	snprintf(buf, n, "%.*s.crop.%s", (int)len, path, ext);
}

static int save_crop(const char *dest)
{
	char *temporary = NULL;
	const char *slash;
	struct stat old_stat;
	bool had_destination = lstat(dest, &old_stat) == 0;
	int x, y, width, height, fd;
	int result = -1;
	char encode_error[256];

	x = clampi((int)floor(B.crop.x), 0, B.img.width - 1);
	y = clampi((int)floor(B.crop.y), 0, B.img.height - 1);
	width = clampi((int)ceil(B.crop.x + B.crop.w) - x, 1, B.img.width - x);
	height = clampi((int)ceil(B.crop.y + B.crop.h) - y, 1, B.img.height - y);

	slash = strrchr(dest, '/');
	size_t directory_length = slash ? (size_t)(slash - dest + 1) : 0;
	static const char suffix[] = ".bimg.tmp.XXXXXX";
	temporary = malloc(directory_length + sizeof(suffix));
	if (!temporary) {
		fprintf(stderr, "bimg: out of memory\n");
		goto out;
	}
	if (directory_length)
		memcpy(temporary, dest, directory_length);
	memcpy(temporary + directory_length, suffix, sizeof(suffix));
	fd = mkstemp(temporary);
	if (fd < 0) {
		fprintf(stderr, "bimg: cannot create temporary file: %s\n",
			strerror(errno));
		goto out;
	}
	close(fd);

	if (image_save_crop(&B.img, temporary, x, y, width, height, B.lossless,
			    encode_error, sizeof(encode_error)) < 0) {
		fprintf(stderr, "bimg: cannot save %s: %s\n", dest, encode_error);
		goto out;
	}

	if (had_destination) {
		if (chmod(temporary, old_stat.st_mode & 0777) < 0) {
			fprintf(stderr,
				"bimg: cannot preserve destination permissions: %s\n",
				strerror(errno));
			goto out;
		}
	} else {
		mode_t mask = umask(0);
		umask(mask);
		if (chmod(temporary, 0666 & ~mask) < 0) {
			fprintf(stderr,
				"bimg: cannot set destination permissions: %s\n",
				strerror(errno));
			goto out;
		}
	}

	fd = open(temporary, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fsync(fd) < 0) {
		fprintf(stderr, "bimg: cannot flush cropped image: %s\n",
			strerror(errno));
		if (fd >= 0)
			close(fd);
		goto out;
	}
	close(fd);

	if (B.force) {
		if (rename(temporary, dest) < 0) {
			fprintf(stderr, "bimg: cannot replace %s: %s\n", dest,
				strerror(errno));
			goto out;
		}
	} else {
		if (link(temporary, dest) < 0) {
			fprintf(stderr, "bimg: cannot create %s: %s\n", dest,
				strerror(errno));
			goto out;
		}
		if (unlink(temporary) < 0)
			fprintf(stderr, "bimg: warning: cannot remove %s: %s\n",
				temporary, strerror(errno));
	}
	result = 0;

out:
	if (result != 0 && temporary)
		unlink(temporary);
	free(temporary);
	return result;
}

// reload the saved file as the current image
static void reload_saved(const char *dest)
{
	char *np = strdup(dest);
	if (!np)
		die("out of memory");
	free(B.files[B.cur]);
	B.files[B.cur] = np;
	if (load_image_path(dest)) {
		B.crop_mode = false;
		B.show_help = false;
		B.status[0] = '\0';
		B.crop = (struct rect){0, 0, (double)B.img.width,
					 (double)B.img.height};
		B.aspect = 0.0;
		refit();
		sync_animation();
	} else {
		snprintf(B.status, sizeof(B.status), "RELOAD FAILED - SEE STDERR");
		mark_redraw();
	}
}

static void do_save(void)
{
	char dest[4096];
	struct stat st;
	enum image_format f = save_format();

	build_save_path(dest, sizeof(dest));
	if (B.lossless && B.img.format != IMAGE_JPEG) {
		snprintf(B.status, sizeof(B.status), "LOSSLESS NEEDS JPEG");
	} else if (B.out_path && !image_extension_matches(f, B.out_path)) {
		snprintf(B.status, sizeof(B.status), "OUTPUT MUST BE %s",
			 image_format_name(f));
	} else if (lstat(dest, &st) == 0 && !B.force) {
		snprintf(B.status, sizeof(B.status), "EXISTS: USE -f TO REPLACE");
	} else if (save_crop(dest) == 0) {
		reload_saved(dest);
		return;
	} else {
		snprintf(B.status, sizeof(B.status), "SAVE FAILED - SEE STDERR");
	}
	mark_redraw();
}

// ---------- drawing ----------

static void draw_status(void)
{
	float s = (float)B.scale;
	int bw = (B.w > 0 ? B.w : 1) * B.scale;
	float pad = 8.f * s;
	float tw = (float)strlen(B.status) * 6.f * s;
	float bx = fmin(tw + 2.f * pad, (float)bw);
	float bh = 7.f * s + 2.f * pad;
	quad(0.f, 0.f, bx, bh, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.86f);
	draw_text(B.status, pad, pad, s, 1.5f);
}

static void draw_help(void)
{
	static const char *lines[] = {
	    "1 1:1   4 4:3   5 FREE   6 16:9",
	    "O ORIGINAL   R RESET   0 FIT   ? HELP",
	    "+/- ZOOM   ENTER SAVE   F FULLSCREEN",
	    "LEFT CROP/MOVE/RESIZE   RIGHT PAN   C/ESC EXIT",
	};
	float s = (float)B.scale;
	int bw = (B.w > 0 ? B.w : 1) * (int)s;
	int bh = (B.h > 0 ? B.h : 1) * (int)s;
	float pad = 8.f * s;
	float line_height = 10.f * s;
	float box_w = 0.f;
	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		float lw = (float)strlen(lines[i]) * 6.f * s;
		if (lw > box_w)
			box_w = lw;
	}
	float box_h = (float)(sizeof(lines) / sizeof(lines[0])) * line_height + 2.f * pad;
	box_w += 2.f * pad;
	if (box_w > bw - 2.f * pad)
		box_w = bw - 2.f * pad;
	float top = bh - box_h - pad;
	quad(pad, top, pad + box_w, top + box_h, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
	     0.86f);
	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
		draw_text(lines[i], 2.f * pad, top + (float)i * line_height + pad / 2.f, s,
			  1.5f);
}

static void draw(void)
{
	int bw = (B.w > 0 ? B.w : 1) * B.scale;
	int bh = (B.h > 0 ? B.h : 1) * B.scale;
	eglMakeCurrent(B.edpy, B.esurf, B.esurf, B.ectx);
	glViewport(0, 0, bw, bh);
	glClearColor(0.09f, 0.09f, 0.10f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	if (!B.have_img)
		goto swap;
	float s = (float)B.scale;
	float z = (float)(B.zoom * s);
	float ox = (float)(B.px * s);
	float oy = (float)(B.py * s);
	float iw = (float)B.iw * z;
	float ih = (float)B.ih * z;
	glUseProgram(B.prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, B.tex);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, B.wtex);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, B.ftex);
	glUniform1i(B.u_tex, 0);
	glUniform1i(B.u_wtex, 1);
	glUniform1i(B.u_ftex, 2);
	glUniform2f(B.u_win, (float)bw, (float)bh);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	quad(ox, oy, ox + iw, oy + ih, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f);
	if (B.crop_mode) {
		// crop rect is in original image px; the displayed image may
		// be halved, so map through the display ratio
		double r = (double)B.iw / B.img.width;
		float cx = (float)(B.px + B.crop.x * r * B.zoom) * s;
		float cy = (float)(B.py + B.crop.y * r * B.zoom) * s;
		float cw = (float)(B.crop.w * r * B.zoom) * s;
		float ch = (float)(B.crop.h * r * B.zoom) * s;
		float dk = 105.f / 255.f; // darken factor
		// darken the image outside the crop (4 quads, clamped to the image)
		quad(ox, oy, ox + iw, cy, 0.f, 0.f, 1.f, (cy - oy) / ih, dk, dk, dk,
		     0.6f);
		quad(ox, cy, cx, cy + ch, 0.f, (cy - oy) / ih, (cx - ox) / iw,
		     (cy + ch - oy) / ih, dk, dk, dk, 0.6f);
		quad(cx + cw, cy, ox + iw, cy + ch, (cx + cw - ox) / iw,
		     (cy - oy) / ih, 1.f, (cy + ch - oy) / ih, dk, dk, dk, 0.6f);
		quad(ox, cy + ch, ox + iw, oy + ih, 0.f, (cy + ch - oy) / ih, 1.f, 1.f,
		     dk, dk, dk, 0.6f);
		// white border + 8 handles
		float ln = s;
		quad(cx, cy, cx + cw, cy + ln, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f);
		quad(cx, cy + ch - ln, cx + cw, cy + ch, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f,
		     1.f, 1.f);
		quad(cx, cy, cx + ln, cy + ch, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f);
		quad(cx + cw - ln, cy, cx + cw, cy + ch, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f,
		     1.f, 1.f);
		float hd = 7.f * s;
		float pts[8][2] = {
		    {cx, cy},     {cx + cw / 2.f, cy}, {cx + cw, cy},
		    {cx, cy + ch / 2.f}, {cx + cw, cy + ch / 2.f},
		    {cx, cy + ch}, {cx + cw / 2.f, cy + ch},
		    {cx + cw, cy + ch},
		};
		for (int i = 0; i < 8; i++)
			quad(pts[i][0] - hd / 2.f, pts[i][1] - hd / 2.f,
			     pts[i][0] + hd / 2.f, pts[i][1] + hd / 2.f, 0.f, 0.f, 0.f,
			     0.f, 1.f, 1.f, 1.f, 1.f);
		if (B.show_help)
			draw_help();
		if (B.status[0])
			draw_status();
	}
swap:
	eglSwapBuffers(B.edpy, B.esurf);
}

// ---------- Wayland: events ----------

static void registry_on_global(void *data, struct wl_registry *reg, uint32_t name,
				const char *iface, uint32_t ver)
{
	struct bimg *b = data;
	(void)ver;
	if (!strcmp(iface, "wl_compositor"))
		b->compositor = (struct wl_compositor *)wl_registry_bind(reg, name,
									   &wl_compositor_interface, 4);
	else if (!strcmp(iface, "xdg_wm_base"))
		b->wm_base = (struct xdg_wm_base *)wl_registry_bind(reg, name,
								    &xdg_wm_base_interface,
								    4);
	else if (!strcmp(iface, "wl_seat"))
		b->seat = (struct wl_seat *)wl_registry_bind(reg, name,
							     &wl_seat_interface, 5);
	else if (!strcmp(iface, "wl_output"))
		b->out = (struct wl_output *)wl_registry_bind(reg, name,
							     &wl_output_interface, 3);
}

static void wm_base_on_ping(void *data, struct xdg_wm_base *wb, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wb, serial);
}

static void xdg_on_configure(void *data, struct xdg_surface *x, uint32_t serial)
{
	struct bimg *b = data;
	xdg_surface_ack_configure(x, serial);
	b->configured = true;
	mark_redraw();
}

static void top_on_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h,
			     struct wl_array *states)
{
	struct bimg *b = data;
	(void)top;
	(void)states;
	if (w > 0 && h > 0 && (w != b->w || h != b->h)) {
		b->w = w;
		b->h = h;
		wl_egl_window_resize(b->ewin, w * b->scale, h * b->scale, 0, 0);
		if (b->fit)
			refit();
		mark_redraw();
	}
}

static void top_on_close(void *data, struct xdg_toplevel *top)
{
	(void)data;
	(void)top;
	B.running = false;
	B.exit_status = 0;
}

static void kb_on_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
			 int32_t fd, uint32_t size)
{
	(void)data;
	(void)kb;
	(void)format;
	(void)size;
	close(fd);
}

static void kb_on_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
			struct wl_surface *surface, struct wl_array *keys)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surface;
	(void)keys;
}

static void kb_on_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
			struct wl_surface *surface)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surface;
}

static void kb_on_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
		      uint32_t key, uint32_t state)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;
	handle_key(key);
}

static void kb_on_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
			     uint32_t mods_depressed, uint32_t mods_latched,
			     uint32_t mods_locked, uint32_t group)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)mods_depressed;
	(void)mods_latched;
	(void)mods_locked;
	(void)group;
}

static void kb_on_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
			       int32_t delay)
{
	(void)data;
	(void)kb;
	(void)rate;
	(void)delay;
}

static void ptr_on_enter(void *data, struct wl_pointer *p, uint32_t serial,
			 struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	struct bimg *b = data;
	(void)p;
	(void)serial;
	(void)s;
	b->mx = wl_fixed_to_double(x);
	b->my = wl_fixed_to_double(y);
}

static void ptr_on_leave(void *data, struct wl_pointer *p, uint32_t serial,
			 struct wl_surface *s)
{
	struct bimg *b = data;
	(void)p;
	(void)serial;
	(void)s;
	b->drag = DRAG_NONE;
	b->lbtn = 0;
}

static void ptr_on_motion(void *data, struct wl_pointer *p, uint32_t time,
			   wl_fixed_t x, wl_fixed_t y)
{
	struct bimg *b = data;
	(void)p;
	(void)time;
	double nx = wl_fixed_to_double(x), ny = wl_fixed_to_double(y);
	if (b->crop_mode) {
		if (b->drag == DRAG_PAN) {
			b->px += nx - b->mx;
			b->py += ny - b->my;
			b->fit = false;
			mark_redraw();
		} else if (b->drag != DRAG_NONE) {
			b->mx = nx;
			b->my = ny;
			update_drag(nx, ny);
		}
	} else if (b->lbtn) {
		b->px += nx - b->mx;
		b->py += ny - b->my;
		b->fit = false;
		mark_redraw();
	}
	b->mx = nx;
	b->my = ny;
}

static void ptr_on_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
			 uint32_t button, uint32_t state)
{
	struct bimg *b = data;
	(void)p;
	(void)serial;
	(void)time;
	if (button == 273) { // BTN_LEFT
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			if (b->crop_mode) {
				b->drag = hit_test(b->mx, b->my);
				b->drag_crop = b->crop;
				b->drag_x = b->mx;
				b->drag_y = b->my;
				if (b->drag == DRAG_NEW) {
					screen_to_image(b->mx, b->my, &b->drag_anchor_x,
							 &b->drag_anchor_y);
					b->drag_anchor_x =
					    clampd(b->drag_anchor_x, 0.0, b->img.width);
					b->drag_anchor_y =
					    clampd(b->drag_anchor_y, 0.0, b->img.height);
				}
			} else {
				b->lbtn = 1;
			}
		} else {
			if (b->crop_mode) {
				if (b->drag != DRAG_PAN)
					b->drag = DRAG_NONE;
			} else {
				b->lbtn = 0;
			}
		}
	} else if (button == 274 && b->crop_mode) { // BTN_RIGHT
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			b->drag = DRAG_PAN;
			b->drag_x = b->mx;
			b->drag_y = b->my;
		} else if (b->drag == DRAG_PAN) {
			b->drag = DRAG_NONE;
		}
	}
}

static void ptr_on_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
			wl_fixed_t value)
{
	struct bimg *b = data;
	(void)p;
	(void)time;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && b->have_img)
		zoom_at(b->mx, b->my, pow(1.25, -wl_fixed_to_double(value) / 120.0));
}

// every pointer event slot must be non-NULL or libwayland aborts on dispatch
static void ptr_noop_frame(void *data, struct wl_pointer *p)
{
	(void)data;
	(void)p;
}

static void ptr_noop_axis_source(void *data, struct wl_pointer *p, uint32_t s)
{
	(void)data;
	(void)p;
	(void)s;
}

static void ptr_noop_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
	(void)data;
	(void)p;
	(void)time;
	(void)axis;
}

static void ptr_noop_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
				    int32_t discrete)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)discrete;
}

static void ptr_noop_axis_value120(void *data, struct wl_pointer *p, uint32_t axis,
				    int32_t value120)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)value120;
}

static void ptr_noop_axis_rel_dir(void *data, struct wl_pointer *p, uint32_t axis,
				   uint32_t direction)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)direction;
}

static void ptr_noop_warp(void *data, struct wl_pointer *p, wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)p;
	(void)x;
	(void)y;
}

static void out_on_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
			    int32_t pw, int32_t ph, int32_t subpixel,
			    const char *make, const char *model, int32_t transform)
{
	struct bimg *b = data;
	(void)o;
	(void)x;
	(void)y;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	if (pw > 0 && ph > 0) {
		b->geo_w = pw;
		b->geo_h = ph;
		b->have_geo = true;
	}
}

static void out_on_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
			int32_t h, int32_t refresh)
{
	(void)data;
	(void)o;
	(void)flags;
	(void)w;
	(void)h;
	(void)refresh;
}

static void out_on_done(void *data, struct wl_output *o)
{
	(void)data;
	(void)o;
}

static void out_on_name(void *data, struct wl_output *o, const char *name)
{
	(void)data;
	(void)o;
	(void)name;
}

static void out_on_scale(void *data, struct wl_output *o, int32_t factor)
{
	struct bimg *b = data;
	(void)o;
	if (factor > 0 && factor != b->scale) {
		b->scale = factor;
		wl_surface_set_buffer_scale(b->surface, b->scale);
		mark_redraw();
	}
}

static const struct wl_registry_listener reg_l = { .global = registry_on_global };
static const struct xdg_wm_base_listener wmb_l = { .ping = wm_base_on_ping };
static const struct xdg_surface_listener xdg_l = { .configure = xdg_on_configure };
static const struct xdg_toplevel_listener top_l = {
    .configure = top_on_configure,
    .close = top_on_close,
};
static const struct wl_keyboard_listener kb_l = {
    .keymap = kb_on_keymap,
    .enter = kb_on_enter,
    .leave = kb_on_leave,
    .key = kb_on_key,
    .modifiers = kb_on_modifiers,
    .repeat_info = kb_on_repeat_info,
};
static const struct wl_pointer_listener ptr_l = {
    .enter = ptr_on_enter,
    .leave = ptr_on_leave,
    .motion = ptr_on_motion,
    .button = ptr_on_button,
    .axis = ptr_on_axis,
    .frame = ptr_noop_frame,
    .axis_source = ptr_noop_axis_source,
    .axis_stop = ptr_noop_axis_stop,
    .axis_discrete = ptr_noop_axis_discrete,
    .axis_value120 = ptr_noop_axis_value120,
    .axis_relative_direction = ptr_noop_axis_rel_dir,
    .warp = ptr_noop_warp,
};
static const struct wl_output_listener out_l = {
    .geometry = out_on_geometry,
    .mode = out_on_mode,
    .done = out_on_done,
    .scale = out_on_scale,
    .name = out_on_name,
};

// ---------- keys ----------

static void toggle_fullscreen(void)
{
	B.fullscreen = !B.fullscreen;
	if (B.top) {
		if (B.fullscreen)
			xdg_toplevel_set_fullscreen(B.top, B.out);
		else
			xdg_toplevel_unset_fullscreen(B.top);
	}
	mark_redraw();
}

static void handle_key(uint32_t key)
{
	if (B.crop_mode) {
		switch (key) {
		case KEY_ESC:
		case KEY_C:
			B.crop_mode = false;
			B.show_help = false;
			B.status[0] = '\0';
			mark_redraw();
			break;
		case KEY_1:
			apply_aspect(1.0);
			break;
		case KEY_4:
			apply_aspect(4.0 / 3.0);
			break;
		case KEY_5:
			apply_aspect(0.0);
			break;
		case KEY_6:
			apply_aspect(16.0 / 9.0);
			break;
		case KEY_O:
			apply_aspect((double)B.img.width / B.img.height);
			break;
		case KEY_0:
			refit();
			break;
		case KEY_R:
			B.crop = (struct rect){0, 0, (double)B.img.width,
						 (double)B.img.height};
			if (B.aspect > 0.0)
				apply_aspect(B.aspect);
			else
				mark_redraw();
			break;
		case KEY_SLASH: // ? (shift+/)
			B.show_help = !B.show_help;
			mark_redraw();
			break;
		case KEY_F:
			toggle_fullscreen();
			break;
		case KEY_ENTER:
			do_save();
			break;
		default:
			break;
		}
		return;
	}
	switch (key) {
	case KEY_Q:
	case KEY_ESC:
		B.running = false;
		B.exit_status = 0;
		return;
	}
	if (!B.have_img)
		return;
	switch (key) {
	case KEY_LEFT:
		next_image(-1);
		break;
	case KEY_RIGHT:
	case KEY_T:
		next_image(1);
		break;
	case KEY_S:
		next_image(-1);
		break;
	case KEY_EQUAL: // +
		zoom_at(B.w / 2.0, B.h / 2.0, 1.25);
		break;
	case KEY_MINUS:
		zoom_at(B.w / 2.0, B.h / 2.0, 1.0 / 1.25);
		break;
	case KEY_1:
		one_to_one();
		break;
	case KEY_0:
		refit();
		break;
	case KEY_F:
		toggle_fullscreen();
		break;
	case KEY_C:
		enter_crop();
		break;
	default:
		break;
	}
}

// ---------- animation ----------

static int arm_animation(void)
{
	int duration = image_current_duration(&B.img);
	struct itimerspec timer = {0};
	if (duration < 10)
		duration = 10;
	timer.it_value.tv_sec = duration / 1000;
	timer.it_value.tv_nsec = (long)(duration % 1000) * 1000000L;
	return timerfd_settime(B.anim_fd, 0, &timer, NULL);
}

static void sync_animation(void)
{
	struct itimerspec off = {0};
	if (B.anim_fd < 0)
		return;
	if (B.have_img && image_animated(&B.img)) {
		if (arm_animation() < 0)
			fprintf(stderr, "bimg: cannot arm animation timer: %s\n",
				strerror(errno));
	} else {
		timerfd_settime(B.anim_fd, 0, &off, NULL);
	}
}

static int dispatch_events(void)
{
	struct pollfd descriptors[2] = {
	    {wl_display_get_fd(B.dpy), POLLIN, 0},
	    {B.anim_fd, POLLIN, 0},
	};
	nfds_t count = B.anim_fd >= 0 ? 2 : 1;

	while (wl_display_prepare_read(B.dpy) != 0) {
		if (wl_display_dispatch_pending(B.dpy) < 0)
			return -1;
	}
	if (wl_display_flush(B.dpy) < 0) {
		if (errno == EAGAIN)
			descriptors[0].events |= POLLOUT;
		else {
			wl_display_cancel_read(B.dpy);
			return -1;
		}
	}
	int ready = poll(descriptors, count, -1);
	if (ready < 0) {
		wl_display_cancel_read(B.dpy);
		return errno == EINTR ? 0 : -1;
	}
	if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
		wl_display_cancel_read(B.dpy);
		errno = EPIPE;
		return -1;
	}
	if (descriptors[0].revents & POLLOUT && wl_display_flush(B.dpy) < 0 &&
	    errno != EAGAIN) {
		wl_display_cancel_read(B.dpy);
		return -1;
	}
	if (descriptors[0].revents & POLLIN) {
		if (wl_display_read_events(B.dpy) < 0)
			return -1;
	} else {
		wl_display_cancel_read(B.dpy);
	}
	if (wl_display_dispatch_pending(B.dpy) < 0)
		return -1;
	if (count == 2 && descriptors[1].revents & POLLIN) {
		uint64_t expirations;
		if (read(B.anim_fd, &expirations, sizeof(expirations)) ==
		    (ssize_t)sizeof(expirations)) {
			(void)expirations;
			image_advance(&B.img);
			build_texture();
			if (arm_animation() < 0)
				return -1;
			mark_redraw();
		}
	}
	if (count == 2 && descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
		errno = EIO;
		return -1;
	}
	return 0;
}

// ---------- main ----------

static void egl_init(void)
{
	wl_display_roundtrip(B.dpy);
	B.edpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, (void *)B.dpy, NULL);
	if (B.edpy == EGL_NO_DISPLAY)
		die("eglGetPlatformDisplay: 0x%x", eglGetError());
	EGLint maj, min;
	if (!eglInitialize(B.edpy, &maj, &min))
		die("eglInitialize: 0x%x", eglGetError());
	static const EGLint cfg[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_BUFFER_SIZE, 32,
		EGL_NONE,
	};
	EGLConfig ec;
	EGLint ncfg;
	if (!eglChooseConfig(B.edpy, cfg, &ec, 1, &ncfg))
		die("eglChooseConfig: 0x%x", eglGetError());
	B.ecfg = ec;
	static const EGLint ctx_cfg[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	B.ectx = eglCreateContext(B.edpy, ec, NULL, ctx_cfg);
	if (B.ectx == EGL_NO_CONTEXT)
		die("eglCreateContext: 0x%x", eglGetError());
	// surface comes later: the window must be created at the target size,
	// because Mesa ignores wl_egl_window_resize before the first present
	if (!eglMakeCurrent(B.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, B.ectx))
		die("eglMakeCurrent: 0x%x", eglGetError());
}

static void usage(FILE *stream)
{
	fprintf(stream, "usage: bimg [-f] [-o OUTPUT] [-l|--jpeg-lossless] "
		    "<file-or-dir>...\n");
}

int main(int argc, char **argv)
{
	int option;
	static const struct option long_options[] = {
		{"jpeg-lossless", no_argument, NULL, 'l'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	B.scale = 1;
	B.cur = -1;
	B.running = true;
	B.anim_fd = -1;
	B.exit_status = 1;

	while ((option = getopt_long(argc, argv, "flo:h", long_options, NULL)) != -1) {
		switch (option) {
		case 'f':
			B.force = true;
			break;
		case 'o':
			B.out_path = optarg;
			break;
		case 'l':
			B.lossless = true;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	for (int i = optind; i < argc; i++) {
		struct stat st;
		if (stat(argv[i], &st) != 0) {
			fprintf(stderr, "bimg: %s: %s\n", argv[i], strerror(errno));
			continue;
		}
		if (S_ISDIR(st.st_mode))
			add_dir(argv[i]);
		else
			files_push(argv[i]);
	}
	if (B.nfiles == 0) {
		add_dir(".");
	}
	if (B.nfiles == 0) {
		usage(stderr);
		return 1;
	}

	B.dpy = wl_display_connect(NULL);
	if (!B.dpy)
		die("cannot connect to Wayland display");
	B.reg = wl_display_get_registry(B.dpy);
	wl_registry_add_listener(B.reg, &reg_l, &B);
	wl_display_flush(B.dpy);
	wl_display_roundtrip(B.dpy);
	if (!B.compositor || !B.wm_base)
		die("compositor is missing wl_compositor or xdg_wm_base");

	B.surface = wl_compositor_create_surface(B.compositor);
	B.xdg = xdg_wm_base_get_xdg_surface(B.wm_base, B.surface);
	B.top = xdg_surface_get_toplevel(B.xdg);
	xdg_wm_base_add_listener(B.wm_base, &wmb_l, &B);
	xdg_surface_add_listener(B.xdg, &xdg_l, &B);
	xdg_toplevel_add_listener(B.top, &top_l, &B);
	xdg_toplevel_set_title(B.top, "bimg");
	xdg_toplevel_set_app_id(B.top, "bimg");
	if (B.seat) {
		B.kb = wl_seat_get_keyboard(B.seat);
		B.ptr = wl_seat_get_pointer(B.seat);
	}
	if (B.kb)
		wl_keyboard_add_listener(B.kb, &kb_l, &B);
	if (B.ptr)
		wl_pointer_add_listener(B.ptr, &ptr_l, &B);
	if (B.out)
		wl_output_add_listener(B.out, &out_l, &B);

	egl_init();
	gl_init();
	if (!next_image(1))
		die("no loadable images");

	// create the window at the image size: the first eglSwapBuffers maps
	// exactly this size (Mesa ignores pre-present resizes); the fullscreen
	// configure then resizes it properly from frame two. The wl_output
	// geometry is not reliable for this.
	int32_t cw = B.iw, ch = B.ih;
	if (cw < 1)
		cw = 1;
	if (ch < 1)
		ch = 1;
	B.w = cw;
	B.h = ch;
	B.ewin = wl_egl_window_create(B.surface, B.w * B.scale, B.h * B.scale);
	B.esurf = eglCreatePlatformWindowSurface(B.edpy, B.ecfg, B.ewin, NULL);
	if (B.esurf == EGL_NO_SURFACE)
		die("eglCreatePlatformWindowSurface: 0x%x", eglGetError());
	if (!eglMakeCurrent(B.edpy, B.esurf, B.esurf, B.ectx))
		die("eglMakeCurrent: 0x%x", eglGetError());
	refit();
	if (!getenv("BIMG_TILED")) {
		B.fullscreen = true;
		xdg_toplevel_set_fullscreen(B.top, B.out);
	}
	// empty commit: makes the compositor send the initial configure
	wl_surface_commit(B.surface);

	B.anim_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (B.anim_fd < 0) {
		fprintf(stderr, "bimg: cannot create animation timer: %s\n",
			strerror(errno));
		B.anim_fd = -1;
	}
	sync_animation();

	while (B.running) {
		if (dispatch_events() < 0) {
			fprintf(stderr, "bimg: Wayland connection failed: %s\n",
				strerror(errno));
			B.exit_status = 1;
			break;
		}
		// present only after the first configure: swapping an unconfigured
		// toplevel makes wlroots kill the client ("never been configured")
		if (B.redraw && B.configured) {
			draw();
			B.redraw = false;
		}
	}

	free_display_image();
	eglMakeCurrent(B.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(B.edpy, B.ectx);
	eglDestroySurface(B.edpy, B.esurf);
	eglTerminate(B.edpy);
	if (B.anim_fd >= 0)
		close(B.anim_fd);
	xdg_toplevel_destroy(B.top);
	xdg_surface_destroy(B.xdg);
	if (B.kb)
		wl_keyboard_destroy(B.kb);
	if (B.ptr)
		wl_pointer_destroy(B.ptr);
	if (B.out)
		wl_output_destroy(B.out);
	if (B.ewin)
		wl_egl_window_destroy(B.ewin);
	wl_surface_destroy(B.surface);
	if (B.wm_base)
		xdg_wm_base_destroy(B.wm_base);
	if (B.seat)
		wl_seat_destroy(B.seat);
	if (B.compositor)
		wl_compositor_destroy(B.compositor);
	if (B.reg)
		wl_registry_destroy(B.reg);
	wl_display_flush(B.dpy);
	wl_display_disconnect(B.dpy);
	if (B.have_img)
		image_free(&B.img);
	for (int i = 0; i < B.nfiles; i++)
		free(B.files[i]);
	free(B.files);
	return B.exit_status;
}
