#define _GNU_SOURCE
#include "hrdp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ext-data-control-v1-client-protocol.h"

#define IFACE_CLIPBOARD "org.freedesktop.impl.portal.Clipboard"

/* A NULL-terminated list of mime types. */
struct mimes {
	char **v;
	size_t n;
};

/* What the compositor says is on the clipboard. */
struct offer {
	struct ext_data_control_offer_v1 *obj;
	struct mimes mimes;
};

/* A paste of a session's selection, waiting for SelectionWrite. */
struct transfer {
	struct wl_list link;
	struct session *session;
	uint32_t serial;
	int fd; /* write end of the pasting client's pipe */
};

static struct {
	struct ext_data_control_device_v1 *device;
	struct offer *selection; /* NULL when the clipboard is empty */
	/* The session that owns the current selection, if one does. */
	struct session *selection_owner;

	/* Our source, set by SetSelection on behalf of `owner`. */
	struct ext_data_control_source_v1 *source;
	struct session *owner;
	struct mimes source_mimes;
	bool expect_own; /* the next matching selection event is our source */

	struct wl_list transfers; /* struct transfer.link */
	uint32_t next_serial;
} cb;

/* ---- mime lists ------------------------------------------------------- */

static void mimes_add(struct mimes *l, const char *mime)
{
	char **v = realloc(l->v, (l->n + 2) * sizeof(*v));
	if (!v)
		return;
	l->v = v;
	if (!(v[l->n] = strdup(mime)))
		return;
	v[++l->n] = NULL;
}

static void mimes_clear(struct mimes *l)
{
	for (size_t i = 0; i < l->n; i++)
		free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->n = 0;
}

static bool mimes_has(const struct mimes *l, const char *mime)
{
	for (size_t i = 0; i < l->n; i++)
		if (strcmp(l->v[i], mime) == 0)
			return true;
	return false;
}

static bool mimes_equal(const struct mimes *a, const struct mimes *b)
{
	if (a->n != b->n)
		return false;
	for (size_t i = 0; i < a->n; i++)
		if (!mimes_has(b, a->v[i]))
			return false;
	return true;
}

static void strv_free(char **v)
{
	for (char **p = v; p && *p; p++)
		free(*p);
	free(v);
}

/* ---- signals ---------------------------------------------------------- */

static void emit_owner_changed(struct session *s)
{
	static char *none[] = {NULL};
	sd_bus_message *m = NULL;
	char **mimes = cb.selection && cb.selection->mimes.v ? cb.selection->mimes.v : none;
	int r;

	if ((r = sd_bus_message_new_signal(server.bus, &m, PORTAL_PATH, IFACE_CLIPBOARD, "SelectionOwnerChanged")) < 0 ||
	    (r = sd_bus_message_append(m, "o", s->handle)) < 0 ||
	    (r = sd_bus_message_open_container(m, 'a', "{sv}")) < 0 ||
	    (r = sd_bus_message_append(m, "{sv}", "session_is_owner", "b", s == cb.selection_owner)) < 0 ||
	    (r = sd_bus_message_open_container(m, 'e', "sv")) < 0 ||
	    (r = sd_bus_message_append(m, "s", "mime_types")) < 0 ||
	    (r = sd_bus_message_open_container(m, 'v', "as")) < 0 ||
	    (r = sd_bus_message_append_strv(m, mimes)) < 0 ||
	    (r = sd_bus_message_close_container(m)) < 0 ||
	    (r = sd_bus_message_close_container(m)) < 0 ||
	    (r = sd_bus_message_close_container(m)) < 0 ||
	    (r = sd_bus_send(server.bus, m, NULL)) < 0)
		log_warn("%s: cannot emit SelectionOwnerChanged: %s", s->handle, strerror(-r));
	sd_bus_message_unref(m);
}

static void emit_owner_changed_all(void)
{
	struct session *s;
	wl_list_for_each(s, &server.sessions, link)
		if (s->clipboard_enabled)
			emit_owner_changed(s);
}

/* ---- transfers -------------------------------------------------------- */

static void transfer_free(struct transfer *t)
{
	if (t->fd >= 0)
		close(t->fd);
	wl_list_remove(&t->link);
	free(t);
}

static struct transfer *transfer_find(struct session *s, uint32_t serial)
{
	struct transfer *t;
	wl_list_for_each(t, &cb.transfers, link)
		if (t->session == s && t->serial == serial)
			return t;
	return NULL;
}

/* ---- our source ------------------------------------------------------- */

static void drop_source(void)
{
	if (cb.source)
		ext_data_control_source_v1_destroy(cb.source);
	cb.source = NULL;
	cb.owner = NULL;
	cb.expect_own = false;
	mimes_clear(&cb.source_mimes);
}

static void source_send(void *data, struct ext_data_control_source_v1 *src, const char *mime, int32_t fd)
{
	if (src != cb.source || !cb.owner || !cb.owner->clipboard_enabled) {
		close(fd);
		return;
	}
	struct transfer *t = calloc(1, sizeof(*t));
	if (!t) {
		close(fd);
		return;
	}
	t->session = cb.owner;
	t->serial = ++cb.next_serial;
	t->fd = fd;
	wl_list_insert(&cb.transfers, &t->link);

	log_debug("%s: paste of '%s', serial %u", t->session->handle, mime, t->serial);
	int r = sd_bus_emit_signal(server.bus, PORTAL_PATH, IFACE_CLIPBOARD, "SelectionTransfer", "osu",
				   t->session->handle, mime, t->serial);
	if (r < 0) {
		log_warn("%s: cannot emit SelectionTransfer: %s", t->session->handle, strerror(-r));
		transfer_free(t);
	}
}

static void source_cancelled(void *data, struct ext_data_control_source_v1 *src)
{
	if (src == cb.source)
		drop_source();
	else
		ext_data_control_source_v1_destroy(src);
}

static const struct ext_data_control_source_v1_listener source_listener = {
	.send = source_send,
	.cancelled = source_cancelled,
};

/* ---- the compositor's selection --------------------------------------- */

static void offer_destroy(struct offer *o)
{
	if (!o)
		return;
	ext_data_control_offer_v1_destroy(o->obj);
	mimes_clear(&o->mimes);
	free(o);
}

static void offer_offer(void *data, struct ext_data_control_offer_v1 *obj, const char *mime)
{
	mimes_add(&((struct offer *)data)->mimes, mime);
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
	.offer = offer_offer,
};

static void device_data_offer(void *data, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *obj)
{
	struct offer *o = calloc(1, sizeof(*o));
	if (!o) {
		ext_data_control_offer_v1_destroy(obj);
		return;
	}
	o->obj = obj;
	ext_data_control_offer_v1_add_listener(obj, &offer_listener, o);
}

static void device_selection(void *data, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *obj)
{
	offer_destroy(cb.selection);
	cb.selection = obj ? ext_data_control_offer_v1_get_user_data(obj) : NULL;

	/* Our own source comes back as an offer like anyone else's. */
	cb.selection_owner = NULL;
	if (cb.selection && cb.expect_own && mimes_equal(&cb.selection->mimes, &cb.source_mimes)) {
		cb.selection_owner = cb.owner;
		cb.expect_own = false;
	}
	log_debug("clipboard changed: %zu mime types, owner %s", cb.selection ? cb.selection->mimes.n : 0,
		  cb.selection_owner ? cb.selection_owner->handle : "outside");
	emit_owner_changed_all();
}

static void device_primary_selection(void *data, struct ext_data_control_device_v1 *dev,
				     struct ext_data_control_offer_v1 *obj)
{
	if (obj)
		offer_destroy(ext_data_control_offer_v1_get_user_data(obj));
}

static void device_finished(void *data, struct ext_data_control_device_v1 *dev)
{
	log_warn("the compositor withdrew clipboard access");
	clipboard_finish();
}

static const struct ext_data_control_device_v1_listener device_listener = {
	.data_offer = device_data_offer,
	.selection = device_selection,
	.finished = device_finished,
	.primary_selection = device_primary_selection,
};

/* ---- D-Bus methods ---------------------------------------------------- */

static struct session *clipboard_session(const char *handle, sd_bus_error *err)
{
	struct session *s = session_find(handle);
	if (!s || !s->started || !s->clipboard_enabled) {
		sd_bus_error_set(err, SD_BUS_ERROR_ACCESS_DENIED, "clipboard is not enabled for this session");
		return NULL;
	}
	if (!cb.device) {
		sd_bus_error_set(err, SD_BUS_ERROR_NOT_SUPPORTED, "clipboard is unavailable");
		return NULL;
	}
	return s;
}

/* Reads an a{sv}, storing the string array under `key` in `out`. */
static int read_options_strv(sd_bus_message *m, const char *key, char ***out)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k;
		if ((r = sd_bus_message_read(m, "s", &k)) < 0)
			return r;
		if (strcmp(k, key) == 0 && sd_bus_message_verify_type(m, 'v', "as") > 0) {
			if ((r = sd_bus_message_enter_container(m, 'v', "as")) < 0 ||
			    (r = sd_bus_message_read_strv(m, out)) < 0 ||
			    (r = sd_bus_message_exit_container(m)) < 0)
				return r;
		} else if ((r = sd_bus_message_skip(m, "v")) < 0) {
			return r;
		}
		if ((r = sd_bus_message_exit_container(m)) < 0)
			return r;
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

static int method_request_clipboard(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *handle;
	int r;

	if ((r = sd_bus_message_read(m, "o", &handle)) < 0 || (r = sd_bus_message_skip(m, "a{sv}")) < 0)
		return r;
	struct session *s = session_find(handle);
	if (!s || s->started)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "no such session, or already started");
	s->clipboard_requested = true;
	log_info("%s: clipboard requested", handle);
	return sd_bus_reply_method_return(m, "");
}

static int method_set_selection(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *handle;
	char **mimes = NULL;
	int r;

	if ((r = sd_bus_message_read(m, "o", &handle)) < 0 || (r = read_options_strv(m, "mime_types", &mimes)) < 0) {
		strv_free(mimes);
		return r;
	}
	struct session *s = clipboard_session(handle, err);
	if (!s) {
		strv_free(mimes);
		return -EINVAL;
	}

	drop_source();
	if (!mimes || !*mimes) {
		ext_data_control_device_v1_set_selection(cb.device, NULL);
		log_debug("%s: cleared the clipboard", handle);
	} else {
		cb.source = ext_data_control_manager_v1_create_data_source(server.wl.data_control_manager);
		ext_data_control_source_v1_add_listener(cb.source, &source_listener, NULL);
		for (char **p = mimes; *p; p++) {
			ext_data_control_source_v1_offer(cb.source, *p);
			mimes_add(&cb.source_mimes, *p);
		}
		ext_data_control_device_v1_set_selection(cb.device, cb.source);
		cb.owner = s;
		cb.expect_own = true;
		log_debug("%s: took the clipboard with %zu mime types", handle, cb.source_mimes.n);
	}
	strv_free(mimes);
	return sd_bus_reply_method_return(m, "");
}

static int method_selection_write(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *handle;
	uint32_t serial;
	int r;

	if ((r = sd_bus_message_read(m, "ou", &handle, &serial)) < 0)
		return r;
	struct session *s = clipboard_session(handle, err);
	if (!s)
		return -EINVAL;
	struct transfer *t = transfer_find(s, serial);
	if (!t)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "no such transfer");

	r = sd_bus_reply_method_return(m, "h", t->fd); /* sd-bus duplicates the fd */
	/* Drop our copy so the pasting client sees EOF once the session is done. */
	transfer_free(t);
	return r;
}

static int method_selection_write_done(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *handle;
	uint32_t serial;
	int success, r;

	if ((r = sd_bus_message_read(m, "oub", &handle, &serial, &success)) < 0)
		return r;
	struct session *s = clipboard_session(handle, err);
	if (!s)
		return -EINVAL;
	struct transfer *t = transfer_find(s, serial);
	if (t) /* the session never asked for the fd */
		transfer_free(t);
	log_debug("%s: paste serial %u %s", handle, serial, success ? "done" : "failed");
	return sd_bus_reply_method_return(m, "");
}

static int method_selection_read(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *handle, *mime;
	int p[2], r;

	if ((r = sd_bus_message_read(m, "os", &handle, &mime)) < 0)
		return r;
	struct session *s = clipboard_session(handle, err);
	if (!s)
		return -EINVAL;
	if (!cb.selection || !mimes_has(&cb.selection->mimes, mime))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "the clipboard has no '%s'", mime);
	if (pipe2(p, O_CLOEXEC) < 0)
		return sd_bus_error_set_errno(err, errno);

	ext_data_control_offer_v1_receive(cb.selection->obj, mime, p[1]); /* duplicated while marshalling */
	close(p[1]);
	wl_display_flush(server.wl.display);
	r = sd_bus_reply_method_return(m, "h", p[0]);
	close(p[0]);
	log_debug("%s: reading '%s' from the clipboard", handle, mime);
	return r;
}

static int prop_version(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
			void *data, sd_bus_error *err)
{
	return sd_bus_message_append(reply, "u", 1);
}

static const sd_bus_vtable clipboard_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RequestClipboard", "oa{sv}", "", method_request_clipboard, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SetSelection", "oa{sv}", "", method_set_selection, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectionWrite", "ou", "h", method_selection_write, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectionWriteDone", "oub", "", method_selection_write_done, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectionRead", "os", "h", method_selection_read, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("SelectionOwnerChanged", "oa{sv}", 0),
	SD_BUS_SIGNAL("SelectionTransfer", "osu", 0),
	SD_BUS_PROPERTY("version", "u", prop_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

/* ---- lifecycle -------------------------------------------------------- */

int clipboard_init(void)
{
	wl_list_init(&cb.transfers);
	if (server.wl.data_control_manager) {
		cb.device = ext_data_control_manager_v1_get_data_device(server.wl.data_control_manager, server.wl.seat);
		ext_data_control_device_v1_add_listener(cb.device, &device_listener, NULL);
	}
	int r = sd_bus_add_object_vtable(server.bus, NULL, PORTAL_PATH, IFACE_CLIPBOARD, clipboard_vtable, NULL);
	if (r < 0)
		log_error("cannot export %s: %s", IFACE_CLIPBOARD, strerror(-r));
	return r;
}

void clipboard_finish(void)
{
	struct transfer *t, *tmp;

	if (!cb.transfers.next)
		return; /* never initialised */
	wl_list_for_each_safe(t, tmp, &cb.transfers, link)
		transfer_free(t);
	drop_source();
	offer_destroy(cb.selection);
	cb.selection = NULL;
	cb.selection_owner = NULL;
	if (cb.device)
		ext_data_control_device_v1_destroy(cb.device);
	cb.device = NULL;
}

bool clipboard_available(void)
{
	return cb.device != NULL;
}

/* Tells a newly started session what is on the clipboard. */
void clipboard_session_started(struct session *s)
{
	if (s->clipboard_enabled)
		emit_owner_changed(s);
}

void clipboard_session_closed(struct session *s)
{
	struct transfer *t, *tmp;

	wl_list_for_each_safe(t, tmp, &cb.transfers, link)
		if (t->session == s)
			transfer_free(t);
	if (cb.selection_owner == s)
		cb.selection_owner = NULL;
	/* Nobody is left to paste from; the compositor clears the clipboard. */
	if (cb.owner == s)
		drop_source();
}
