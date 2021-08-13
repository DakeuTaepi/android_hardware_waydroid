/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wayland-hwc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <linux/input.h>
#include <drm_fourcc.h>
#include <system/graphics.h>

#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <hardware/gralloc.h>
#include <log/log.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>

#include <wayland-client.h>
#include <wayland-android-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct buffer;

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;

	//ALOGE("*** %s: Signaling release fence for buffer %p with fence %d", __func__, mybuf, mybuf->release_fence_fd);
    mybuf->busy = false;
	sw_sync_timeline_inc(mybuf->timeline_fd, 1);
	close(mybuf->release_fence_fd);
	mybuf->release_fence_fd = -1;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

int
create_android_wl_buffer(struct display *display, struct buffer *buffer,
		     int width, int height, int format,
		     int stride, buffer_handle_t target)
{
	struct android_wlegl_handle *wlegl_handle;
	struct wl_array ints;
	int *the_ints;

	buffer->width = width;
	buffer->height = height;
	buffer->bpp = 32;
	buffer->format = format;
	buffer->handle = target;
	buffer->stride = stride;

	wl_array_init(&ints);
	the_ints = (int *)wl_array_add(&ints, target->numInts * sizeof(int));
	memcpy(the_ints, target->data + target->numFds, target->numInts * sizeof(int));
	wlegl_handle = android_wlegl_create_handle(display->android_wlegl, target->numFds, &ints);
	wl_array_release(&ints);

	for (int i = 0; i < target->numFds; i++) {
		android_wlegl_handle_add_fd(wlegl_handle, target->data[i]);
	}

	buffer->buffer = android_wlegl_create_buffer(display->android_wlegl, buffer->width, buffer->height, buffer->stride, buffer->format, GRALLOC_USAGE_HW_RENDER, wlegl_handle);
	android_wlegl_handle_destroy(wlegl_handle);

	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

	return 0;
}

static void
create_succeeded(void *data,
         struct zwp_linux_buffer_params_v1 *params,
         struct wl_buffer *new_buffer)
{
    struct buffer *buffer = data;

    buffer->buffer = new_buffer;
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

    zwp_linux_buffer_params_v1_destroy(params);
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
    struct buffer *buffer = data;

    buffer->buffer = NULL;

    zwp_linux_buffer_params_v1_destroy(params);

    ALOGE("%s: zwp_linux_buffer_params.create failed.", __func__);
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    create_succeeded,
    create_failed
};

bool isFormatSupported(struct display *display, uint32_t format) {
	for (int i = 0; i < display->formats_count; i++) {
		if (format == display->formats[i])
			return true;
	}
	return false;
}

int ConvertHalFormatToDrm(struct display *display, uint32_t hal_format) {
	uint32_t fmt;

	switch (hal_format) {
		case HAL_PIXEL_FORMAT_RGB_888:
			fmt = DRM_FORMAT_RGB888;
			if (!isFormatSupported(display, fmt))
				fmt = DRM_FORMAT_BGR888;
			break;
		case HAL_PIXEL_FORMAT_BGRA_8888:
			fmt = DRM_FORMAT_ABGR8888;
			if (!isFormatSupported(display, fmt))
				fmt = DRM_FORMAT_ARGB8888;
			break;
		case HAL_PIXEL_FORMAT_RGBX_8888:
			fmt = DRM_FORMAT_XRGB8888;
			if (!isFormatSupported(display, fmt))
				fmt = DRM_FORMAT_XBGR8888;
			break;
		case HAL_PIXEL_FORMAT_RGBA_8888:
			fmt = DRM_FORMAT_ARGB8888;
			if (!isFormatSupported(display, fmt))
				fmt = DRM_FORMAT_ABGR8888;
			break;
		case HAL_PIXEL_FORMAT_RGB_565:
			fmt = DRM_FORMAT_RGB565;
			if (!isFormatSupported(display, fmt))
				fmt = DRM_FORMAT_BGR565;
			break;
		case HAL_PIXEL_FORMAT_YV12:
			fmt = DRM_FORMAT_YVU420;
			if (!isFormatSupported(display, fmt))
				fmt = DRM_FORMAT_GR88;
			break;
		default:
			ALOGE("Cannot convert hal format to drm format %u", hal_format);
			return -EINVAL;
	}
	if (!isFormatSupported(display, fmt)) {
		ALOGE("Current wayland display doesn't support hal format %u", hal_format);
		return -EINVAL;
	}
	return fmt;
}

int
create_dmabuf_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int format,
             int prime_fd, int stride, uint64_t modifier,
             buffer_handle_t target)
{
    struct zwp_linux_buffer_params_v1 *params;

    assert(prime_fd >= 0);
    buffer->format = ConvertHalFormatToDrm(display, format);
    assert(buffer->format >= 0);
    buffer->width = width;
    buffer->height = height;
    buffer->bpp = 32;
    buffer->handle = target;
    buffer->stride = stride;

    params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
    zwp_linux_buffer_params_v1_add(params, prime_fd, 0, 0, buffer->stride, modifier >> 32, modifier & 0xffffffff);
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer);

    buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params, buffer->width, buffer->height, buffer->format, 0);
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

    return 0;
}

static struct buffer *
window_next_buffer(struct window *window)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++)
		if (!window->buffers[i].busy)
			return &window->buffers[i];

	return NULL;
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	struct buffer *buffer;

	buffer = window_next_buffer(window);
	if (!buffer) {
		fprintf(stderr,
			!callback ? "Failed to create the first buffer.\n" :
			"All buffers busy at redraw(). Server bug?\n");
		abort();
	}

	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, window->width, window->height);
	if (window->display->scale > 1)
		wl_surface_set_buffer_scale(window->surface, window->display->scale);

	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
	wl_surface_commit(window->surface);
	buffer->busy = 1;
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *surface,
				 uint32_t serial)
{
	struct window *window = data;

	xdg_surface_ack_configure(surface, serial);
	
	if (window->initialized && window->wait_for_configure)
		redraw(window, NULL, 0);

	window->wait_for_configure = false;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	xdg_surface_handle_configure,
};

struct window *
create_window(struct display *display, int width, int height)
{
	struct window *window;

	window = calloc(1, sizeof *window);
	if (!window)
		return NULL;

	window->callback = NULL;
	window->display = display;
	window->width = width;
	window->height = height;
	window->surface = wl_compositor_create_surface(display->compositor);

	if (display->wm_base) {
		window->xdg_surface =
				xdg_wm_base_get_xdg_surface(display->wm_base, window->surface);
		assert(window->xdg_surface);
		
		xdg_surface_add_listener(window->xdg_surface,
									 &xdg_surface_listener, window);

		window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);

		assert(window->xdg_toplevel);

		xdg_toplevel_set_title(window->xdg_toplevel, "Anbox");

		window->wait_for_configure = true;
		wl_surface_commit(window->surface);
	} else {
		assert(0);
	}
	return window;
}

static int
ensure_pipe(struct display* display, int input_type)
{
	if (display->input_fd[input_type] == -1) {
		display->input_fd[input_type] = open(INPUT_PIPE_NAME[input_type], O_WRONLY | O_NONBLOCK);
		if (display->input_fd[input_type] == -1) {
			ALOGE("Failed to open pipe to InputFlinger: %s", strerror(errno));
			return -1;
		}
	}
	return 0;
}

#define ADD_EVENT(type_, code_, value_)            \
	event[n].time.tv_sec = rt.tv_sec;              \
	event[n].time.tv_usec = rt.tv_nsec / 1000;     \
	event[n].type = type_;                         \
	event[n].code = code_;                         \
	event[n].value = value_;                       \
	n++;

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
               uint32_t format, int fd, uint32_t size)
{
	/* Just so we don’t leak the keymap fd */
	close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
					  uint32_t serial, struct wl_surface *surface,
					  struct wl_array *keys)
{
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
					  uint32_t serial, struct wl_surface *surface)
{
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
					uint32_t serial, uint32_t time, uint32_t key,
					uint32_t state)
{
	struct display* display = (struct display*)data;
	struct input_event event[6];
	struct timespec rt;
	int res, n = 0;

	if (ensure_pipe(display, INPUT_KEYBOARD))
		return;

	if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
		ALOGE("%s:%d error in touch clock_gettime: %s",
			  __FILE__, __LINE__, strerror(errno));
	}
	ADD_EVENT(EV_KEY, key, state);

	res = write(display->input_fd[INPUT_KEYBOARD], &event, sizeof(event));
	if (res < sizeof(event))
		ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
						  uint32_t serial, uint32_t mods_depressed,
						  uint32_t mods_latched, uint32_t mods_locked,
						  uint32_t group)
{
}

static void
keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
							int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
	keyboard_handle_repeat_info,
};

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
					 uint32_t serial, struct wl_surface *surface,
					 wl_fixed_t sx, wl_fixed_t sy)
{
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
					 uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
					  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	struct display* display = (struct display*)data;
	struct input_event event[6];
	struct timespec rt;
	int x, y, res, n = 0;

	if (ensure_pipe(display, INPUT_POINTER))
		return;

	if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
		ALOGE("%s:%d error in touch clock_gettime: %s",
			  __FILE__, __LINE__, strerror(errno));
	}
	x = wl_fixed_to_int(sx);
	y = wl_fixed_to_int(sy);
	if (display->scale > 1) {
		x *= display->scale;
		y *= display->scale;
	}

	ADD_EVENT(EV_ABS, ABS_X, x);
	ADD_EVENT(EV_ABS, ABS_Y, y);
	ADD_EVENT(EV_REL, REL_X, display->ptrPrvX - x);
	ADD_EVENT(EV_REL, REL_Y, display->ptrPrvY - y);
	ADD_EVENT(EV_SYN, SYN_REPORT, 0);
	display->ptrPrvX = x;
	display->ptrPrvY = y;

	res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
	if (res < sizeof(event))
		ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
					  uint32_t serial, uint32_t time, uint32_t button,
					  uint32_t state)
{
	struct display* display = (struct display*)data;
	struct input_event event[6];
	struct timespec rt;
	int res, n = 0;

	if (ensure_pipe(display, INPUT_POINTER))
		return;

	if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
		ALOGE("%s:%d error in touch clock_gettime: %s",
			  __FILE__, __LINE__, strerror(errno));
	}
	ADD_EVENT(EV_KEY, button, state);
	ADD_EVENT(EV_SYN, SYN_REPORT, 0);

	res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
	if (res < sizeof(event))
		ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
					uint32_t time, uint32_t axis, wl_fixed_t value)
{
	struct display* display = (struct display*)data;
	struct input_event event[6];
	struct timespec rt;
	int res, n = 0;

	if (ensure_pipe(display, INPUT_POINTER))
		return;

	if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
		ALOGE("%s:%d error in touch clock_gettime: %s",
			  __FILE__, __LINE__, strerror(errno));
	}
	ADD_EVENT(EV_REL, (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
			  ? REL_WHEEL : REL_HWHEEL, wl_fixed_to_int(value));
	ADD_EVENT(EV_SYN, SYN_REPORT, 0);

	res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
	if (res < sizeof(event))
		ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer,
						   uint32_t axis_source)
{
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
						 uint32_t time, uint32_t axis)
{
}

static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
							 uint32_t axis, int32_t discrete)
{
}

static void
pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,
	pointer_handle_leave,
	pointer_handle_motion,
	pointer_handle_button,
	pointer_handle_axis,
	pointer_handle_frame,
	pointer_handle_axis_source,
	pointer_handle_axis_stop,
	pointer_handle_axis_discrete,
};

static int
get_touch_id(struct display *display, int id)
{
	int i = 0;
	for (i = 0; i < MAX_TOUCHPOINTS; i++) {
		if (display->touch_id[i] == id)
			return i;
	}
	for (i = 0; i < MAX_TOUCHPOINTS; i++) {
		if (display->touch_id[i] == -1) {
			display->touch_id[i] = id;
			return i;
		}
	}
	return -1;
}

static int
flush_touch_id(struct display *display, int id)
{
	for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
		if (display->touch_id[i] == id) {
			display->touch_id[i] = -1;
			return i;
		}
	}
	return -1;
}

static void
touch_handle_down(void *data, struct wl_touch *wl_touch,
		  uint32_t serial, uint32_t time, struct wl_surface *surface,
		  int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct display* display = (struct display*)data;
	struct input_event event[6];
	struct timespec rt;
	int x, y, res, n = 0;

	if (ensure_pipe(display, INPUT_TOUCH))
		return;

	if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
	   ALOGE("%s:%d error in touch clock_gettime: %s",
			__FILE__, __LINE__, strerror(errno));
	}
	x = wl_fixed_to_int(x_w);
	y = wl_fixed_to_int(y_w);
	if (display->scale > 1) {
		x *= display->scale;
		y *= display->scale;
	}

	ADD_EVENT(EV_ABS, ABS_MT_SLOT, get_touch_id(display, id));
	ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, get_touch_id(display, id));
	ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
	ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
	ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
	ADD_EVENT(EV_SYN, SYN_REPORT, 0);

	res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
	if (res < sizeof(event))
		ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id)
{
	struct display* display = (struct display*)data;
	struct input_event event[4];
	struct timespec rt;
	int res, n = 0;

	if (ensure_pipe(display, INPUT_TOUCH))
		return;

	if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
	   ALOGE("%s:%d error in touch clock_gettime: %s",
			__FILE__, __LINE__, strerror(errno));
	}
	ADD_EVENT(EV_ABS, ABS_MT_SLOT, flush_touch_id(display, id));
	ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
	ADD_EVENT(EV_SYN, SYN_REPORT, 0);

	res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
	if (res < sizeof(event))
		ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_motion(void *data, struct wl_touch *wl_touch,
			uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct display* display = (struct display*)data;
	struct input_event event[6];
	struct timespec rt;
	int x, y, res, n = 0;

	if (ensure_pipe(display, INPUT_TOUCH))
		return;

	if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
	   ALOGE("%s:%d error in touch clock_gettime: %s",
			__FILE__, __LINE__, strerror(errno));
	}
	x = wl_fixed_to_int(x_w);
	y = wl_fixed_to_int(y_w);
	if (display->scale > 1) {
		x *= display->scale;
		y *= display->scale;
	}

	ADD_EVENT(EV_ABS, ABS_MT_SLOT, get_touch_id(display, id));
	ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, get_touch_id(display, id));
	ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
	ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
	ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
	ADD_EVENT(EV_SYN, SYN_REPORT, 0);

	res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
	if (res < sizeof(event))
		ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_shape(void *data, struct wl_touch *wl_touch, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{
	struct display* display = (struct display*)data;
	struct input_event event[6];
	struct timespec rt;
	int res, n = 0;

	if (ensure_pipe(display, INPUT_TOUCH))
		return;

	if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
	   ALOGE("%s:%d error in touch clock_gettime: %s",
			__FILE__, __LINE__, strerror(errno));
	}
	ADD_EVENT(EV_ABS, ABS_MT_SLOT, get_touch_id(display, id));
	ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, get_touch_id(display, id));
	ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MAJOR, wl_fixed_to_int(major));
	ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MINOR, wl_fixed_to_int(minor));
	ADD_EVENT(EV_SYN, SYN_REPORT, 0);

	res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
	if (res < sizeof(event))
		ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_orientation(void *data, struct wl_touch *wl_touch, int32_t id, wl_fixed_t orientation)
{
}

static const struct wl_touch_listener touch_listener = {
	touch_handle_down,
	touch_handle_up,
	touch_handle_motion,
	touch_handle_frame,
	touch_handle_cancel,
	touch_handle_shape,
	touch_handle_orientation,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	xdg_wm_base_ping,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
			 enum wl_seat_capability caps)
{
	struct display *d = data;
    
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
		d->pointer = wl_seat_get_pointer(seat);
		d->input_fd[INPUT_POINTER] = -1;
		d->ptrPrvX = 0;
		d->ptrPrvY = 0;
		mkfifo(INPUT_PIPE_NAME[INPUT_POINTER], S_IRWXO | S_IRWXG | S_IRWXU);
		wl_pointer_add_listener(d->pointer, &pointer_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
		remove(INPUT_PIPE_NAME[INPUT_POINTER]);
		wl_pointer_destroy(d->pointer);
		d->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
		d->keyboard = wl_seat_get_keyboard(seat);
		d->input_fd[INPUT_KEYBOARD] = -1;
		mkfifo(INPUT_PIPE_NAME[INPUT_KEYBOARD], S_IRWXO | S_IRWXG | S_IRWXU);
		wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
		remove(INPUT_PIPE_NAME[INPUT_KEYBOARD]);
		wl_keyboard_destroy(d->keyboard);
		d->keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
		d->touch = wl_seat_get_touch(seat);
		d->input_fd[INPUT_TOUCH] = -1;
		mkfifo(INPUT_PIPE_NAME[INPUT_TOUCH], S_IRWXO | S_IRWXG | S_IRWXU);
		for (int i = 0; i < MAX_TOUCHPOINTS; i++)
			d->touch_id[i] = -1;
		wl_touch_set_user_data(d->touch, d);
		wl_touch_add_listener(d->touch, &touch_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
		remove(INPUT_PIPE_NAME[INPUT_TOUCH]);
		wl_touch_destroy(d->touch);
		d->touch = NULL;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *seat,
				 const char *name)
{
	ALOGW("seat name: %s\n", name);
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
	seat_handle_name,
};

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct display *d = data;

	++d->formats_count;
	d->formats = realloc(d->formats,
					d->formats_count * sizeof(*d->formats));
	d->formats[d->formats_count - 1] = format;
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
	struct display *d = data;

	++d->formats_count;
	d->formats = realloc(d->formats,
					d->formats_count * sizeof(*d->formats));
	d->formats[d->formats_count - 1] = format;
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format,
	dmabuf_modifiers
};

static void
output_handle_mode(void *data, struct wl_output *wl_output,
		   uint32_t flags, int32_t width, int32_t height,
		   int32_t refresh)
{
	struct display *d = data;

	d->width = width;
	d->height = height;
}

static void
output_handle_geometry(void *data, struct wl_output *wl_output,
		       int32_t x, int32_t y,
		       int32_t physical_width, int32_t physical_height,
		       int32_t subpixel,
		       const char *make, const char *model,
		       int32_t output_transform)
{
}

static void
output_handle_done(void *data, struct wl_output *wl_output)
{
}

static void
output_handle_scale(void *data, struct wl_output *wl_output,
		    int32_t scale)
{
	struct display *d = data;

	d->scale = scale;
}

static const struct wl_output_listener output_listener = {
	output_handle_geometry,
	output_handle_mode,
	output_handle_done,
	output_handle_scale,
};

static void
presentation_clock_id(void *data, struct wp_presentation *presentation,
		      uint32_t clk_id)
{
        ALOGE("*** %s: clk_id %d CLOCK_MONOTONIC %d", __func__, clk_id, CLOCK_MONOTONIC);
}

static const struct wp_presentation_listener presentation_listener = {
	presentation_clock_id
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry,
					 id, &wl_compositor_interface, version);
	} else if (strcmp(interface, "wl_subcompositor") == 0) {
		d->subcompositor =
			wl_registry_bind(registry,
					 id, &wl_subcompositor_interface, 1);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		d->wm_base = wl_registry_bind(registry,
					 id, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(d->wm_base, &xdg_wm_base_listener, d);
	} else if (strcmp(interface, "wl_seat") == 0) {
		d->seat = wl_registry_bind(registry, id,
					   &wl_seat_interface, 1);
		wl_seat_add_listener(d->seat, &seat_listener, d);
	} else if (strcmp(interface, "wl_output") == 0) {
		d->output = wl_registry_bind(registry, id,
						&wl_output_interface, version);
		wl_output_add_listener(d->output, &output_listener, d);
	} else if (strcmp(interface, "wp_presentation") == 0) {
		d->presentation = wl_registry_bind(registry, id,
					   &wp_presentation_interface, 1);
		wp_presentation_add_listener(d->presentation,
					     &presentation_listener, d);
	} else if ((d->gtype == GRALLOC_ANDROID) &&
			   (strcmp(interface, "android_wlegl") == 0)) {
		d->android_wlegl = wl_registry_bind(registry, id,
						&android_wlegl_interface, 1);
	} else if ((d->gtype == GRALLOC_GBM) &&
			   (strcmp(interface, "zwp_linux_dmabuf_v1") == 0)) {
		if (version < 3)
			return;
		d->dmabuf = wl_registry_bind(registry, id,
				&zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener, d);
    }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

int
get_gralloc_type(const char *gralloc)
{
    if (strcmp(gralloc, "default") == 0) {
        return GRALLOC_DEFAULT;
    } else if (strcmp(gralloc, "gbm") == 0) {
        return GRALLOC_GBM;
    } else {
        return GRALLOC_ANDROID;
    }
}

struct display *
create_display(const char *gralloc)
{
	struct display *display;

	display = calloc(1, sizeof *display);
	if (display == NULL) {
		fprintf(stderr, "out of memory\n");
		return NULL;
	}
	display->gtype = get_gralloc_type(gralloc);
	display->display = wl_display_connect(NULL);
	assert(display->display);

	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry,
				 &registry_listener, display);
	wl_display_roundtrip(display->display);

	return display;
}

void
destroy_display(struct display *display)
{
	if (display->wm_base)
		xdg_wm_base_destroy(display->wm_base);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);
	free(display);
}
