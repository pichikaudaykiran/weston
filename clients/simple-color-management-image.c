/*
 * Copyright © 2019 Harish Krupo
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <math.h>
#include <assert.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>

#include <linux/input.h>
#include <wayland-client.h>

#include <wayland-egl.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <xf86drm.h>
#include <drm_fourcc.h>

#ifdef HAVE_LIBDRM_INTEL
#include <i915_drm.h>
#include <intel_bufmgr.h>
#endif

#ifdef HAVE_PANGO
#include <pango/pangocairo.h>
#endif

#include "color-management-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "shared/os-compatibility.h"
#include "shared/helpers.h"
#include "shared/platform.h"
#include "shared/xalloc.h"
#include <libweston/zalloc.h>
#include "window.h"

#define DBG(fmt, ...) \
	fprintf(stderr, "%d:%s " fmt, __LINE__, __func__, ##__VA_ARGS__)

#define NUM_BUFFERS 1

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0
#endif

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010         fourcc_code('P', '0', '1', '0') /* 2x2 subsampled Cb:Cr plane 10 bits per channel */
#endif

#ifndef DRM_FORMAT_P012
#define DRM_FORMAT_P012		fourcc_code('P', '0', '1', '2') /* 2x2 subsampled Cr:Cb plane, 12 bit per channel */
#endif

#ifndef DRM_FORMAT_P016
#define DRM_FORMAT_P016		fourcc_code('P', '0', '1', '6') /* 2x2 subsampled Cr:Cb plane, 16 bit per channel */
#endif

static int32_t option_help;
static int32_t option_fullscreen;
static int32_t option_subtitle;
static char* option_input_file;
static uint32_t option_pixel_format;
static uint32_t option_width;
static uint32_t option_height;

static const struct weston_option options[] = {
	{ WESTON_OPTION_BOOLEAN, "fullscreen", 'f', &option_fullscreen },
	{ WESTON_OPTION_BOOLEAN, "subtitle", 's', &option_subtitle },
	{ WESTON_OPTION_STRING, "input_file", 'i', &option_input_file },
	{ WESTON_OPTION_STRING, "pixel_format", 'p', &option_pixel_format },
	{ WESTON_OPTION_INTEGER, "width", 'w', &option_width },
	{ WESTON_OPTION_INTEGER, "height", 'h', &option_height },
	{ WESTON_OPTION_STRING, "help", 'x', &option_help },
};

static const char help_text[] =
"Usage: %s [options] FILENAME\n"
"\n"
"   -f, --fullscreen\tRun in fullscreen mode\n"
"   -s, --subtitle\tShow subtiles\n"
"   -i, --input\t\tInput Image file to render\n"
"   -p, --pix_fmt\tImage pixel format\n"
"                   YUV420 \n"
"                   NV12\n"
"                   P010 \n"
"                   ARGB8888\n"
"                   BGRA8888\n"
"                   ABGR2101010\n"
"                   ARGB2101010\n"
"   -w, --width\t\tWidth of the input image file\n"
"   -h, --height\t\tHeight of the input file\n"
"   -x, --help\t\tShow this help text\n"
"\n";


/*  NV12/P010 YUV Layout

    <----    WIDTH   ---->
    +------------------------+ ^
    |YYYYYYYYYYYYYYYYYYYY^^^^| |
    |YYYYYYYYYYYYYYYYYYYY^^^^| H
    |YYYYYYYYYYYYYYYYYYYY^^^^| E
    |YYYYYYYYYYYYYYYYYYYY^^^^| I  Luma plane (Y)
    |YYYYYYYYYYYYYYYYYYYY^^^^| G
    |YYYYYYYYYYYYYYYYYYYY^^^^| H
    |YYYYYYYYYYYYYYYYYYYY^^^^| T
    |YYYYYYYYYYYYYYYYYYYY^^^^| |
    +------------------------+ v
    |UVUVUVUVUVUVUVUVUVUV^^^^|
    |UVUVUVUVUVUVUVUVUVUV^^^^|    Chroma plane (UV)
    |UVUVUVUVUVUVUVUVUVUV^^^^|
    |UVUVUVUVUVUVUVUVUVUV^^^^|
    +------------------------+
    <----    ROW PITCH    --->
*/

struct app;
struct buffer;

struct drm_device {
	int fd;
	char *name;

	int (*alloc_bo)(struct buffer *buf);
	void (*free_bo)(struct buffer *buf);
	int (*export_bo_to_prime)(struct buffer *buf);
	int (*map_bo)(struct buffer *buf);
	void (*unmap_bo)(struct buffer *buf);
	void (*device_destroy)(struct buffer *buf);
};

struct buffer {
	struct wl_buffer *buffer;
	int busy;

	struct drm_device *dev;
	int drm_fd;

#ifdef HAVE_LIBDRM_INTEL
	drm_intel_bufmgr *bufmgr;
	drm_intel_bo *intel_bo;
#endif /* HAVE_LIBDRM_INTEL */

	uint32_t gem_handle;
	int dmabuf_fd;
	uint8_t *mmap;

	int width;
	int height;
	int bpp;             // bits per pixel
	unsigned long stride;
	int format;
	uint16_t bytes_per_pixel;
};

struct subtitle {

	struct wl_surface *wl_surface;
	int width;
	int height;

	struct widget *widget;
	uint32_t time;
	struct wl_callback *frame_cb;
	struct app *app;
	struct buffer buffers[NUM_BUFFERS];
	struct buffer *prev_buffer;
};

struct image {
	int fd;
	FILE *fp;
	int size;
	struct buffer buffers[NUM_BUFFERS];
	struct buffer *prev_buffer;
};

struct app {
	struct display *display;
	struct window *window;
	struct widget *widget;
	struct image image;

	struct subtitle *subtitle;

	struct zwp_color_manager_v1 *color_manager;
	struct zwp_color_space_v1 *color_space;
	struct zwp_color_management_surface_v1 *cm_surface;
	struct zwp_linux_dmabuf_v1 *dmabuf;
};

static int
create_dmabuf_buffer(struct app *app, struct buffer *buffer,
		     int width, int height, int format);

static void drm_shutdown(struct buffer *my_buf);

static void
destroy_dmabuf_buffer(struct buffer *buffer)
{
	wl_buffer_destroy(buffer->buffer);
	close(buffer->dmabuf_fd);
	buffer->dev->free_bo(buffer);
	drm_shutdown(buffer);
}

static void
subtitle_resize_handler(struct widget *widget,
			int32_t width, int32_t height, void *data)
{
	struct subtitle *sub = data;
	struct app *app = sub->app;
	struct rectangle allocation;
	/* struct wl_surface *surface; */
	uint32_t format;
	int i;

	widget_get_allocation(sub->widget, &allocation);

	/* surface = widget_get_wl_surface(widget); */
	/* //clear surface's buffer */
	/* wl_surface_attach(surface, NULL); */
	/* for (i = 0; i < NUM_BUFFERS; i++) { */
	/* 	destroy_dmabuf_buffer(&sub->buffers[i]); */
	/* } */

	format = DRM_FORMAT_ARGB8888;

	for (i = 0; i < NUM_BUFFERS; i++) {
		create_dmabuf_buffer(app, &sub->buffers[i],
				     allocation.width,
				     allocation.height,
				     format);

	}

}

static struct buffer *
subtitle_next_buffer(struct subtitle *sub)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++)
		if (!sub->buffers[i].busy)
			return &sub->buffers[i];

	return NULL;
}

#ifdef HAVE_PANGO
static PangoLayout *
create_layout(cairo_t *cr, const char *title)
{
	PangoLayout *layout;
	PangoFontDescription *desc;

	layout = pango_cairo_create_layout(cr);
	pango_layout_set_text(layout, title, -1);
	desc = pango_font_description_from_string("Sans Bold 15");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	pango_layout_set_auto_dir (layout, FALSE);
	pango_layout_set_single_paragraph_mode (layout, TRUE);
	pango_layout_set_width (layout, -1);

	return layout;
}
#endif

static void
fill_subtitle(struct buffer *buffer)
{
	cairo_surface_t *surface;
	cairo_t* cr;
	char *title = "Hello world";
	PangoLayout *title_layout;

	assert(buffer->mmap);

	surface = cairo_image_surface_create_for_data(buffer->mmap,
						      CAIRO_FORMAT_ARGB32,
						      buffer->width,
						      buffer->height,
						      buffer->stride);
	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
#ifdef HAVE_PANGO
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
#else
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
#endif

	cairo_paint(cr);

#ifdef HAVE_PANGO
	/* cairo_set_operator(cr, CAIRO_OPERATOR_OVER); */
	title_layout = create_layout(cr, title);
	cairo_move_to(cr, 0, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	pango_cairo_show_layout(cr, title_layout);
#endif

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
subtitle_redraw_handler(struct widget *widget, void *data)
{
	struct subtitle *sub = data;
	struct rectangle allocation;
	struct buffer *buffer;
	struct wl_surface *surface;

	widget_get_allocation(sub->widget, &allocation);
	buffer = subtitle_next_buffer(sub);

	if (!buffer->dev->map_bo(buffer)) {
		fprintf(stderr, "map_bo failed\n");
		return;
	}

	fill_subtitle(buffer);

	buffer->dev->unmap_bo(buffer);

	surface = widget_get_wl_surface(widget);
	wl_surface_attach(surface, buffer->buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, allocation.width, allocation.height);
	wl_surface_commit(surface);
	buffer->busy = 1;

	/* widget_schedule_redraw(sub->widget); */
}

static struct subtitle *
subtitle_create(struct app *app)
{
	struct subtitle *sub;

	sub = xzalloc(sizeof *sub);
	sub->app = app;

	sub->widget = window_add_subsurface(app->window, sub,
					    SUBSURFACE_SYNCHRONIZED);

	widget_set_use_cairo(sub->widget, 0);
	widget_set_resize_handler(sub->widget, subtitle_resize_handler);
	widget_set_redraw_handler(sub->widget, subtitle_redraw_handler);

	return sub;
}

static void
subtitle_destroy(struct subtitle *sub)
{
	/* int i; */

	/* for (i = 0; i < NUM_BUFFERS; i++) { */
	/* 	destroy_dmabuf_buffer(&sub->buffers[i]); */
	/* } */

	widget_destroy(sub->widget);
	free(sub);
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;
	mybuf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static struct buffer *
image_next_buffer(struct image *s)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++)
		if (!s->buffers[i].busy)
			return &s->buffers[i];

	return NULL;
}

// TODO : code is incomplete. Need to finish it */
static void
copy_yuv420p10_to_p010_to_dma_buf(struct image *image, struct buffer *buffer)
{
    int frame_size = 0, y_size = 0, u_size = 0, i = 0;

    unsigned char *y_src = NULL, *u_src = NULL, *v_src = NULL;;
    unsigned char *y_dst = NULL, *uv_dst = NULL;
    unsigned char *src_buffer = NULL;

    int bytes_per_pixel = 2;
    assert(buffer->mmap);

/*  YUV420P   -- This is a planar format. YYYYYYYY UU VV
  0  -------------------
	|                       |
	|                       |
	|                       |
	|      Y -Plane         |
	|                       |
	|                       |
	|                       |
	 ------------------- w*h
	|                       |
	|     U - Plane         |
	 ------------------- w*h/4
	|                       |
	|     V - Plane         |
	 ------------------- w*h/4

    YUV420 has 3 planes , Y, U, V.
    Total Framesize = w* h + w*h/4+w*h/4
*/
    frame_size = buffer->width * buffer->height * bytes_per_pixel * 3/2;
    y_size = buffer->width * buffer->height * bytes_per_pixel;
    u_size = buffer->width * buffer->height/4 * bytes_per_pixel;

    src_buffer = (unsigned char*)malloc(frame_size);
    fread(src_buffer, 1, frame_size, image->fp);
    y_src = src_buffer;
    u_src = src_buffer + y_size; // U offset for yuv420
    v_src = u_src + u_size; // V offset for yuv420

    y_dst = (unsigned char*)buffer->mmap + 0; // Y plane
    uv_dst = (unsigned char*)buffer->mmap + buffer->stride * buffer->height; // UV offset for P010

    for (i = 0; i < buffer->height; i++) {
        memcpy(y_dst, y_src, buffer->width * 2);
        y_dst += buffer->stride;	// Doing the line by line copy. that is the reason, he is moving after buffer->stride
        y_src += buffer->width * 2;
    }
}

static void 
copy_rgb_to_dma_buf(struct image *image, struct buffer *buffer)
{
    int frame_size = 0;
    unsigned char *src_buffer = NULL;
    unsigned char *dst_buffer = NULL;

    assert(buffer->mmap);
    frame_size = buffer->width * buffer->height * buffer->bytes_per_pixel;
    src_buffer = (unsigned char*)malloc(frame_size);
    fread(src_buffer, 1, frame_size, image->fp);

    dst_buffer = (unsigned char*)buffer->mmap + 0;
    memcpy(dst_buffer, src_buffer, frame_size);
}

static void
convert_yuv420pTonv12_and_copy_to_dma_buf(struct image *image, struct buffer *buffer)
{
    unsigned char *y_src = NULL, *u_src = NULL, *v_src = NULL;
    unsigned char *y_dst = NULL, *uv_dst = NULL;
    unsigned char *src_buffer = NULL;
    int frame_size = 0, y_size = 0, u_size = 0;
    int row = 0, col = 0;

    y_size = buffer->width * buffer->height;
    u_size = (buffer->width >> 1) * (buffer->height >> 1);
    frame_size = buffer->width * buffer->height + ((buffer->width * buffer->height) >> 1);

    syslog(LOG_INFO,"frame_size = %d, y_size = %d \n", frame_size, y_size);

    src_buffer = (unsigned char*)malloc(frame_size);
    fread(src_buffer, 1, frame_size, image->fp);

    y_src = src_buffer;
    u_src = y_src + y_size; // UV offset
    v_src = y_src + y_size + u_size;

    y_dst = (unsigned char*)buffer->mmap + 0; // Y plane
    uv_dst = (unsigned char*)buffer->mmap + buffer->stride * buffer->height; // U offset for NV12

    /* Y plane */
    for (row = 0; row < buffer->height; row++) {
        memcpy(y_dst, y_src, buffer->width);
        y_dst += buffer->stride; // Doing the line by line copy based on buffer->stride
        y_src += buffer->width;
    }

    /* UV plane */
    for (row = 0; row < buffer->height / 2; row++) {
        for (col = 0; col < buffer->width / 2; col++) {
            uv_dst[col * 2] = u_src[col];
            uv_dst[col * 2 + 1] = v_src[col];
        }

        uv_dst += buffer->stride;
        u_src += (buffer->width / 2);
        v_src += (buffer->width / 2);
    }
}

// TODO : This code is incomplete. Need to read the U and V planes separately
// and copy them to destination
static void 
copy_yuv420_to_dma_buf(struct image *image, struct buffer *buffer)
{
    int frame_size = 0, y_size = 0, u_size = 0;
    int i = 0;
	uint32_t bytes_read = 0, file_size = 0;

    unsigned char *y_src = NULL, *u_src = NULL, *v_src = NULL;
    unsigned char *y_dst = NULL, *u_dst = NULL, *v_dst = NULL;
    unsigned char *src_buffer = NULL;

    int bytes_per_pixel = 1;
    assert(buffer->mmap);

    frame_size = buffer->width * buffer->height  + ((buffer->width * buffer->height) >> 1);
    y_size = buffer->width * buffer->height * bytes_per_pixel;
	u_size = buffer->width * (buffer->height/4) * bytes_per_pixel;

	/* Even number of frames? */
    fseek(image->fp, 0L, SEEK_END);
    file_size = ftell(image->fp);
    fseek(image->fp, 0L, SEEK_SET);

    syslog(LOG_INFO,"file_size = %d, frame_size = %d, y_size = %d \n", file_size, frame_size, y_size);

    src_buffer = (unsigned char*)malloc(frame_size);
    bytes_read = fread(src_buffer, sizeof(uint8_t), frame_size, image->fp);
	if(bytes_read < frame_size) {
		syslog(LOG_ERR, "Failed to read full frame. bytes_read = %d \n", bytes_read);
	}
    y_src = src_buffer;
    u_src = src_buffer + y_size; // U offset
    v_src = src_buffer + y_size + u_size; // V offset

    fprintf(stderr, "hkps width %d height %d stride %ld\n", buffer->width, buffer->height, buffer->stride);

    y_dst = (unsigned char*)buffer->mmap + 0; // Y plane
    u_dst = (unsigned char*)buffer->mmap + buffer->stride * buffer->height; // U offset
    v_dst = (unsigned char*)buffer->mmap + buffer->stride * buffer->height * 5/4; // V offset

    for (i = 0; i < buffer->height; i++) {
        memcpy(y_dst, y_src, buffer->width);
        y_dst += buffer->stride;  // Doing the line by line copy based on buffer->stride
        y_src += buffer->width;
    }

    for (i = 0; i < buffer->height/4; i++)  {
        memcpy(u_dst, u_src, buffer->width);
        u_dst += buffer->stride;
        u_src += buffer->width;
    }

    for (i = 0; i < buffer->height/4; i++)  {
        memcpy(v_dst, v_src, buffer->width);
        v_dst += buffer->stride;
        v_src += buffer->width;
    }
}

static void
copy_nv12_to_dma_buf(struct image *image, struct buffer *buffer)
{
    int frame_size = 0, y_size = 0;
    int i = 0;

    unsigned char *y_src = NULL, *uv_src = NULL;
    unsigned char *y_dst = NULL, *uv_dst = NULL;
    unsigned char *src_buffer = NULL;

    uint32_t bytes_read = 0, file_size = 0;

    assert(buffer->mmap);
    frame_size = buffer->width * buffer->height  + ((buffer->width * buffer->height) >> 1);
    y_size = buffer->width * buffer->height;

	/* Even number of frames? */
    fseek(image->fp, 0L, SEEK_END);
    file_size = ftell(image->fp);
    fseek(image->fp, 0L, SEEK_SET);

    syslog(LOG_INFO,"file_size = %d, frame_size = %d, y_size = %d \n", file_size, frame_size, y_size);

    src_buffer = (unsigned char*)malloc(frame_size);
    bytes_read = fread(src_buffer, sizeof(uint8_t), frame_size, image->fp);
    if(bytes_read < frame_size) {
        syslog(LOG_ERR, "Failed to read full frame. bytes_read = %d \n", bytes_read);
    }

    y_src = src_buffer + 0;
    uv_src = src_buffer + y_size; // UV offset for NV12

    fprintf(stderr, "hkps width %d height %d stride %ld\n", buffer->width, buffer->height, buffer->stride);

    y_dst = (unsigned char*)buffer->mmap + 0; // Y plane
    uv_dst = (unsigned char*)buffer->mmap + buffer->stride * buffer->height; // UV offset for NV12

    for (i = 0; i < buffer->height; i++) {
      memcpy(y_dst, y_src, buffer->width);
      y_dst += buffer->stride;  // line by line copy based on buffer->stride
      y_src += buffer->width;
    }

    for (i = 0; i < buffer->height >> 1; i++)  {
      memcpy(uv_dst, uv_src, buffer->width);
      uv_dst += buffer->stride;
      uv_src += buffer->width;
    }
}

static void
copy_p010_to_dma_buf(struct image *image, struct buffer *buffer)
{
    int frame_size = 0, y_size = 0;
    int i = 0;

    unsigned char *y_src = NULL, *uv_src = NULL;
    unsigned char *y_dst = NULL, *uv_dst = NULL;
    unsigned char *src_buffer = NULL;
	int bytes_per_pixel = 2;

    assert(buffer->mmap);

    y_size = buffer->width * buffer->height * 2;

    frame_size = buffer->width * buffer->height * bytes_per_pixel * 3/2;

    src_buffer = (unsigned char*)malloc(frame_size);
    fread(src_buffer, sizeof(uint8_t), frame_size, image->fp);

    y_src = src_buffer;
    uv_src = src_buffer + y_size; // UV offset for P010

    fprintf(stderr, "hkps width %d height %d stride %ld\n", buffer->width, buffer->height, buffer->stride);

    y_dst = (unsigned char*)buffer->mmap + 0; // Y plane
    uv_dst = (unsigned char*)buffer->mmap + (buffer->stride * buffer->height); // UV offset for P010

    for (i = 0; i < buffer->height; i++) {
      memcpy(y_dst, y_src, buffer->width * bytes_per_pixel);
      y_dst += buffer->stride;  // Doing the line by line copy based on buffer->stride
      y_src += buffer->width * bytes_per_pixel;
    }

    for (i = 0; i < buffer->height >> 1; i++)  {
      memcpy(uv_dst, uv_src, buffer->width * bytes_per_pixel);
      uv_dst += buffer->stride;
      uv_src += buffer->width * bytes_per_pixel;
    }
}

static void
fill_buffer(struct buffer *buffer, struct image *image)
{

    switch (buffer->format) {
        case DRM_FORMAT_YUV420:
            copy_yuv420_to_dma_buf(image, buffer);
            break;
        case DRM_FORMAT_P010:
            copy_p010_to_dma_buf(image, buffer);
            return ;
        case DRM_FORMAT_NV12:
            copy_nv12_to_dma_buf(image, buffer);
            return ;
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_ABGR2101010:
            copy_rgb_to_dma_buf(image, buffer);
            break;
	}
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct app *app = data;
	struct image *image = &app->image;
	struct buffer *buffer;
	struct wl_buffer *wlbuffer;
	struct wl_surface *surface;

	/* usleep(window->delay); */

	buffer = image_next_buffer(image);

	// If no free buffers available schedule redraw and return;
	// XXX:TODO: Should we create new buffers here?
	if(!buffer) {
		widget_schedule_redraw(widget);
		return;
	}

	if (!buffer->dev->map_bo(buffer)) {
		fprintf(stderr, "map_bo failed\n");
		return;
	}

	fill_buffer(buffer, image);

	buffer->dev->unmap_bo(buffer);

	wlbuffer = buffer->buffer;
	surface = widget_get_wl_surface(widget);
	wl_surface_attach(surface, wlbuffer, 0, 0);
	wl_surface_damage(surface, 0, 0, buffer->width, buffer->height);
	wl_surface_commit(surface);
	/* widget_schedule_redraw(widget); */

	buffer->busy = 1;
}

/*
 * +---------------------------+
 * |   |                       |
 * |   |                       |
 * |   |vm   Video             |
 * |   |                       |
 * |   |                       |
 * |___+-------------------+   |
 * | hm| Subtitle          |   |
 * |   +-------------------+   |
 * |                           |
 * +---------------------------+
 *
 * hm : horizontal margin
 * vm : vertical margin
 */

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct app *app = data;
	struct rectangle area;

	// margin is in percentage
	int vm = 85, hm = 40;
	int x, y, w, h;
	int mhorizontal, mvertical;

	if (app->subtitle) {
		widget_get_allocation(widget, &area);

		mhorizontal = area.width * hm / 100;
		mvertical = area.height * vm / 100;

		x = area.x + mhorizontal;
		y = area.y + mvertical;
		w = area.width * 2 / 10; // 20% of total width
		h = area.height / 20; // 5% of total height

		widget_set_allocation(app->subtitle->widget,
				      x, y, w, h);
	}

}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	struct app *app = data;

	window_schedule_redraw(app->window);
}

static void
key_handler(struct window *window, struct input *input, uint32_t time,
	    uint32_t key, uint32_t sym,
	    enum wl_keyboard_key_state state, void *data)
{
	struct app *app = data;
	struct rectangle winrect;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
		return;

	switch (sym) {
	case XKB_KEY_Up:
		window_get_allocation(window, &winrect);
		winrect.height -= 100;
		if (winrect.height < 150)
			winrect.height = 150;
		window_schedule_resize(window, winrect.width, winrect.height);
		break;
	case XKB_KEY_Down:
		window_get_allocation(window, &winrect);
		winrect.height += 100;
		if (winrect.height > 600)
			winrect.height = 600;
		window_schedule_resize(window, winrect.width, winrect.height);
		break;
	case XKB_KEY_Escape:
		display_exit(app->display);
		break;
	}
}

static void image_close(struct image *s)
{
	fclose(s->fp);
}

static bool image_open(struct app *app,
		       struct image *image,
		       const char *filename)
{
	image->fp = fopen(filename, "r");
	return true;
}

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
	/* struct app *app = data; */
	/* uint64_t modifier = ((uint64_t) modifier_hi << 32) | modifier_lo; */
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
	/* XXX: deprecated */
}


static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format,
	dmabuf_modifiers
};

static void
global_handler(struct display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{

	struct app *app = data;

	if (strcmp(interface, "zwp_color_manager_v1") == 0) {
		app->color_manager =
			display_bind(display, id,
				     &zwp_color_manager_v1_interface, 1);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		if (version < 3)
			return;
		app->dmabuf =
			display_bind(display, id,
				     &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(app->dmabuf,
						 &dmabuf_listener,
						 app);
	}

}

static void
global_handler_remove(struct display *display, uint32_t id,
		      const char *interface, uint32_t version, void *data)
{
}

#ifdef HAVE_LIBDRM_INTEL
static int
intel_alloc_bo(struct buffer *my_buf)
{
	/* XXX: try different tiling modes for testing FB modifiers. */
	uint32_t tiling = I915_TILING_NONE;

	assert(my_buf->bufmgr);

	my_buf->intel_bo = drm_intel_bo_alloc_tiled(my_buf->bufmgr, "test",
						    my_buf->width, my_buf->height,
						    (my_buf->bpp / 8), &tiling,
						    &my_buf->stride, 0);

	if (!my_buf->intel_bo)
		return 0;

	if (tiling != I915_TILING_NONE)
		return 0;

	return 1;
}

static void
intel_free_bo(struct buffer *my_buf)
{
	drm_intel_bo_unreference(my_buf->intel_bo);
}

static int
intel_map_bo(struct buffer *my_buf)
{
	if (drm_intel_gem_bo_map_gtt(my_buf->intel_bo) != 0)
		return 0;

	my_buf->mmap = my_buf->intel_bo->virtual;

	return 1;
}

static int
intel_bo_export_to_prime(struct buffer *buffer)
{
	return drm_intel_bo_gem_export_to_prime(buffer->intel_bo, &buffer->dmabuf_fd);
}

static void
intel_unmap_bo(struct buffer *my_buf)
{
	drm_intel_gem_bo_unmap_gtt(my_buf->intel_bo);
}

static void
intel_device_destroy(struct buffer *my_buf)
{
	drm_intel_bufmgr_destroy(my_buf->bufmgr);
}

#endif /* HAVE_LIBDRM_INTEL */

static void
drm_device_destroy(struct buffer *buf)
{
	buf->dev->device_destroy(buf);
	close(buf->drm_fd);
}

static int
drm_device_init(struct buffer *buf)
{
	struct drm_device *dev = calloc(1, sizeof(struct drm_device));

	drmVersionPtr version = drmGetVersion(buf->drm_fd);

	dev->fd = buf->drm_fd;
	dev->name = strdup(version->name);
	if (0) {
		/* nothing */
	}
#ifdef HAVE_LIBDRM_INTEL
	else if (!strcmp(dev->name, "i915")) {
		fprintf(stderr, "TRACE:: drm device %s supported.\n",
			dev->name);
		buf->bufmgr = drm_intel_bufmgr_gem_init(buf->drm_fd, 32);
		if (!buf->bufmgr) {
			free(dev->name);
			free(dev);
			return 0;
		}
		dev->alloc_bo = intel_alloc_bo;
		dev->free_bo = intel_free_bo;
		dev->export_bo_to_prime = intel_bo_export_to_prime;
		dev->map_bo = intel_map_bo;
		dev->unmap_bo = intel_unmap_bo;
		dev->device_destroy = intel_device_destroy;
	}
#endif
	else {
		fprintf(stderr, "Error: drm device %s unsupported.\n",
			dev->name);
		free(dev->name);
		free(dev);
		return 0;
	}
	buf->dev = dev;
	return 1;
}

static int
drm_connect(struct buffer *my_buf)
{
	/* This won't work with card0 as we need to be authenticated; instead,
	 * boot with drm.rnodes=1 and use that. */
	my_buf->drm_fd = open("/dev/dri/renderD128", O_RDWR);
	if (my_buf->drm_fd < 0)
		return 0;

	return drm_device_init(my_buf);
}

static void
drm_shutdown(struct buffer *my_buf)
{
	drm_device_destroy(my_buf);
}


/* static void */
/* create_succeeded(void *data, */
/* 		 struct zwp_linux_buffer_params_v1 *params, */
/* 		 struct wl_buffer *new_buffer) */
/* { */
/* 	struct buffer *buffer = data; */

/* 	buffer->buffer = new_buffer; */
/* 	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer); */

/* 	zwp_linux_buffer_params_v1_destroy(params); */
/* } */

/* static void */
/* create_failed(void *data, struct zwp_linux_buffer_params_v1 *params) */
/* { */
/* 	struct buffer *buffer = data; */

/* 	buffer->buffer = NULL; */
/* 	running = 0; */

/* 	zwp_linux_buffer_params_v1_destroy(params); */

/* 	fprintf(stderr, "Error: zwp_linux_buffer_params.create failed.\n"); */
/* } */

/* static const struct zwp_linux_buffer_params_v1_listener params_listener = { */
/* 	create_succeeded, */
/* 	create_failed */
/* }; */

static int
create_dmabuf_buffer(struct app *app, struct buffer *buffer,
		     int width, int height, int format)
{
	struct zwp_linux_buffer_params_v1 *params;
	uint64_t modifier = 0;
	uint32_t flags = 0;
	struct drm_device *drm_dev;

	if (!drm_connect(buffer)) {
		fprintf(stderr, "drm_connect failed\n");
		goto error;
	}

	drm_dev = buffer->dev;

	buffer->width = width;
	buffer->height = height;
	buffer->format = format;

	switch (format) {
	case DRM_FORMAT_NV12:
		/* adjust height for allocation of NV12 Y and UV planes */
		buffer->height = height * 3 / 2;
		buffer->bpp = 8;
		break;
	case DRM_FORMAT_YUV420:
		buffer->height = height * 3 / 2;
		buffer->bpp = 8;
		break;
	case DRM_FORMAT_P010:
		buffer->height = height * 3 / 2;
		buffer->bpp = 16;
		break;
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_ABGR2101010:
        buffer->height = height;
        buffer->bytes_per_pixel = 4;
    default:
        buffer->height = height;
        buffer->bpp = 32;
        buffer->bytes_per_pixel = 4;
    }

	if (!drm_dev->alloc_bo(buffer)) {
		fprintf(stderr, "alloc_bo failed\n");
		goto error1;
	}

	if (drm_dev->export_bo_to_prime(buffer) != 0) {
		fprintf(stderr, "gem_export_to_prime failed\n");
		goto error2;
	}
	if (buffer->dmabuf_fd < 0) {
		fprintf(stderr, "error: dmabuf_fd < 0\n");
		goto error2;
	}

	/* We now have a dmabuf! For format XRGB8888, it should contain 2x2
	 * tiles (i.e. each tile is 256x256) of misc colours, and be mappable,
	 * either as ARGB8888, or XRGB8888. For format NV12, it should contain
	 * the Y and UV components, and needs to be re-adjusted for passing the
	 * correct height to the compositor.
	 */
	buffer->height = height;

	syslog(LOG_INFO,"buffer->width = %d , buffer->height = %d, buffer->stride = %ld, format = 0x%x \n",
				buffer->width,buffer->height,buffer->stride, buffer->format);

	params = zwp_linux_dmabuf_v1_create_params(app->dmabuf);
	zwp_linux_buffer_params_v1_add(params,
				       buffer->dmabuf_fd,
				       0, /* plane_idx */
				       0, /* offset */
				       buffer->stride,
				       modifier >> 32,
				       modifier & 0xffffffff);

	switch (format) {
	case DRM_FORMAT_NV12:
		/* add the second plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       1,
					       buffer->stride * buffer->height,
					       buffer->stride,
					       modifier >> 32,
					       modifier & 0xffffffff);
		break;
	case DRM_FORMAT_YUV420:
		/* add the second plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       1,
					       buffer->stride * buffer->height,
					       buffer->stride / 2,
					       modifier >> 32,
					       modifier & 0xffffffff);

		/* add the third plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       2,
					       buffer->stride * buffer->height * 3 / 2,
					       buffer->stride / 2,
					       modifier >> 32,
					       modifier & 0xffffffff);
		break;
	case DRM_FORMAT_P010:
		/* add the second plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       1,
					       buffer->stride * buffer->height,
					       buffer->stride,
					       modifier >> 32,
					       modifier & 0xffffffff);
		break;
	default:
		break;
	}

	/* zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer); */
	/* if (display->req_dmabuf_immediate) { */

	// Lets try immediate
	buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params,
								 buffer->width,
								 buffer->height,
								 format,
								 flags);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	/* } */
	/* else */
	/* 	zwp_linux_buffer_params_v1_create(params, */
	/* 				  buffer->width, */
	/* 				  buffer->height, */
	/* 				  format, */
	/* 				  flags); */

	return 0;

error2:
	drm_dev->free_bo(buffer);
error1:
	drm_shutdown(buffer);
error:
	return -1;
}

static struct app *
image_create(struct display *display, const char *filename)
{
	struct app *app;
	struct wl_surface *surface;
	struct wl_display *wldisplay;
	uint32_t i, width, height, format;
	int ret;
	struct buffer *buffer;

	app = xzalloc(sizeof *app);

	app->display = display;
	display_set_user_data(app->display, app);
	display_set_global_handler(display, global_handler);
	display_set_global_handler_remove(display, global_handler_remove);

	// Ensure that we have received the DMABUF format and modifier support
	wldisplay = display_get_display(display);
	wl_display_roundtrip(wldisplay);

	app->window = window_create(app->display);
	app->widget = window_add_widget(app->window, app);
	window_set_title(app->window, "Wayland Simple HDR image");

	window_set_key_handler(app->window, key_handler);
	window_set_user_data(app->window, app);
	window_set_keyboard_focus_handler(app->window, keyboard_focus_handler);

	widget_set_redraw_handler(app->widget, redraw_handler);
	widget_set_resize_handler(app->widget, resize_handler);

	widget_set_use_cairo(app->widget, 0);

	if (!image_open(app, &app->image, filename))
		goto err;

	surface = window_get_wl_surface(app->window);
	if (app->color_manager == NULL) {
		fprintf(stderr, "error: No color manager global \n");
		free(app);
		return NULL;
	}

	app->cm_surface = zwp_color_manager_v1_get_color_management_surface(
					app->color_manager,
					widget_get_wl_surface(app->widget)); // surface
	if (app->cm_surface == NULL) {
		fprintf(stderr, "error: cm_surface is NULL \n");
		free(app);
		return NULL;
	}

	app->color_space = zwp_color_manager_v1_create_color_space_from_names(app->color_manager,
			ZWP_COLOR_MANAGER_V1_EOTF_NAMES_SRGB, // EOTF
			ZWP_COLOR_MANAGER_V1_CHROMATICITY_NAMES_BT709, // Chromaticity
			ZWP_COLOR_MANAGER_V1_WHITEPOINT_NAMES_D65 //Whitepoint
			);
	if (app->color_space == NULL) {
		fprintf(stderr, "error: color_space is NULL \n");
		free(app);
		return NULL;
	}

	zwp_color_management_surface_v1_set_color_space(
			app->cm_surface,
			app->color_space,
			ZWP_COLOR_MANAGEMENT_SURFACE_V1_RENDER_INTENT_RELATIVE,
			ZWP_COLOR_MANAGEMENT_SURFACE_V1_ALPHA_MODE_STRAIGHT);

	/* if (option_subtitle) */
	/* 	app->subtitle = subtitle_create(app); */

	width = option_width;
	height = option_height;
	format = option_pixel_format;

	if (option_fullscreen) {
		window_set_fullscreen(app->window, 1);
	} else {
		/* if not fullscreen, resize as per the video size */
		widget_schedule_resize(app->widget, width, height);
	}

	for (i = 0; i < NUM_BUFFERS; i++) {
		buffer = &app->image.buffers[i];
		ret = create_dmabuf_buffer(app, buffer, width, height, format);

		if (ret < 0)
			goto err;
	}

	return app;

err:
	free(app);
	return NULL;
}

static void
image_destroy(struct app *app)
{
	if (app->subtitle)
		subtitle_destroy(app->subtitle);

	image_close(&app->image);

	widget_destroy(app->widget);
	window_destroy(app->window);
	free(app);
}

static int parse_pixel_format(const char* c)
{
	if (c != NULL) {
		if (!strcmp(c, "YUV420"))
			return DRM_FORMAT_YUV420;
		else if (!strcmp(c, "NV12"))
			return DRM_FORMAT_NV12;
		else if (!strcmp(c, "XRGB8888"))
			return DRM_FORMAT_XRGB8888;
		else if (!strcmp(c, "ARGB8888"))
			return DRM_FORMAT_ARGB8888;
		else if (!strcmp(c, "BGRA8888"))
			return DRM_FORMAT_BGRA8888;
		else if (!strcmp(c, "ABGR2101010"))
			return DRM_FORMAT_ABGR2101010;
		else if (!strcmp(c, "ARGB2101010"))
			return DRM_FORMAT_ARGB2101010;
		else if (!strcmp(c, "P010"))
			return DRM_FORMAT_P010;
	}
	return DRM_FORMAT_XRGB8888;
}

static int
is_true(const char* c)
{
	int ret = 0;
	if (c != NULL && ((c[0] ==  '1') || !strcasecmp(c, "true")))
		ret = 1;
	return ret;
}

int
main(int argc, char *argv[])
{
	struct display *display;
	struct app *app;

	int c, option_index, long_options;

    while ((c = getopt_long(argc, argv, "f:s:i:p:w:h:x:",
                   long_options, &option_index)) != -1) {
		switch (c) {
        case 'f':
			if (is_true(optarg))
				option_fullscreen = 1;
			else
				option_fullscreen = 0;
			break;
			
        case 's':
			if (is_true(optarg))
				option_subtitle = 1;
			else
				option_subtitle = 0;
			break;

		case 'i':
			option_input_file = optarg;
			break;

		case 'w':
			option_width = atoi(optarg);
			break;

		case 'h':
			option_height = atoi(optarg);
			break;

		case 'p':
			option_pixel_format = parse_pixel_format(optarg);
			break;

		case 'x':
		default:
			syslog(LOG_INFO,help_text, argv[0]);
			return 0;
		}
	}

	if( option_input_file == NULL) {
		syslog(LOG_INFO,help_text, argv[0]);
		return 0;
	}

	display = display_create(&argc, argv);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	if (!display_has_subcompositor(display)) {
		fprintf(stderr, "compositor does not support "
                        "the subcompositor extension\n");
		return -1;
	}

	app = image_create(display, option_input_file);
	if (!app) {
		fprintf(stderr, "Failed to initialize!");
		exit(EXIT_FAILURE);
	}

	display_run(display);

	image_destroy(app);
	display_destroy(display);

	return 0;
}
