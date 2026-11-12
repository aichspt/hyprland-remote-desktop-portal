#define _GNU_SOURCE
#include "hrdp.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ext-data-control-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

/* Wheel distance libinput reports for one detent; clients expect it. */
#define WHEEL_STEP 15.0

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- outputs ---------------------------------------------------------- */

static void xdg_output_logical_position(void *data, struct zxdg_output_v1 *xo, int32_t x, int32_t y)
{
	struct output *o = data;
	o->x = x;
	o->y = y;
}

static void xdg_output_logical_size(void *data, struct zxdg_output_v1 *xo, int32_t w, int32_t h)
{
	struct output *o = data;
	o->width = w;
	o->height = h;
}

/* xdg-output v3 deprecates its own done event in favour of wl_output.done,
 * so both land here. */
static void output_commit(struct output *o)
{
	o->has_geometry = o->width > 0 && o->height > 0;
	server.wl.layout_changed = true;
	log_debug("output %s: %dx%d at %d,%d", o->name ? o->name : "?", o->width, o->height, o->x, o->y);
}

static void xdg_output_done(void *data, struct zxdg_output_v1 *xo)
{
	output_commit(data);
}

static void xdg_output_name(void *data, struct zxdg_output_v1 *xo, const char *name)
{
	struct output *o = data;
	free(o->name);
	o->name = strdup(name);
}

static void xdg_output_description(void *data, struct zxdg_output_v1 *xo, const char *desc)
{
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_logical_position,
	.logical_size = xdg_output_logical_size,
	.done = xdg_output_done,
	.name = xdg_output_name,
	.description = xdg_output_description,
};

static void wl_output_geometry(void *data, struct wl_output *wo, int32_t x, int32_t y, int32_t pw, int32_t ph,
			       int32_t subpixel, const char *make, const char *model, int32_t transform)
{
}

static void wl_output_mode(void *data, struct wl_output *wo, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	struct output *o = data;
	if (flags & WL_OUTPUT_MODE_CURRENT)
		o->mode_width = w;
}

static void wl_output_done(void *data, struct wl_output *wo)
{
	output_commit(data);
}

static void wl_output_scale(void *data, struct wl_output *wo, int32_t factor)
{
}

static void wl_output_name(void *data, struct wl_output *wo, const char *name)
{
}

static void wl_output_description(void *data, struct wl_output *wo, const char *desc)
{
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = wl_output_geometry,
	.mode = wl_output_mode,
	.done = wl_output_done,
	.scale = wl_output_scale,
	.name = wl_output_name,
	.description = wl_output_description,
};

static void output_add_xdg(struct wl_state *wl, struct output *o)
{
	if (o->xdg_output || !wl->xdg_output_manager)
		return;
	o->xdg_output = zxdg_output_manager_v1_get_xdg_output(wl->xdg_output_manager, o->wl_output);
	zxdg_output_v1_add_listener(o->xdg_output, &xdg_output_listener, o);
}

static void output_destroy(struct output *o)
{
	if (o->xdg_output)
		zxdg_output_v1_destroy(o->xdg_output);
	wl_output_release(o->wl_output);
	wl_list_remove(&o->link);
	free(o->name);
	free(o);
}

struct rect wl_layout_bounds(struct wl_state *wl)
{
	struct rect r = {0};
	bool first = true;
	int32_t x2 = 0, y2 = 0;
	struct output *o;

	wl_list_for_each(o, &wl->outputs, link) {
		if (!o->has_geometry)
			continue;
		if (first) {
			r.x = o->x;
			r.y = o->y;
			x2 = o->x + o->width;
			y2 = o->y + o->height;
			first = false;
			continue;
		}
		if (o->x < r.x)
			r.x = o->x;
		if (o->y < r.y)
			r.y = o->y;
		if (o->x + o->width > x2)
			x2 = o->x + o->width;
		if (o->y + o->height > y2)
			y2 = o->y + o->height;
	}
	r.width = x2 - r.x;
	r.height = y2 - r.y;
	return r;
}

/* ---- registry --------------------------------------------------------- */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	struct wl_state *wl = data;

	if (strcmp(iface, wl_seat_interface.name) == 0 && !wl->seat) {
		wl->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
	} else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		wl->pointer_manager = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	} else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		wl->keyboard_manager = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (strcmp(iface, ext_data_control_manager_v1_interface.name) == 0) {
		wl->data_control_manager = wl_registry_bind(reg, name, &ext_data_control_manager_v1_interface, 1);
	} else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
		wl->xdg_output_manager =
			wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, version < 3 ? version : 3);
		struct output *o;
		wl_list_for_each(o, &wl->outputs, link)
			output_add_xdg(wl, o);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		if (version < 3)
			return; /* need wl_output.release */
		struct output *o = calloc(1, sizeof(*o));
		if (!o)
			return;
		o->global_name = name;
		o->wl_output = wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(o->wl_output, &wl_output_listener, o);
		wl_list_insert(wl->outputs.prev, &o->link);
		output_add_xdg(wl, o);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	struct wl_state *wl = data;
	struct output *o, *tmp;

	wl_list_for_each_safe(o, tmp, &wl->outputs, link) {
		if (o->global_name == name) {
			log_debug("output %s removed", o->name ? o->name : "?");
			output_destroy(o);
			wl->layout_changed = true;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int wl_init(struct wl_state *wl)
{
	wl_list_init(&wl->outputs);
	wl->display = wl_display_connect(NULL);
	if (!wl->display) {
		log_error("cannot connect to the Wayland display (is WAYLAND_DISPLAY set?)");
		return -1;
	}
	wl->registry = wl_display_get_registry(wl->display);
	wl_registry_add_listener(wl->registry, &registry_listener, wl);
	/* globals, then the output events they trigger */
	wl_display_roundtrip(wl->display);
	wl_display_roundtrip(wl->display);

	if (!wl->seat || !wl->pointer_manager || !wl->keyboard_manager) {
		log_error("compositor lacks wl_seat, zwlr_virtual_pointer_manager_v1 or zwp_virtual_keyboard_manager_v1");
		return -1;
	}
	if (!wl->xdg_output_manager)
		log_warn("compositor lacks xdg-output; absolute pointer motion is unavailable");
	if (!wl->data_control_manager)
		log_warn("compositor lacks ext-data-control; the clipboard is unavailable");
	wl->layout_changed = false;
	return 0;
}

void wl_finish(struct wl_state *wl)
{
	struct output *o, *tmp;

	if (!wl->display)
		return;
	wl_list_for_each_safe(o, tmp, &wl->outputs, link)
		output_destroy(o);
	if (wl->xdg_output_manager)
		zxdg_output_manager_v1_destroy(wl->xdg_output_manager);
	if (wl->data_control_manager)
		ext_data_control_manager_v1_destroy(wl->data_control_manager);
	if (wl->pointer_manager)
		zwlr_virtual_pointer_manager_v1_destroy(wl->pointer_manager);
	if (wl->keyboard_manager)
		zwp_virtual_keyboard_manager_v1_destroy(wl->keyboard_manager);
	if (wl->seat)
		wl_seat_destroy(wl->seat);
	wl_registry_destroy(wl->registry);
	wl_display_flush(wl->display);
	wl_display_disconnect(wl->display);
	wl->display = NULL;
}

/* ---- virtual devices -------------------------------------------------- */

int vinput_create(struct vinput *vi, struct wl_state *wl, uint32_t devices, struct xkb_keymap *keymap)
{
	memset(vi, 0, sizeof(*vi));

	if (devices & DEVICE_POINTER)
		vi->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(wl->pointer_manager, wl->seat);

	if (devices & DEVICE_KEYBOARD) {
		size_t size;
		int fd = keymap_to_fd(keymap, &size);
		if (fd < 0)
			return -1;
		vi->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(wl->keyboard_manager, wl->seat);
		/* wayland-client duplicates the fd while marshalling */
		zwp_virtual_keyboard_v1_keymap(vi->keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
		close(fd);
		vi->keymap = xkb_keymap_ref(keymap);
		vi->xkb_state = xkb_state_new(keymap);
	}
	wl_display_flush(wl->display);
	return 0;
}

void vinput_release_keys(struct vinput *vi)
{
	if (!vi->keyboard)
		return;
	for (uint32_t code = 0; code < KEY_COUNT; code++)
		if (vi->keys_down[code])
			vinput_key(vi, code, false);
}

void vinput_release_buttons(struct vinput *vi)
{
	if (!vi->pointer || !vi->n_buttons_down)
		return;
	while (vi->n_buttons_down)
		vinput_button(vi, vi->buttons_down[vi->n_buttons_down - 1], false);
	vinput_frame(vi);
}

void vinput_release_all(struct vinput *vi)
{
	vinput_release_keys(vi);
	vinput_release_buttons(vi);
}

void vinput_destroy(struct vinput *vi)
{
	vinput_release_all(vi);
	if (vi->pointer)
		zwlr_virtual_pointer_v1_destroy(vi->pointer);
	if (vi->keyboard)
		zwp_virtual_keyboard_v1_destroy(vi->keyboard);
	if (vi->xkb_state)
		xkb_state_unref(vi->xkb_state);
	if (vi->keymap)
		xkb_keymap_unref(vi->keymap);
	memset(vi, 0, sizeof(*vi));
}

void vinput_motion(struct vinput *vi, double dx, double dy)
{
	if (!vi->pointer)
		return;
	zwlr_virtual_pointer_v1_motion(vi->pointer, now_ms(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	vi->pointer_dirty = true;
}

/* (x, y) are logical coordinates relative to the top-left of the layout. */
void vinput_motion_absolute(struct vinput *vi, struct wl_state *wl, double x, double y)
{
	struct rect b = wl_layout_bounds(wl);

	if (!vi->pointer || b.width <= 0 || b.height <= 0)
		return;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x > b.width - 1)
		x = b.width - 1;
	if (y > b.height - 1)
		y = b.height - 1;
	/* the protocol takes integers; scale up to keep sub-pixel precision */
	const double k = 16.0;
	zwlr_virtual_pointer_v1_motion_absolute(vi->pointer, now_ms(), (uint32_t)(x * k), (uint32_t)(y * k),
						(uint32_t)(b.width * k), (uint32_t)(b.height * k));
	vi->pointer_dirty = true;
}

void vinput_button(struct vinput *vi, uint32_t button, bool pressed)
{
	size_t i;

	if (!vi->pointer)
		return;
	for (i = 0; i < vi->n_buttons_down; i++)
		if (vi->buttons_down[i] == button)
			break;
	bool is_down = i < vi->n_buttons_down;
	if (pressed == is_down)
		return; /* drop duplicate presses/releases */
	if (pressed) {
		if (vi->n_buttons_down == MAX_BUTTONS)
			return;
		vi->buttons_down[vi->n_buttons_down++] = button;
	} else {
		vi->buttons_down[i] = vi->buttons_down[--vi->n_buttons_down];
	}
	zwlr_virtual_pointer_v1_button(vi->pointer, now_ms(), button,
				       pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	vi->pointer_dirty = true;
}

void vinput_scroll(struct vinput *vi, double dx, double dy)
{
	if (!vi->pointer)
		return;
	uint32_t t = now_ms();
	zwlr_virtual_pointer_v1_axis_source(vi->pointer, WL_POINTER_AXIS_SOURCE_FINGER);
	if (dy != 0)
		zwlr_virtual_pointer_v1_axis(vi->pointer, t, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
	if (dx != 0)
		zwlr_virtual_pointer_v1_axis(vi->pointer, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
	vi->pointer_dirty = true;
}

static void scroll_discrete_axis(struct vinput *vi, uint32_t t, uint32_t axis, int32_t v120)
{
	if (v120 == 0)
		return;
	double value = WHEEL_STEP * v120 / 120.0;
	int32_t steps = v120 / 120;
	if (steps != 0)
		zwlr_virtual_pointer_v1_axis_discrete(vi->pointer, t, axis, wl_fixed_from_double(value), steps);
	else /* high-resolution wheel: a fraction of a detent */
		zwlr_virtual_pointer_v1_axis(vi->pointer, t, axis, wl_fixed_from_double(value));
}

void vinput_scroll_discrete(struct vinput *vi, int32_t dx120, int32_t dy120)
{
	if (!vi->pointer)
		return;
	uint32_t t = now_ms();
	zwlr_virtual_pointer_v1_axis_source(vi->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
	scroll_discrete_axis(vi, t, WL_POINTER_AXIS_VERTICAL_SCROLL, dy120);
	scroll_discrete_axis(vi, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx120);
	vi->pointer_dirty = true;
}

void vinput_scroll_stop(struct vinput *vi, bool stop_x, bool stop_y)
{
	if (!vi->pointer)
		return;
	uint32_t t = now_ms();
	zwlr_virtual_pointer_v1_axis_source(vi->pointer, WL_POINTER_AXIS_SOURCE_FINGER);
	if (stop_y)
		zwlr_virtual_pointer_v1_axis_stop(vi->pointer, t, WL_POINTER_AXIS_VERTICAL_SCROLL);
	if (stop_x)
		zwlr_virtual_pointer_v1_axis_stop(vi->pointer, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
	vi->pointer_dirty = true;
}

void vinput_frame(struct vinput *vi)
{
	if (!vi->pointer || !vi->pointer_dirty)
		return;
	zwlr_virtual_pointer_v1_frame(vi->pointer);
	vi->pointer_dirty = false;
}

static void send_modifiers(struct vinput *vi)
{
	uint32_t dep = xkb_state_serialize_mods(vi->xkb_state, XKB_STATE_MODS_DEPRESSED);
	uint32_t lat = xkb_state_serialize_mods(vi->xkb_state, XKB_STATE_MODS_LATCHED);
	uint32_t lck = xkb_state_serialize_mods(vi->xkb_state, XKB_STATE_MODS_LOCKED);
	uint32_t grp = xkb_state_serialize_layout(vi->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);

	if (dep == vi->mods_depressed && lat == vi->mods_latched && lck == vi->mods_locked && grp == vi->group)
		return;
	vi->mods_depressed = dep;
	vi->mods_latched = lat;
	vi->mods_locked = lck;
	vi->group = grp;
	zwp_virtual_keyboard_v1_modifiers(vi->keyboard, dep, lat, lck, grp);
}

void vinput_key(struct vinput *vi, uint32_t code, bool pressed)
{
	if (!vi->keyboard || code >= KEY_COUNT)
		return;
	if (vi->keys_down[code] == pressed)
		return; /* drop duplicate presses/releases */
	vi->keys_down[code] = pressed;
	zwp_virtual_keyboard_v1_key(vi->keyboard, now_ms(), code,
				    pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
	xkb_state_update_key(vi->xkb_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	send_modifiers(vi);
}

/* Finds a keycode producing keysym in the first layout, at level 0 or 1. */
static bool keysym_lookup(struct xkb_keymap *km, xkb_keysym_t sym, xkb_keycode_t *out_code, xkb_level_index_t *out_level)
{
	xkb_keycode_t min = xkb_keymap_min_keycode(km), max = xkb_keymap_max_keycode(km);

	for (xkb_level_index_t level = 0; level < 2; level++) {
		for (xkb_keycode_t kc = min; kc <= max; kc++) {
			const xkb_keysym_t *syms;
			int n = xkb_keymap_key_get_syms_by_level(km, kc, 0, level, &syms);
			for (int i = 0; i < n; i++) {
				if (syms[i] == sym) {
					*out_code = kc;
					*out_level = level;
					return true;
				}
			}
		}
	}
	return false;
}

bool vinput_keysym(struct vinput *vi, xkb_keysym_t sym, bool pressed)
{
	xkb_keycode_t kc;
	xkb_level_index_t level;
	const uint32_t shift = 42; /* KEY_LEFTSHIFT */

	if (!vi->keyboard || !keysym_lookup(vi->keymap, sym, &kc, &level))
		return false;
	bool need_shift = level == 1 && !vi->keys_down[shift];
	if (need_shift && pressed)
		vinput_key(vi, shift, true);
	vinput_key(vi, kc - 8, pressed);
	if (need_shift && pressed)
		vinput_key(vi, shift, false);
	return true;
}
