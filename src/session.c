#define _GNU_SOURCE
#include "hrdp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const enum eis_device_capability all_caps[] = {
	EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_BUTTON,
	EIS_DEVICE_CAP_SCROLL,  EIS_DEVICE_CAP_KEYBOARD,
};

struct session *session_find(const char *handle)
{
	struct session *s;
	wl_list_for_each(s, &server.sessions, link)
		if (strcmp(s->handle, handle) == 0)
			return s;
	return NULL;
}

/* ---- EIS devices ------------------------------------------------------ */

static bool bound(struct session *s, enum eis_device_capability cap)
{
	return s->bound_caps & cap;
}

static struct eis_device *device_new(struct session *s, const char *name)
{
	struct eis_device *d = eis_seat_new_device(s->seat);
	eis_device_configure_name(d, name);
	eis_device_configure_type(d, EIS_DEVICE_TYPE_VIRTUAL);
	return d;
}

static void device_commit(struct eis_device *d)
{
	eis_device_add(d);
	eis_device_resume(d);
}

static void device_drop(struct eis_device **d)
{
	if (!*d)
		return;
	eis_device_remove(*d);
	eis_device_unref(*d);
	*d = NULL;
}

static void add_pointer_caps(struct session *s, struct eis_device *d)
{
	if (bound(s, EIS_DEVICE_CAP_BUTTON))
		eis_device_configure_capability(d, EIS_DEVICE_CAP_BUTTON);
	if (bound(s, EIS_DEVICE_CAP_SCROLL))
		eis_device_configure_capability(d, EIS_DEVICE_CAP_SCROLL);
}

static void create_pointer(struct session *s)
{
	struct eis_device *d = device_new(s, "Hyprland remote pointer");
	eis_device_configure_capability(d, EIS_DEVICE_CAP_POINTER);
	add_pointer_caps(s, d);
	device_commit(d);
	s->dev_pointer = d;
}

static void create_absolute(struct session *s)
{
	struct rect b = wl_layout_bounds(&server.wl);
	struct output *o;
	int regions = 0;

	if (b.width <= 0 || b.height <= 0) {
		log_warn("no output geometry; not offering an absolute pointer");
		return;
	}
	struct eis_device *d = device_new(s, "Hyprland remote absolute pointer");
	eis_device_configure_capability(d, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
	add_pointer_caps(s, d);

	/* Regions use the layout's top-left corner as the origin, since EIS
	 * offsets are unsigned and outputs may sit at negative positions. */
	wl_list_for_each(o, &server.wl.outputs, link) {
		if (!o->has_geometry)
			continue;
		struct eis_region *r = eis_device_new_region(d);
		eis_region_set_offset(r, (uint32_t)(o->x - b.x), (uint32_t)(o->y - b.y));
		eis_region_set_size(r, (uint32_t)o->width, (uint32_t)o->height);
		if (o->mode_width > 0)
			eis_region_set_physical_scale(r, (double)o->mode_width / o->width);
		if (o->name)
			eis_region_set_mapping_id(r, o->name);
		eis_region_add(r);
		eis_region_unref(r);
		regions++;
		log_debug("region %s: %dx%d at %d,%d", o->name ? o->name : "?", o->width, o->height, o->x - b.x,
			  o->y - b.y);
	}
	device_commit(d);
	s->dev_absolute = d;
	log_info("%s: absolute pointer with %d region(s), layout %dx%d", s->handle, regions, b.width, b.height);
}

static void create_keyboard(struct session *s)
{
	size_t size;
	int fd = keymap_to_fd(s->vi.keymap, &size);
	if (fd < 0)
		return;

	struct eis_device *d = device_new(s, "Hyprland remote keyboard");
	eis_device_configure_capability(d, EIS_DEVICE_CAP_KEYBOARD);
	struct eis_keymap *km = eis_device_new_keymap(d, EIS_KEYMAP_TYPE_XKB, fd, size);
	close(fd);
	if (km) {
		eis_keymap_add(km);
		eis_keymap_unref(km);
	}
	device_commit(d);
	s->dev_keyboard = d;
}

static void sync_devices(struct session *s)
{
	bool want_pointer = bound(s, EIS_DEVICE_CAP_POINTER) && s->vi.pointer;
	bool want_absolute = bound(s, EIS_DEVICE_CAP_POINTER_ABSOLUTE) && s->vi.pointer;
	bool want_keyboard = bound(s, EIS_DEVICE_CAP_KEYBOARD) && s->vi.keyboard;

	if (!want_pointer)
		device_drop(&s->dev_pointer);
	else if (!s->dev_pointer)
		create_pointer(s);

	if (!want_absolute)
		device_drop(&s->dev_absolute);
	else if (!s->dev_absolute)
		create_absolute(s);

	if (!want_keyboard)
		device_drop(&s->dev_keyboard);
	else if (!s->dev_keyboard)
		create_keyboard(s);
}

void sessions_layout_changed(void)
{
	struct session *s;

	wl_list_for_each(s, &server.sessions, link) {
		if (!s->dev_absolute)
			continue;
		log_info("%s: output layout changed, updating regions", s->handle);
		device_drop(&s->dev_absolute);
		create_absolute(s);
	}
}

/* ---- EIS client ------------------------------------------------------- */

static void client_drop(struct session *s)
{
	vinput_release_all(&s->vi);
	device_drop(&s->dev_pointer);
	device_drop(&s->dev_absolute);
	device_drop(&s->dev_keyboard);
	if (s->seat) {
		eis_seat_unref(s->seat);
		s->seat = NULL;
	}
	if (s->client) {
		eis_client_unref(s->client);
		s->client = NULL;
	}
	s->bound_caps = 0;
}

static void client_connect(struct session *s, struct eis_client *client)
{
	if (s->client || !eis_client_is_sender(client)) {
		log_warn("%s: rejecting EIS client '%s'", s->handle, eis_client_get_name(client));
		eis_client_disconnect(client);
		return;
	}
	log_info("%s: EIS client '%s' connected", s->handle, eis_client_get_name(client));
	s->client = eis_client_ref(client);
	eis_client_connect(client);

	s->seat = eis_client_new_seat(client, "hyprland");
	if (s->vi.pointer) {
		eis_seat_configure_capability(s->seat, EIS_DEVICE_CAP_POINTER);
		eis_seat_configure_capability(s->seat, EIS_DEVICE_CAP_BUTTON);
		eis_seat_configure_capability(s->seat, EIS_DEVICE_CAP_SCROLL);
		if (server.wl.xdg_output_manager)
			eis_seat_configure_capability(s->seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
	}
	if (s->vi.keyboard)
		eis_seat_configure_capability(s->seat, EIS_DEVICE_CAP_KEYBOARD);
	eis_seat_add(s->seat);
}

static void handle_device_closed(struct session *s, struct eis_device *d)
{
	struct eis_device **slots[] = {&s->dev_pointer, &s->dev_absolute, &s->dev_keyboard};
	for (size_t i = 0; i < 3; i++) {
		if (*slots[i] == d) {
			if (d == s->dev_keyboard)
				vinput_release_keys(&s->vi);
			else
				vinput_release_buttons(&s->vi);
			device_drop(slots[i]);
		}
	}
}

static void handle_event(struct session *s, struct eis_event *e)
{
	struct vinput *vi = &s->vi;
	struct eis_device *dev = eis_event_get_device(e);

	switch (eis_event_get_type(e)) {
	case EIS_EVENT_CLIENT_CONNECT:
		client_connect(s, eis_event_get_client(e));
		break;
	case EIS_EVENT_CLIENT_DISCONNECT:
		if (eis_event_get_client(e) == s->client) {
			log_info("%s: EIS client disconnected", s->handle);
			client_drop(s);
		}
		break;
	case EIS_EVENT_SEAT_BIND:
		s->bound_caps = 0;
		for (size_t i = 0; i < sizeof(all_caps) / sizeof(all_caps[0]); i++)
			if (eis_event_seat_has_capability(e, all_caps[i]))
				s->bound_caps |= all_caps[i];
		log_debug("%s: seat bound, caps 0x%x", s->handle, s->bound_caps);
		/* button/scroll caps changing means rebuilding pointer devices */
		device_drop(&s->dev_pointer);
		device_drop(&s->dev_absolute);
		sync_devices(s);
		break;
	case EIS_EVENT_DEVICE_CLOSED:
		handle_device_closed(s, dev);
		break;
	case EIS_EVENT_DEVICE_START_EMULATING:
		break;
	case EIS_EVENT_DEVICE_STOP_EMULATING:
		if (dev == s->dev_keyboard)
			vinput_release_keys(vi);
		else
			vinput_release_buttons(vi);
		break;
	case EIS_EVENT_FRAME:
		vinput_frame(vi);
		break;
	case EIS_EVENT_POINTER_MOTION:
		vinput_motion(vi, eis_event_pointer_get_dx(e), eis_event_pointer_get_dy(e));
		break;
	case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
		vinput_motion_absolute(vi, &server.wl, eis_event_pointer_get_absolute_x(e),
				       eis_event_pointer_get_absolute_y(e));
		break;
	case EIS_EVENT_BUTTON_BUTTON:
		vinput_button(vi, eis_event_button_get_button(e), eis_event_button_get_is_press(e));
		break;
	case EIS_EVENT_SCROLL_DELTA:
		vinput_scroll(vi, eis_event_scroll_get_dx(e), eis_event_scroll_get_dy(e));
		break;
	case EIS_EVENT_SCROLL_STOP:
	case EIS_EVENT_SCROLL_CANCEL:
		vinput_scroll_stop(vi, eis_event_scroll_get_stop_x(e), eis_event_scroll_get_stop_y(e));
		break;
	case EIS_EVENT_SCROLL_DISCRETE:
		vinput_scroll_discrete(vi, eis_event_scroll_get_discrete_dx(e), eis_event_scroll_get_discrete_dy(e));
		break;
	case EIS_EVENT_KEYBOARD_KEY:
		vinput_key(vi, eis_event_keyboard_get_key(e), eis_event_keyboard_get_key_is_press(e));
		break;
	default:
		log_debug("%s: ignoring EIS event %s", s->handle, eis_event_type_to_string(eis_event_get_type(e)));
		break;
	}
}

static int eis_io(sd_event_source *src, int fd, uint32_t revents, void *data)
{
	struct session *s = data;
	struct eis_event *e;

	eis_dispatch(s->eis);
	while ((e = eis_get_event(s->eis))) {
		handle_event(s, e);
		eis_event_unref(e);
	}
	return 0;
}

static void eis_log(struct eis *eis, enum eis_log_priority prio, const char *msg, struct eis_log_context *ctx)
{
	if (prio >= EIS_LOG_PRIORITY_WARNING)
		log_warn("libeis: %s", msg);
	else
		log_debug("libeis: %s", msg);
}

static void eis_teardown(struct session *s)
{
	client_drop(s);
	if (s->eis_source)
		s->eis_source = sd_event_source_disable_unref(s->eis_source);
	if (s->eis) {
		eis_unref(s->eis);
		s->eis = NULL;
	}
}

int session_connect_eis(struct session *s)
{
	/* one client per session; a new request replaces the old connection */
	eis_teardown(s);

	s->eis = eis_new(s);
	if (!s->eis)
		return -ENOMEM;
	eis_log_set_handler(s->eis, eis_log);
	eis_log_set_priority(s->eis, log_verbose ? EIS_LOG_PRIORITY_DEBUG : EIS_LOG_PRIORITY_WARNING);
	int r = eis_setup_backend_fd(s->eis);
	if (r < 0)
		goto fail;
	r = sd_event_add_io(server.event, &s->eis_source, eis_get_fd(s->eis), EPOLLIN, eis_io, s);
	if (r < 0)
		goto fail;
	int fd = eis_backend_fd_add_client(s->eis);
	if (fd < 0) {
		r = fd;
		goto fail;
	}
	return fd;

fail:
	log_error("%s: cannot set up EIS: %s", s->handle, strerror(-r));
	eis_teardown(s);
	return r;
}

/* ---- lifecycle -------------------------------------------------------- */

int session_start_input(struct session *s)
{
	struct xkb_keymap *km = NULL;

	if (s->devices & DEVICE_KEYBOARD) {
		km = keymap_compile(server.xkb, &server.cfg);
		if (!km)
			return -1;
	}
	int r = vinput_create(&s->vi, &server.wl, s->devices, km);
	if (km)
		xkb_keymap_unref(km);
	if (r < 0)
		return r;
	s->started = true;
	log_info("%s: started for app '%s' (devices 0x%x)", s->handle, s->app_id, s->devices);
	return 0;
}

void session_destroy(struct session *s, bool emit_closed)
{
	log_info("%s: closed", s->handle);
	if (s->pending_start) {
		/* response 2: the interaction ended some other way */
		sd_bus_reply_method_return(s->pending_start, "ua{sv}", 2, 0);
		s->pending_start = sd_bus_message_unref(s->pending_start);
	}
	if (s->dialog_source)
		s->dialog_source = sd_event_source_unref(s->dialog_source); /* kills the dialog */
	if (s->dialog_fd >= 0)
		close(s->dialog_fd);

	eis_teardown(s);
	vinput_destroy(&s->vi);
	if (emit_closed)
		sd_bus_emit_signal(server.bus, s->handle, "org.freedesktop.impl.portal.Session", "Closed", "");
	sd_bus_slot_unref(s->slot);
	wl_list_remove(&s->link);
	free(s->handle);
	free(s->app_id);
	free(s);
}
