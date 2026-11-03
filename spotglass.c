/*
 * spotglass — a tiny glass Spotify player for wlroots compositors (mango).
 *
 * One process does everything:
 *   - `spotglass` starts it in front of windows; run it again to toggle front/back
 *     (bind it to a key). It then follows any MPRIS player (Spotify, browsers, ...)
 *   - `spotglass --spotify` follows only Spotify and exits when Spotify closes
 *   - layer-shell surface: TOP layer in front, BOTTOM layer (above wallpaper) in back
 *   - prev / play-pause / next buttons, title + artist
 *   - bar visualizer from a PipeWire tap of the default output (only while playing)
 * The glass background is the wallpaper (from `awww/swww query`), blurred in-process.
 *
 * Tweak the CONFIG block, then `make install`.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* ───────────────────────────── CONFIG ───────────────────────────── */
#define W        360                 /* widget size (px)                    */
#define H        112
#define MARGIN_X 24                  /* distance from screen edge           */
#define MARGIN_Y 24
#define ANCHOR   (ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
#define RADIUS   18.0                /* card corner radius                  */
static const double COL[3] = { 0xe8 / 255.0, 0xde / 255.0, 0xd6 / 255.0 }; /* the one colour */
#define BLUR     1                   /* wallpaper blur strength (0 = none)    */
#define TINT     0.18                /* dark glass tint opacity (text contrast) */
#define FONT_T   "JetBrainsMono Nerd Font Propo Bold 10.5"
#define FONT_A   "JetBrainsMono Nerd Font Propo 9"
#define NBARS    40
#define FPS      30
#define SPOTIFY  "org.mpris.MediaPlayer2.spotify"
#define SPOTIFY_WAIT 60              /* --spotify: give up if Spotify never shows (s) */
/* ─────────────────────────────────────────────────────────────────── */

#define FFT_N 2048
#define RATE  48000
#define PAD   16

static GMainLoop *loop;

/* ---------- wayland ---------- */
static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *ptr;
static struct zwlr_layer_shell_v1 *lsh;
static struct wl_surface *surf;
static struct zwlr_layer_surface_v1 *lsurf;
static struct wl_callback *frame_cb;
static int configured, need_draw;

static struct wl_cursor_theme *ctheme;
static struct wl_surface *csurf;
static uint32_t enter_serial;
static const char *cur_name;

struct buf { struct wl_buffer *wb; cairo_surface_t *cs; int busy; };
static struct buf bufs[2];

/* ---------- player state ---------- */
static guint anim_id;
static int front;                    /* 1 = TOP layer (above windows), 0 = BOTTOM */
static int spotify_only;             /* started with --spotify */
static GDBusConnection *bus;
static char title[512], artist[512];
static int playing;
static double px = -1, py = -1;
static int hover = -1;               /* 0 prev, 1 play, 2 next */

/* ---------- audio ---------- */
static struct pw_thread_loop *pwl;
static struct pw_stream *pws;
static float ring[FFT_N];
static unsigned ring_pos;
static float bars[NBARS];

/* button centres */
static const double BY = 36;
static const double BX[3] = { W - PAD - 14 - 72, W - PAD - 14 - 36, W - PAD - 14 };

static void request_draw(void);

/* ═══════════════════════════ audio / FFT ═══════════════════════════ */
static void on_process(void *data)
{
	(void)data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(pws);
	if (!b) return;
	struct spa_data *d = &b->buffer->datas[0];
	if (d->data) {
		float *s = SPA_PTROFF(d->data, d->chunk->offset, float);
		unsigned n = d->chunk->size / (2 * sizeof(float));
		for (unsigned i = 0; i < n; i++)
			ring[ring_pos++ & (FFT_N - 1)] = 0.5f * (s[2 * i] + s[2 * i + 1]);
	}
	pw_stream_queue_buffer(pws, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS, .process = on_process,
};

static void audio_start(void)
{
	if (pws) return;
	pw_thread_loop_lock(pwl);
	struct pw_properties *p = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Music", PW_KEY_STREAM_CAPTURE_SINK, "true",
		PW_KEY_NODE_NAME, "spotglass", PW_KEY_NODE_LATENCY, "1024/48000", NULL);
	pws = pw_stream_new_simple(pw_thread_loop_get_loop(pwl), "spotglass", p, &stream_events, NULL);
	uint8_t tmp[1024];
	struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(tmp, sizeof(tmp));
	const struct spa_pod *params[1] = { spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .channels = 2, .rate = RATE,
					 .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR })) };
	pw_stream_connect(pws, PW_DIRECTION_INPUT, PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
	pw_thread_loop_unlock(pwl);
}

static void audio_stop(void)
{
	if (!pws) return;
	pw_thread_loop_lock(pwl);
	pw_stream_destroy(pws);
	pws = NULL;
	memset(ring, 0, sizeof ring);
	pw_thread_loop_unlock(pwl);
}

static void fft(float *re, float *im, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
	}
	for (int len = 2; len <= n; len <<= 1) {
		double a = -2 * M_PI / len;
		float wr = cos(a), wi = sin(a);
		for (int i = 0; i < n; i += len) {
			float cr = 1, ci = 0;
			for (int k = 0; k < len / 2; k++) {
				int u = i + k, v = u + len / 2;
				float xr = re[v] * cr - im[v] * ci, xi = re[v] * ci + im[v] * cr;
				re[v] = re[u] - xr; im[v] = im[u] - xi;
				re[u] += xr; im[u] += xi;
				float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
			}
		}
	}
}

static void analyze(void)
{
	static float re[FFT_N], im[FFT_N], win[FFT_N];
	static int k0[NBARS + 1];
	static float tilt[NBARS];
	if (!win[1]) {
		for (int i = 0; i < FFT_N; i++) win[i] = 0.5f - 0.5f * cosf(2 * M_PI * i / (FFT_N - 1));
		for (int i = 0; i <= NBARS; i++) {
			double f = 45.0 * pow(14000.0 / 45.0, (double)i / NBARS);
			k0[i] = (int)(f * FFT_N / RATE);
		}
		for (int i = 0; i < NBARS; i++) {
			double fc = 45.0 * pow(14000.0 / 45.0, (i + 0.5) / NBARS);
			tilt[i] = 3.0 * log2(fc / 400.0);          /* +3 dB/oct: music is bass-heavy */
		}
	}
	pw_thread_loop_lock(pwl);
	for (int i = 0; i < FFT_N; i++) re[i] = ring[(ring_pos + i) & (FFT_N - 1)] * win[i];
	pw_thread_loop_unlock(pwl);
	memset(im, 0, sizeof im);
	fft(re, im, FFT_N);

	float raw[NBARS];
	for (int b = 0; b < NBARS; b++) {
		int lo = k0[b], hi = k0[b + 1] > lo ? k0[b + 1] : lo + 1;
		float m = 0;
		for (int k = lo; k < hi && k < FFT_N / 2; k++) {
			float v = re[k] * re[k] + im[k] * im[k];
			if (v > m) m = v;
		}
		float db = 10.0f * log10f(m * 16.0f / ((float)FFT_N * FFT_N) + 1e-12f) + tilt[b];
		float v = (db + 62.0f) / 56.0f;              /* -62 dB .. -6 dB  →  0 .. 1 */
		raw[b] = v < 0 ? 0 : v > 1 ? 1 : v;
	}
	for (int b = 0; b < NBARS; b++) {
		float l = raw[b > 0 ? b - 1 : b], r = raw[b < NBARS - 1 ? b + 1 : b];
		float v = 0.25f * l + 0.5f * raw[b] + 0.25f * r;
		bars[b] = v > bars[b] ? bars[b] * 0.4f + v * 0.6f : fmaxf(v, bars[b] - 0.035f);
	}
}

/* ═════════════════════════════ drawing ═════════════════════════════ */
static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static void col(cairo_t *cr, double a) { cairo_set_source_rgba(cr, COL[0], COL[1], COL[2], a); }

static void tri(cairo_t *cr, double x, double y, double s, int dir)
{
	cairo_move_to(cr, x - dir * s * 0.45, y - s * 0.55);
	cairo_line_to(cr, x + dir * s * 0.55, y);
	cairo_line_to(cr, x - dir * s * 0.45, y + s * 0.55);
	cairo_close_path(cr);
}

static void text(cairo_t *cr, const char *s, int i, double y, double a)
{
	static PangoFontDescription *fd[2];
	if (!fd[i]) fd[i] = pango_font_description_from_string(i ? FONT_A : FONT_T);
	PangoLayout *l = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(l, fd[i]);
	pango_layout_set_width(l, (int)(BX[0] - 24 - PAD) * PANGO_SCALE);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	pango_layout_set_text(l, s, -1);
	col(cr, a);
	cairo_move_to(cr, PAD + 2, y);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
}

/* Mango never blurs BOTTOM-layer surfaces, and the only thing behind this one is the
 * wallpaper — so blur the wallpaper crop ourselves, once per show(). */
static cairo_surface_t *bg;

static void box_blur(unsigned char *px, int w, int h, int stride, int r)
{
	unsigned char *tmp = g_malloc(stride * h);
	for (int pass = 0; pass < 3; pass++)
		for (int dir = 0; dir < 2; dir++) {
			int n = dir ? h : w, m = dir ? w : h;
			for (int j = 0; j < m; j++)
				for (int i = 0; i < n; i++) {
					int sum[4] = { 0 }, cnt = 0;
					for (int k = -r; k <= r; k++) {
						int q = CLAMP(i + k, 0, n - 1);
						unsigned char *p = dir ? px + q * stride + j * 4 : px + j * stride + q * 4;
						for (int c = 0; c < 4; c++) sum[c] += p[c];
						cnt++;
					}
					unsigned char *o = dir ? tmp + i * stride + j * 4 : tmp + j * stride + i * 4;
					for (int c = 0; c < 4; c++) o[c] = sum[c] / cnt;
				}
			memcpy(px, tmp, stride * h);
		}
	g_free(tmp);
}

static void load_bg(void)
{
	g_clear_pointer(&bg, cairo_surface_destroy);
	char *out = NULL, *path = NULL;
	int ow = 0, oh = 0;
	if (BLUR <= 0 || (!g_spawn_command_line_sync("awww query", &out, NULL, NULL, NULL) &&
			  !g_spawn_command_line_sync("swww query", &out, NULL, NULL, NULL)))
		return;
	char *line = out ? strtok(out, "\n") : NULL;      /* first output only */
	/* e.g. ": eDP-1: 1920x1080, scale: 1, currently displaying: image: /path.png" */
	char *img = line ? strstr(line, "image: ") : NULL;
	for (char *p = line; p && (p = strstr(p, ": ")); p += 2)
		if (sscanf(p + 2, "%dx%d", &ow, &oh) == 2) break;
	if (img && ow > 0 && oh > 0) path = g_strdup(img + 7);
	g_free(out);
	int iw, ih;
	if (!path || !gdk_pixbuf_get_file_info(path, &iw, &ih)) { g_free(path); return; }
	double sc = fmax((double)ow / iw, (double)oh / ih);  /* awww default: crop to fill, centred */
	GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, ceil(iw * sc), ceil(ih * sc), FALSE, NULL);
	g_free(path);
	if (!pb) return;
	int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb);
	int x = (ANCHOR & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) ? ow - MARGIN_X - W :
		(ANCHOR & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) ? MARGIN_X : (ow - W) / 2;
	int y = (ANCHOR & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) ? oh - MARGIN_Y - H :
		(ANCHOR & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) ? MARGIN_Y : (oh - H) / 2;
	x += (pw - ow) / 2; y += (ph - oh) / 2;

	/* crop into a 1/4-size surface, blur that (cheap), then upscale smoothly */
	const int f = 4, sw = W / f + 2, sh = H / f + 2;
	cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
	unsigned char *d = cairo_image_surface_get_data(small);
	int ds = cairo_image_surface_get_stride(small), ss = gdk_pixbuf_get_rowstride(pb);
	int nch = gdk_pixbuf_get_n_channels(pb);
	const guchar *src = gdk_pixbuf_read_pixels(pb);
	for (int j = 0; j < sh; j++)
		for (int i = 0; i < sw; i++) {           /* average f×f block → BGRA */
			int r = 0, g = 0, b = 0, n = 0;
			for (int v = 0; v < f; v++)
				for (int u = 0; u < f; u++) {
					int X = CLAMP(x + (i - 1) * f + u, 0, pw - 1), Y = CLAMP(y + (j - 1) * f + v, 0, ph - 1);
					const guchar *p = src + Y * ss + X * nch;
					r += p[0]; g += p[1]; b += p[2]; n++;
				}
			unsigned char *o = d + j * ds + i * 4;
			o[0] = b / n * 0.9; o[1] = g / n * 0.9; o[2] = r / n * 0.9; o[3] = 255;
		}
	g_object_unref(pb);
	box_blur(d, sw, sh, ds, BLUR);
	cairo_surface_mark_dirty(small);
	bg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
	cairo_t *cr = cairo_create(bg);
	cairo_scale(cr, f, f);
	cairo_set_source_surface(cr, small, -1, -1);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(small);
}

static void draw_card(cairo_t *cr)
{
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	/* glass: dark tint + faint mono sheen + hairline border */
	rrect(cr, 0.5, 0.5, W - 1, H - 1, RADIUS);
	if (bg) { cairo_set_source_surface(cr, bg, 0, 0); cairo_fill_preserve(cr); }
	cairo_set_source_rgba(cr, 0.06, 0.05, 0.05, bg ? TINT : TINT + 0.35);
	cairo_fill_preserve(cr);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, H);
	cairo_pattern_add_color_stop_rgba(g, 0, COL[0], COL[1], COL[2], 0.10);
	cairo_pattern_add_color_stop_rgba(g, 1, COL[0], COL[1], COL[2], 0.02);
	cairo_set_source(cr, g);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(g);
	col(cr, 0.22);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	/* title / artist */
	text(cr, title[0] ? title : "Nothing playing", 0, 15, 0.95);
	text(cr, artist[0] ? artist : (playing ? "" : title[0] ? "paused" : ""), 1, 35, 0.55);

	/* buttons */
	for (int i = 0; i < 3; i++) {
		double r = i == 1 ? 17 : 14, a = i == 1 ? 0.14 : 0.0;
		if (hover == i) a += 0.12;
		if (a > 0) { cairo_arc(cr, BX[i], BY, r, 0, 2 * M_PI); col(cr, a); cairo_fill(cr); }
	}
	col(cr, 0.95);
	for (int s = -1; s <= 1; s += 2) {           /* prev (s=-1) and next (s=1) */
		double x = BX[s < 0 ? 0 : 2];
		tri(cr, x - s * 2, BY, 11, s);
		cairo_fill(cr);
		rrect(cr, x + s * 5.5 - 1.1, BY - 6, 2.2, 12, 1.1);
		cairo_fill(cr);
	}
	if (playing) {
		rrect(cr, BX[1] - 5.5, BY - 6.5, 3.6, 13, 1.6); cairo_fill(cr);
		rrect(cr, BX[1] + 1.9, BY - 6.5, 3.6, 13, 1.6); cairo_fill(cr);
	} else {
		tri(cr, BX[1] + 1, BY, 13, 1);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_set_line_width(cr, 2);
		cairo_fill_preserve(cr);
		cairo_stroke(cr);
	}

}

/* static layer (card, text, buttons) is cached; only redrawn when state changes */
static cairo_surface_t *cache;
static int cache_dirty = 1;

static void draw_frame(cairo_t *cr)
{
	if (!cache) cache = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
	if (cache_dirty) {
		cairo_t *c = cairo_create(cache);
		draw_card(c);
		cairo_destroy(c);
		cache_dirty = 0;
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(cr, cache, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	/* visualizer */
	const double x0 = PAD + 2, x1 = W - PAD - 2, base = H - 14, maxh = 36, gap = 3;
	const double bw = (x1 - x0 - gap * (NBARS - 1)) / NBARS;
	for (int b = 0; b < NBARS; b++) {
		double v = bars[b], h = fmax(bw, v * maxh);
		rrect(cr, x0 + b * (bw + gap), base - h, bw, h, bw / 2);
		col(cr, 0.35 + 0.6 * v);
		cairo_fill(cr);
	}
}

/* ═════════════════════════════ wayland ═════════════════════════════ */
static void buf_release(void *data, struct wl_buffer *wb) { (void)wb; ((struct buf *)data)->busy = 0; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static int buffers_init(void)
{
	size_t stride = W * 4, sz = stride * H;
	int fd = memfd_create("spotglass", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, sz * 2) < 0) return -1;
	unsigned char *mem = mmap(NULL, sz * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) return -1;
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, sz * 2);
	for (int i = 0; i < 2; i++) {
		bufs[i].wb = wl_shm_pool_create_buffer(pool, sz * i, W, H, stride, WL_SHM_FORMAT_ARGB8888);
		wl_buffer_add_listener(bufs[i].wb, &buf_listener, &bufs[i]);
		bufs[i].cs = cairo_image_surface_create_for_data(mem + sz * i, CAIRO_FORMAT_ARGB32, W, H, stride);
	}
	wl_shm_pool_destroy(pool);
	close(fd);
	return 0;
}

static void draw(void);
static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
	(void)d; (void)t;
	wl_callback_destroy(cb);
	frame_cb = NULL;
	if (need_draw) draw();
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void draw(void)
{
	need_draw = 1;
	if (!configured || frame_cb) return;          /* hidden/occluded → no work */
	struct buf *b = !bufs[0].busy ? &bufs[0] : !bufs[1].busy ? &bufs[1] : NULL;
	if (!b) return;
	if (playing && pws) analyze();
	cairo_t *cr = cairo_create(b->cs);
	draw_frame(cr);
	cairo_destroy(cr);
	cairo_surface_flush(b->cs);
	frame_cb = wl_surface_frame(surf);
	wl_callback_add_listener(frame_cb, &frame_listener, NULL);
	wl_surface_attach(surf, b->wb, 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, W, H);
	wl_surface_commit(surf);
	b->busy = 1;
	need_draw = 0;
	wl_display_flush(dpy);
}

static gboolean tick(gpointer d)
{
	(void)d;
	if (!playing) {
		float m = 0;
		for (int i = 0; i < NBARS; i++) { bars[i] = fmaxf(0, bars[i] - 0.035f); m = fmaxf(m, bars[i]); }
		if (m == 0) { draw(); anim_id = 0; return G_SOURCE_REMOVE; }
	}
	draw();
	return G_SOURCE_CONTINUE;
}

static void request_draw(void)
{
	cache_dirty = 1;
	if (!surf) return;
	float m = 0;
	for (int i = 0; i < NBARS; i++) m = fmaxf(m, bars[i]);
	if (!anim_id && (playing || m > 0))
		anim_id = g_timeout_add(1000 / FPS, tick, NULL);
	if (!anim_id) draw();
}

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
	(void)d; (void)w; (void)h;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	configured = 1;
	draw();
}
static void hide(void);
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls) { (void)d; (void)ls; hide(); }
static const struct zwlr_layer_surface_v1_listener ls_listener = { ls_configure, ls_closed };

static void show(void)
{
	if (surf) return;
	load_bg();
	cache_dirty = 1;
	surf = wl_compositor_create_surface(comp);
	lsurf = zwlr_layer_shell_v1_get_layer_surface(lsh, surf, NULL,
		front ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "spotglass");
	zwlr_layer_surface_v1_set_size(lsurf, W, H);
	zwlr_layer_surface_v1_set_anchor(lsurf, ANCHOR);
	zwlr_layer_surface_v1_set_margin(lsurf, MARGIN_Y, MARGIN_X, MARGIN_Y, MARGIN_X);
	zwlr_layer_surface_v1_set_keyboard_interactivity(lsurf, 0);
	zwlr_layer_surface_v1_add_listener(lsurf, &ls_listener, NULL);
	wl_surface_commit(surf);
	wl_display_flush(dpy);
}

static void hide(void)
{
	if (anim_id) { g_source_remove(anim_id); anim_id = 0; }
	if (frame_cb) { wl_callback_destroy(frame_cb); frame_cb = NULL; }
	if (lsurf) zwlr_layer_surface_v1_destroy(lsurf);
	if (surf) wl_surface_destroy(surf);
	lsurf = NULL; surf = NULL; configured = 0; hover = -1;
	g_clear_pointer(&bg, cairo_surface_destroy);
	memset(bars, 0, sizeof bars);
	wl_display_flush(dpy);
}

/* pointer */
static void set_cursor(const char *name)
{
	if (!ctheme || !ptr || name == cur_name) return;
	struct wl_cursor *c = wl_cursor_theme_get_cursor(ctheme, name);
	if (!c) c = wl_cursor_theme_get_cursor(ctheme, "left_ptr");
	if (!c) return;
	struct wl_cursor_image *img = c->images[0];
	wl_pointer_set_cursor(ptr, enter_serial, csurf, img->hotspot_x, img->hotspot_y);
	wl_surface_attach(csurf, wl_cursor_image_get_buffer(img), 0, 0);
	wl_surface_damage_buffer(csurf, 0, 0, img->width, img->height);
	wl_surface_commit(csurf);
	cur_name = name;
}

static void update_hover(void)
{
	int h = -1;
	for (int i = 0; i < 3; i++)
		if (hypot(px - BX[i], py - BY) < 18) h = i;
	set_cursor(h >= 0 ? "pointer" : "default");
	if (h != hover) { hover = h; request_draw(); }
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)s;
	enter_serial = serial; cur_name = NULL;
	px = wl_fixed_to_double(x); py = wl_fixed_to_double(y);
	update_hover();
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)p; (void)serial; (void)s;
	px = py = -1; cur_name = NULL;
	if (hover != -1) { hover = -1; request_draw(); }
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)t;
	px = wl_fixed_to_double(x); py = wl_fixed_to_double(y);
	update_hover();
}
static void mpris_call(const char *method);
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
	(void)d; (void)p; (void)serial; (void)t;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;
	if (button == BTN_RIGHT) { g_main_loop_quit(loop); return; }   /* right-click closes */
	if (button != BTN_LEFT || hover < 0) return;
	static const char *m[3] = { "Previous", "PlayPause", "Next" };
	mpris_call(m[hover]);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_listener = {
	p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_axis_source, p_axis_stop, p_axis_discrete,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
		ptr = wl_seat_get_pointer(s);
		wl_pointer_add_listener(ptr, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ptr) {
		wl_pointer_release(ptr);
		ptr = NULL;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		lsh = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static gboolean wl_ready(gint fd, GIOCondition cond, gpointer d)
{
	(void)fd; (void)d;
	if ((cond & (G_IO_ERR | G_IO_HUP)) || wl_display_dispatch(dpy) < 0) {
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	wl_display_flush(dpy);
	return G_SOURCE_CONTINUE;
}

/* ═══════════════════════════════ MPRIS ═════════════════════════════ */
#define MPRIS_PREFIX "org.mpris.MediaPlayer2."

struct player {
	char *name, *owner;
	int playing;
	gint64 since;                    /* when it last started playing */
	char title[512], artist[512];
};
static GPtrArray *players;
static struct player *cur;
static int seen_spotify;

static int wanted(const char *name)
{
	if (!g_str_has_prefix(name, MPRIS_PREFIX)) return 0;
	if (spotify_only) return !strcmp(name, SPOTIFY);
	return !g_str_has_prefix(name + strlen(MPRIS_PREFIX), "playerctld");
}

static struct player *find(const char *name, const char *owner)
{
	for (guint i = 0; i < players->len; i++) {
		struct player *p = players->pdata[i];
		if ((name && !strcmp(p->name, name)) || (owner && !strcmp(p->owner, owner))) return p;
	}
	return NULL;
}

static void set_playing(int p)
{
	if (p == playing) return;
	playing = p;
	if (p) audio_start(); else audio_stop();
}

/* pick which player the widget shows, then sync the display state */
static void update(void)
{
	struct player *best = NULL;
	for (guint i = 0; i < players->len; i++) {
		struct player *p = players->pdata[i];
		if (p->playing && (!best || p->since > best->since)) best = p;
	}
	if (!best) best = (cur && find(cur->name, NULL)) ? cur : players->len ? players->pdata[0] : NULL;
	cur = best;

	if (spotify_only) {
		if (cur) { seen_spotify = 1; show(); }
		else if (seen_spotify) { g_main_loop_quit(loop); return; }   /* Spotify closed */
	}
	if (cur) {
		g_strlcpy(title, cur->title, sizeof title);
		g_strlcpy(artist, cur->artist, sizeof artist);
		if (!title[0]) {                         /* no metadata: show the player's name */
			g_strlcpy(title, cur->name + strlen(MPRIS_PREFIX), sizeof title);
			char *dot = strchr(title, '.');
			if (dot) *dot = 0;
		}
	} else {
		title[0] = artist[0] = 0;
	}
	set_playing(cur && cur->playing);
	request_draw();
}

static void mpris_call(const char *method)
{
	if (cur)
		g_dbus_connection_call(bus, cur->owner, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
				       method, NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void apply_props(struct player *p, GVariant *dict)
{
	const char *st;
	if (g_variant_lookup(dict, "PlaybackStatus", "&s", &st)) {
		int pl = !strcmp(st, "Playing");
		if (pl && !p->playing) p->since = g_get_monotonic_time();
		p->playing = pl;
	}
	GVariant *md = g_variant_lookup_value(dict, "Metadata", G_VARIANT_TYPE_VARDICT);
	if (md) {
		const char *t;
		g_strlcpy(p->title, g_variant_lookup(md, "xesam:title", "&s", &t) ? t : "", sizeof p->title);
		p->artist[0] = 0;
		GVariant *ar = g_variant_lookup_value(md, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY);
		if (ar) {
			const gchar **v = g_variant_get_strv(ar, NULL);
			gchar *j = g_strjoinv(", ", (gchar **)v);
			g_strlcpy(p->artist, j, sizeof p->artist);
			g_free(j); g_free(v); g_variant_unref(ar);
		}
		g_variant_unref(md);
	}
}

static void getall_done(GObject *src, GAsyncResult *res, gpointer name)
{
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	struct player *p = find(name, NULL);
	g_free(name);
	if (!r) return;
	if (p) {
		GVariant *dict = g_variant_get_child_value(r, 0);
		apply_props(p, dict);
		g_variant_unref(dict);
		update();
	}
	g_variant_unref(r);
}

static void getall(struct player *p)
{
	g_dbus_connection_call(bus, p->owner, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
			       "GetAll", g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"),
			       G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, getall_done,
			       g_strdup(p->name));
}

static void player_free(gpointer d)
{
	struct player *p = d;
	g_free(p->name); g_free(p->owner); g_free(p);
}

static void add_player(const char *name, const char *owner)
{
	struct player *p = find(name, NULL);
	if (!p) {
		p = g_new0(struct player, 1);
		p->name = g_strdup(name);
		g_ptr_array_add(players, p);
	}
	g_free(p->owner);
	p->owner = g_strdup(owner);
	getall(p);
}

static void name_owner_changed(GDBusConnection *c, const char *snd, const char *path, const char *ifc,
			       const char *sig, GVariant *params, gpointer d)
{
	(void)c; (void)snd; (void)path; (void)ifc; (void)sig; (void)d;
	const char *name, *old, *new;
	g_variant_get(params, "(&s&s&s)", &name, &old, &new);
	if (!wanted(name)) return;
	if (new[0]) { add_player(name, new); return; }
	struct player *p = find(name, NULL);
	if (!p) return;
	if (p == cur) cur = NULL;
	g_ptr_array_remove(players, p);
	update();
}

static void props_changed(GDBusConnection *c, const char *snd, const char *path, const char *ifc,
			  const char *sig, GVariant *params, gpointer d)
{
	(void)c; (void)path; (void)ifc; (void)sig; (void)d;
	struct player *p = find(NULL, snd);
	const char *iface;
	GVariant *changed;
	g_variant_get(params, "(&s@a{sv}@as)", &iface, &changed, NULL);
	if (p && !strcmp(iface, "org.mpris.MediaPlayer2.Player")) { apply_props(p, changed); update(); }
	g_variant_unref(changed);
}

static void owner_done(GObject *src, GAsyncResult *res, gpointer name)
{
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	if (r) {
		const char *own;
		g_variant_get(r, "(&s)", &own);
		add_player(name, own);
		g_variant_unref(r);
	}
	g_free(name);
}

static void list_done(GObject *src, GAsyncResult *res, gpointer d)
{
	(void)d;
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
	if (!r) return;
	GVariantIter *it;
	const char *name;
	g_variant_get(r, "(as)", &it);
	while (g_variant_iter_loop(it, "&s", &name))
		if (wanted(name))
			g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
					       "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", name),
					       G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, owner_done,
					       g_strdup(name));
	g_variant_iter_free(it);
	g_variant_unref(r);
}

static gboolean refresh(gpointer d)
{
	(void)d;
	for (guint i = 0; i < players->len; i++) getall(players->pdata[i]);
	return G_SOURCE_CONTINUE;
}

static void mpris_init(void)
{
	players = g_ptr_array_new_with_free_func(player_free);
	g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
					   "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					   name_owner_changed, NULL, NULL);
	g_dbus_connection_signal_subscribe(bus, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
					   "/org/mpris/MediaPlayer2", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					   props_changed, NULL, NULL);
	g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
			       "ListNames", NULL, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
			       list_done, NULL);
	g_timeout_add_seconds(5, refresh, NULL);         /* safety net if a signal is missed */
}

/* ═══════════════════════ front/back + single instance ═══════════════════════ */
static gboolean on_toggle(gpointer d)
{
	(void)d;
	front = !front;
	if (!lsurf) return G_SOURCE_CONTINUE;
	if (zwlr_layer_surface_v1_get_version(lsurf) >= 2) {
		zwlr_layer_surface_v1_set_layer(lsurf, front ? ZWLR_LAYER_SHELL_V1_LAYER_TOP
							     : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
		wl_surface_commit(surf);
		wl_display_flush(dpy);
	} else {
		hide(); show();
	}
	return G_SOURCE_CONTINUE;
}

static gboolean on_quit(gpointer d) { (void)d; g_main_loop_quit(loop); return G_SOURCE_REMOVE; }

static gboolean spotify_timeout(gpointer d)
{
	(void)d;
	if (!seen_spotify) g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

/* Returns 1 if this process should become the instance; otherwise signals the running one. */
static int single_instance(int sig)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char *path = g_strdup_printf("%s/spotglass.lock", dir ? dir : "/tmp");
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	g_free(path);
	if (fd < 0) return 1;
	if (flock(fd, LOCK_EX | LOCK_NB) == 0) {         /* we are the instance; keep fd open */
		char buf[32];
		int n = snprintf(buf, sizeof buf, "%d\n", getpid());
		if (ftruncate(fd, 0) < 0 || write(fd, buf, n) < 0) { /* pid file is best effort */ }
		return 1;
	}
	char buf[32] = { 0 };
	if (read(fd, buf, sizeof buf - 1) > 0 && sig) kill(atoi(buf), sig);
	close(fd);
	return 0;
}

static void usage(void)
{
	puts("usage: spotglass            start (in front), or toggle front/back if running\n"
	     "       spotglass --spotify  follow Spotify only; exit when Spotify closes\n"
	     "       spotglass --quit     close a running spotglass\n"
	     "right-click the widget to close it");
}

int main(int argc, char **argv)
{
	int quit = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--spotify")) spotify_only = 1;
		else if (!strcmp(argv[i], "--quit")) quit = 1;
		else { usage(); return strcmp(argv[i], "-h") && strcmp(argv[i], "--help"); }
	}
	/* already running: plain call toggles it, --spotify leaves it alone, --quit closes it */
	if (!single_instance(quit ? SIGTERM : spotify_only ? 0 : SIGUSR1) || quit) return 0;
	signal(SIGUSR1, SIG_IGN);                        /* until the main loop handles it */
	front = !spotify_only;

	FcInit();
	dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "spotglass: no wayland display\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !lsh) { fprintf(stderr, "spotglass: compositor lacks wlr-layer-shell\n"); return 1; }
	if (buffers_init() < 0) { perror("spotglass: shm"); return 1; }

	const char *cs = getenv("XCURSOR_SIZE");
	ctheme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), cs ? atoi(cs) : 24, shm);
	csurf = wl_compositor_create_surface(comp);

	pw_init(NULL, NULL);
	pwl = pw_thread_loop_new("spotglass-pw", NULL);
	pw_thread_loop_start(pwl);

	GError *err = NULL;
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!bus) { fprintf(stderr, "spotglass: %s\n", err->message); return 1; }

	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, on_quit, NULL);
	g_unix_signal_add(SIGTERM, on_quit, NULL);
	g_unix_signal_add(SIGUSR1, on_toggle, NULL);
	g_unix_fd_add(wl_display_get_fd(dpy), G_IO_IN | G_IO_ERR | G_IO_HUP, wl_ready, NULL);
	mpris_init();
	if (spotify_only) g_timeout_add_seconds(SPOTIFY_WAIT, spotify_timeout, NULL);
	else show();
	wl_display_dispatch_pending(dpy);
	wl_display_flush(dpy);

	g_main_loop_run(loop);

	audio_stop();
	hide();
	pw_thread_loop_stop(pwl);
	pw_thread_loop_destroy(pwl);
	wl_display_disconnect(dpy);
	return 0;
}
