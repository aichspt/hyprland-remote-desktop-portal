#define _GNU_SOURCE
#include "hrdp.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define IFACE_REMOTE "org.freedesktop.impl.portal.RemoteDesktop"
#define IFACE_SESSION "org.freedesktop.impl.portal.Session"

#define RESPONSE_OK 0
#define RESPONSE_CANCELLED 1
#define RESPONSE_OTHER 2

#define DIALOG_DENY "Deny"
#define DIALOG_ONCE "Allow once"
#define DIALOG_ALWAYS "Always allow"

/* ---- helpers ---------------------------------------------------------- */

/* Reads an a{sv}, storing the uint32 value of `key` if present. */
static int read_options_u(sd_bus_message *m, const char *key, uint32_t *out, bool *found)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k;
		if ((r = sd_bus_message_read(m, "s", &k)) < 0)
			return r;
		if (key && strcmp(k, key) == 0 && sd_bus_message_verify_type(m, 'v', "u") > 0) {
			if ((r = sd_bus_message_read(m, "v", "u", out)) < 0)
				return r;
			if (found)
				*found = true;
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

static bool read_options_bool(sd_bus_message *m, const char *key)
{
	bool value = false;
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return false;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *k;
		int b;
		if (sd_bus_message_read(m, "s", &k) < 0)
			break;
		if (strcmp(k, key) == 0 && sd_bus_message_verify_type(m, 'v', "b") > 0 &&
		    sd_bus_message_read(m, "v", "b", &b) >= 0)
			value = b;
		else
			sd_bus_message_skip(m, "v");
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	return value;
}

static int reply_start(sd_bus_message *call, struct session *s, uint32_t response)
{
	if (response == RESPONSE_OK)
		return sd_bus_reply_method_return(call, "ua{sv}", RESPONSE_OK, 2, "devices", "u", s->devices,
						  "clipboard_enabled", "b", s->clipboard_enabled);
	return sd_bus_reply_method_return(call, "ua{sv}", response, 0);
}

static struct session *started_session(const char *handle, uint32_t device, sd_bus_error *err)
{
	struct session *s = session_find(handle);
	if (!s || !s->started) {
		sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "session is not started");
		return NULL;
	}
	if (!(s->devices & device)) {
		sd_bus_error_set(err, SD_BUS_ERROR_ACCESS_DENIED, "device type not granted to this session");
		return NULL;
	}
	return s;
}

/* ---- Session object --------------------------------------------------- */

static int session_close(sd_bus_message *m, void *data, sd_bus_error *err)
{
	session_destroy(data, false);
	return sd_bus_reply_method_return(m, "");
}

static int session_prop_version(sd_bus *bus, const char *path, const char *iface, const char *prop,
				sd_bus_message *reply, void *data, sd_bus_error *err)
{
	return sd_bus_message_append(reply, "u", 1);
}

static const sd_bus_vtable session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", session_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("Closed", "", 0),
	SD_BUS_PROPERTY("version", "u", session_prop_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

/* ---- CreateSession / SelectDevices ------------------------------------ */

static int method_create_session(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *handle, *session_handle, *app_id;
	int r;

	if ((r = sd_bus_message_read(m, "oos", &handle, &session_handle, &app_id)) < 0)
		return r;
	if ((r = read_options_u(m, NULL, NULL, NULL)) < 0)
		return r;
	if (session_find(session_handle))
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "session already exists");

	struct session *s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;
	s->handle = strdup(session_handle);
	s->app_id = strdup(app_id);
	s->devices = SUPPORTED_DEVICES;
	s->dialog_fd = -1;
	r = sd_bus_add_object_vtable(server.bus, &s->slot, session_handle, IFACE_SESSION, session_vtable, s);
	if (r < 0) {
		free(s->handle);
		free(s->app_id);
		free(s);
		return r;
	}
	wl_list_insert(&server.sessions, &s->link);
	log_info("%s: created for app '%s'", session_handle, app_id);
	return sd_bus_reply_method_return(m, "ua{sv}", RESPONSE_OK, 1, "session_id", "s", session_handle);
}

static int method_select_devices(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *handle, *session_handle, *app_id;
	uint32_t types = SUPPORTED_DEVICES;
	bool found = false;
	int r;

	if ((r = sd_bus_message_read(m, "oos", &handle, &session_handle, &app_id)) < 0)
		return r;
	if ((r = read_options_u(m, "types", &types, &found)) < 0)
		return r;
	struct session *s = session_find(session_handle);
	if (!s || s->started)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "no such session, or already started");

	s->devices = (found ? types : SUPPORTED_DEVICES) & SUPPORTED_DEVICES;
	log_info("%s: devices selected 0x%x", session_handle, s->devices);
	return sd_bus_reply_method_return(m, "ua{sv}", RESPONSE_OK, 0);
}

/* ---- Start, with an optional confirmation dialog ---------------------- */

static void finish_start(struct session *s, sd_bus_message *call, bool allowed)
{
	uint32_t response = RESPONSE_CANCELLED;

	if (allowed)
		response = session_start_input(s) < 0 ? RESPONSE_OTHER : RESPONSE_OK;
	else
		log_info("%s: denied for app '%s'", s->handle, s->app_id);
	if (response == RESPONSE_OK)
		s->clipboard_enabled = s->clipboard_requested && clipboard_available();
	reply_start(call, s, response);
	if (response == RESPONSE_OK)
		clipboard_session_started(s);
}

static int dialog_exited(sd_event_source *src, const siginfo_t *si, void *data)
{
	struct session *s = data;
	char buf[128] = {0};
	ssize_t n = read(s->dialog_fd, buf, sizeof(buf) - 1);
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';
	close(s->dialog_fd);
	s->dialog_fd = -1;
	s->dialog_source = sd_event_source_unref(s->dialog_source);

	bool always = strcmp(buf, DIALOG_ALWAYS) == 0;
	bool allowed = always || strcmp(buf, DIALOG_ONCE) == 0;
	if (always)
		config_remember_app(&server.cfg, s->app_id);

	sd_bus_message *call = s->pending_start;
	s->pending_start = NULL;
	finish_start(s, call, allowed);
	sd_bus_message_unref(call);
	return 0;
}

static int ask_user(struct session *s, sd_bus_message *call)
{
	char *text = NULL;
	int p[2];
	const char *who = *s->app_id ? s->app_id : "An application";

	if (asprintf(&text, "%s wants to control your keyboard and mouse%s.", who,
		     s->clipboard_requested ? " and use your clipboard" : "") < 0)
		return -ENOMEM;
	if (pipe2(p, O_CLOEXEC) < 0) {
		free(text);
		return -errno;
	}
	pid_t pid = fork();
	if (pid < 0) {
		free(text);
		close(p[0]);
		close(p[1]);
		return -errno;
	}
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		dup2(p[1], STDOUT_FILENO);
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0)
			dup2(null, STDIN_FILENO);
		execlp("hyprland-dialog", "hyprland-dialog", "--title", "Remote Desktop", "--apptitle",
		       "Allow remote control?", "--text", text, "--buttons",
		       DIALOG_DENY ";" DIALOG_ONCE ";" DIALOG_ALWAYS, (char *)NULL);
		_exit(127);
	}
	free(text);
	close(p[1]);
	s->dialog_fd = p[0];

	int r = sd_event_add_child(server.event, &s->dialog_source, pid, WEXITED, dialog_exited, s);
	if (r < 0) {
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		close(s->dialog_fd);
		s->dialog_fd = -1;
		return r;
	}
	sd_event_source_set_child_process_own(s->dialog_source, true);
	s->pending_start = sd_bus_message_ref(call);
	log_info("%s: asking the user about app '%s'", s->handle, s->app_id);
	return 0;
}

static bool needs_prompt(struct session *s)
{
	if (!*s->app_id)
		return server.cfg.ask_unidentified;
	return !config_app_allowed(&server.cfg, s->app_id);
}

static int method_start(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *handle, *session_handle, *app_id, *parent;
	int r;

	if ((r = sd_bus_message_read(m, "ooss", &handle, &session_handle, &app_id, &parent)) < 0)
		return r;
	struct session *s = session_find(session_handle);
	if (!s || s->started || s->pending_start)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "no such session, or already started");
	if (!s->devices)
		return reply_start(m, s, RESPONSE_OTHER);

	if (!needs_prompt(s)) {
		finish_start(s, m, true);
		return 1;
	}
	if (ask_user(s, m) < 0) {
		log_warn("%s: cannot show the confirmation dialog; denying", s->handle);
		finish_start(s, m, false);
	}
	return 1; /* replied now, or once the dialog closes */
}

/* ---- EIS -------------------------------------------------------------- */

static int method_connect_to_eis(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *session_handle, *app_id;
	int r;

	if ((r = sd_bus_message_read(m, "os", &session_handle, &app_id)) < 0)
		return r;
	struct session *s = session_find(session_handle);
	if (!s || !s->started)
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "session is not started");

	int fd = session_connect_eis(s);
	if (fd < 0)
		return sd_bus_error_set_errno(err, fd);
	r = sd_bus_reply_method_return(m, "h", fd); /* sd-bus duplicates the fd */
	close(fd);
	log_info("%s: handed out an EIS connection", session_handle);
	return r;
}

/* ---- legacy Notify* input methods ------------------------------------- */

static int method_pointer_motion(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *h;
	double dx, dy;
	int r;
	if ((r = sd_bus_message_read(m, "o", &h)) < 0 || (r = read_options_u(m, NULL, NULL, NULL)) < 0 ||
	    (r = sd_bus_message_read(m, "dd", &dx, &dy)) < 0)
		return r;
	struct session *s = started_session(h, DEVICE_POINTER, err);
	if (!s)
		return -EINVAL;
	vinput_motion(&s->vi, dx, dy);
	vinput_frame(&s->vi);
	return sd_bus_reply_method_return(m, "");
}

static int method_pointer_motion_absolute(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *h;
	uint32_t stream;
	double x, y;
	int r;
	if ((r = sd_bus_message_read(m, "o", &h)) < 0 || (r = read_options_u(m, NULL, NULL, NULL)) < 0 ||
	    (r = sd_bus_message_read(m, "udd", &stream, &x, &y)) < 0)
		return r;
	struct session *s = started_session(h, DEVICE_POINTER, err);
	if (!s)
		return -EINVAL;
	/* there are no screencast streams; coordinates are layout-relative */
	vinput_motion_absolute(&s->vi, &server.wl, x, y);
	vinput_frame(&s->vi);
	return sd_bus_reply_method_return(m, "");
}

static int method_pointer_button(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *h;
	int32_t button;
	uint32_t state;
	int r;
	if ((r = sd_bus_message_read(m, "o", &h)) < 0 || (r = read_options_u(m, NULL, NULL, NULL)) < 0 ||
	    (r = sd_bus_message_read(m, "iu", &button, &state)) < 0)
		return r;
	struct session *s = started_session(h, DEVICE_POINTER, err);
	if (!s)
		return -EINVAL;
	vinput_button(&s->vi, (uint32_t)button, state != 0);
	vinput_frame(&s->vi);
	return sd_bus_reply_method_return(m, "");
}

static int method_pointer_axis(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *h;
	double dx, dy;
	int r;
	if ((r = sd_bus_message_read(m, "o", &h)) < 0)
		return r;
	bool finish = read_options_bool(m, "finish");
	if ((r = sd_bus_message_read(m, "dd", &dx, &dy)) < 0)
		return r;
	struct session *s = started_session(h, DEVICE_POINTER, err);
	if (!s)
		return -EINVAL;
	if (dx != 0 || dy != 0)
		vinput_scroll(&s->vi, dx, dy);
	if (finish)
		vinput_scroll_stop(&s->vi, true, true);
	vinput_frame(&s->vi);
	return sd_bus_reply_method_return(m, "");
}

static int method_pointer_axis_discrete(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *h;
	uint32_t axis;
	int32_t steps;
	int r;
	if ((r = sd_bus_message_read(m, "o", &h)) < 0 || (r = read_options_u(m, NULL, NULL, NULL)) < 0 ||
	    (r = sd_bus_message_read(m, "ui", &axis, &steps)) < 0)
		return r;
	struct session *s = started_session(h, DEVICE_POINTER, err);
	if (!s)
		return -EINVAL;
	if (axis == 0)
		vinput_scroll_discrete(&s->vi, 0, steps * 120);
	else
		vinput_scroll_discrete(&s->vi, steps * 120, 0);
	vinput_frame(&s->vi);
	return sd_bus_reply_method_return(m, "");
}

static int method_keyboard_keycode(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *h;
	int32_t keycode;
	uint32_t state;
	int r;
	if ((r = sd_bus_message_read(m, "o", &h)) < 0 || (r = read_options_u(m, NULL, NULL, NULL)) < 0 ||
	    (r = sd_bus_message_read(m, "iu", &keycode, &state)) < 0)
		return r;
	struct session *s = started_session(h, DEVICE_KEYBOARD, err);
	if (!s)
		return -EINVAL;
	vinput_key(&s->vi, (uint32_t)keycode, state != 0);
	return sd_bus_reply_method_return(m, "");
}

static int method_keyboard_keysym(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *h;
	int32_t keysym;
	uint32_t state;
	int r;
	if ((r = sd_bus_message_read(m, "o", &h)) < 0 || (r = read_options_u(m, NULL, NULL, NULL)) < 0 ||
	    (r = sd_bus_message_read(m, "iu", &keysym, &state)) < 0)
		return r;
	struct session *s = started_session(h, DEVICE_KEYBOARD, err);
	if (!s)
		return -EINVAL;
	if (!vinput_keysym(&s->vi, (xkb_keysym_t)keysym, state != 0))
		return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "keysym is not in the keymap");
	return sd_bus_reply_method_return(m, "");
}

static int method_touch(sd_bus_message *m, void *data, sd_bus_error *err)
{
	return sd_bus_error_set(err, SD_BUS_ERROR_NOT_SUPPORTED, "touch input is not supported");
}

/* ---- properties ------------------------------------------------------- */

static int prop_u(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
		  void *data, sd_bus_error *err)
{
	uint32_t v = strcmp(prop, "version") == 0 ? 2 : SUPPORTED_DEVICES;
	return sd_bus_message_append(reply, "u", v);
}

static const sd_bus_vtable remote_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", method_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectDevices", "oosa{sv}", "ua{sv}", method_select_devices, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}", method_start, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerMotion", "oa{sv}dd", "", method_pointer_motion, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerMotionAbsolute", "oa{sv}udd", "", method_pointer_motion_absolute,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerButton", "oa{sv}iu", "", method_pointer_button, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerAxis", "oa{sv}dd", "", method_pointer_axis, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerAxisDiscrete", "oa{sv}ui", "", method_pointer_axis_discrete,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyKeyboardKeycode", "oa{sv}iu", "", method_keyboard_keycode, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyKeyboardKeysym", "oa{sv}iu", "", method_keyboard_keysym, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyTouchDown", "oa{sv}uudd", "", method_touch, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyTouchMotion", "oa{sv}uudd", "", method_touch, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyTouchUp", "oa{sv}u", "", method_touch, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ConnectToEIS", "osa{sv}", "h", method_connect_to_eis, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("AvailableDeviceTypes", "u", prop_u, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("version", "u", prop_u, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

int portal_init(void)
{
	int r = sd_bus_add_object_vtable(server.bus, NULL, PORTAL_PATH, IFACE_REMOTE, remote_vtable, NULL);
	if (r < 0) {
		log_error("cannot export %s: %s", IFACE_REMOTE, strerror(-r));
		return r;
	}
	return clipboard_init();
}
