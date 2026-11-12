/* Test client: goes through the RemoteDesktop portal exactly like Deskflow
 * does (libportal + ConnectToEIS + libei), then executes commands from stdin.
 *
 * Commands, one per line; each is answered with "OK ..." or "ERR ...":
 *   regions                 list absolute-pointer regions as "x y w h name"
 *   abs X Y                 absolute motion (region coordinates)
 *   rel DX DY               relative motion
 *   button CODE 1|0         press/release an evdev button (BTN_LEFT = 272)
 *   scroll V120             vertical wheel, 120 = one detent
 *   key CODE 1|0            press/release an evdev key
 *   cbmimes                 clipboard state as "owner=0|1 MIME..."
 *   cbget MIME              read the clipboard as MIME (must be text)
 *   cbset TEXT              put TEXT on the clipboard as text/plain
 *   quit
 * "READY" is printed once all devices are emulating.
 */
#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <libei.h>
#include <libportal/portal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GMainLoop *loop;
static XdpPortal *portal;
static XdpSession *session;
static struct ei *ei;
static struct ei_device *dev_rel, *dev_abs, *dev_kbd;
static uint32_t sequence;
static bool ready;
static int exit_code = 1;
static char *clip_text; /* what cbset put on the clipboard */

static void fail(const char *what, GError *err)
{
	printf("ERR %s: %s\n", what, err ? err->message : "failed");
	fflush(stdout);
	g_main_loop_quit(loop);
}

static void check_ready(void)
{
	if (ready || !dev_rel || !dev_abs || !dev_kbd)
		return;
	ready = true;
	printf("READY\n");
	fflush(stdout);
}

static struct ei_device **slot_for(struct ei_device *d)
{
	if (ei_device_has_capability(d, EI_DEVICE_CAP_KEYBOARD))
		return &dev_kbd;
	if (ei_device_has_capability(d, EI_DEVICE_CAP_POINTER_ABSOLUTE))
		return &dev_abs;
	if (ei_device_has_capability(d, EI_DEVICE_CAP_POINTER))
		return &dev_rel;
	return NULL;
}

static gboolean on_ei(gint fd, GIOCondition cond, gpointer data)
{
	struct ei_event *e;

	ei_dispatch(ei);
	while ((e = ei_get_event(ei))) {
		struct ei_device *d = ei_event_get_device(e);
		struct ei_device **slot;

		switch (ei_event_get_type(e)) {
		case EI_EVENT_SEAT_ADDED:
			ei_seat_bind_capabilities(ei_event_get_seat(e), EI_DEVICE_CAP_POINTER,
						  EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON,
						  EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD, NULL);
			break;
		case EI_EVENT_DEVICE_ADDED:
			if ((slot = slot_for(d)) && !*slot)
				*slot = ei_device_ref(d);
			break;
		case EI_EVENT_DEVICE_RESUMED:
			ei_device_start_emulating(d, ++sequence);
			check_ready();
			break;
		case EI_EVENT_DEVICE_REMOVED:
			if ((slot = slot_for(d)) && *slot == d) {
				ei_device_unref(*slot);
				*slot = NULL;
				ready = false;
			}
			break;
		case EI_EVENT_DISCONNECT:
			printf("ERR disconnected by the EIS server\n");
			fflush(stdout);
			g_main_loop_quit(loop);
			break;
		default:
			break;
		}
		ei_event_unref(e);
	}
	return G_SOURCE_CONTINUE;
}

static void frame(struct ei_device *d)
{
	ei_device_frame(d, ei_now(ei));
}

/* ---- clipboard -------------------------------------------------------- */

static void on_selection_transfer(XdpSession *s, const char *mime, guint serial, gpointer data)
{
	int fd = xdp_session_selection_write(session, serial);
	gboolean ok = fd >= 0 && clip_text && write(fd, clip_text, strlen(clip_text)) == (ssize_t)strlen(clip_text);
	if (fd >= 0)
		close(fd);
	xdp_session_selection_write_done(session, serial, ok);
}

static void clip_mimes(void)
{
	const char **mimes = xdp_session_get_selection_mime_types(session);
	printf("OK owner=%d", xdp_session_is_selection_owned_by_session(session));
	for (size_t i = 0; mimes && mimes[i]; i++)
		printf(" %s", mimes[i]);
	printf("\n");
}

static void clip_get(const char *mime)
{
	char buf[4096];
	size_t len = 0;
	ssize_t n;

	int fd = xdp_session_selection_read(session, mime);
	if (fd < 0) {
		printf("ERR SelectionRead failed\n");
		return;
	}
	while (len < sizeof(buf) - 1 && (n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0)
		len += n;
	close(fd);
	buf[len] = '\0';
	printf("OK %s\n", buf);
}

static void clip_set(const char *text)
{
	static const char *mimes[] = {"text/plain;charset=utf-8", "text/plain", NULL};
	g_free(clip_text);
	clip_text = g_strdup(text);
	xdp_session_set_selection(session, mimes);
	printf("OK\n");
}

/* Handles commands with string arguments; returns false for the others. */
static bool run_clip_command(char *line)
{
	g_strchomp(line);
	if (strcmp(line, "cbmimes") == 0)
		clip_mimes();
	else if (g_str_has_prefix(line, "cbget "))
		clip_get(line + 6);
	else if (g_str_has_prefix(line, "cbset "))
		clip_set(line + 6);
	else
		return false;
	return true;
}

static void run_command(char *line)
{
	char cmd[32];
	double a = 0, b = 0;
	int n = sscanf(line, "%31s %lf %lf", cmd, &a, &b);

	if (n < 1)
		return;
	if (ready && xdp_session_is_clipboard_enabled(session) && run_clip_command(line)) {
		fflush(stdout);
		return;
	}
	if (strcmp(cmd, "quit") == 0) {
		exit_code = 0;
		g_main_loop_quit(loop);
		return;
	}
	if (!ready) {
		printf("ERR not ready\n");
	} else if (strcmp(cmd, "regions") == 0) {
		struct ei_region *r;
		printf("OK");
		for (size_t i = 0; (r = ei_device_get_region(dev_abs, i)); i++)
			printf(" %u,%u,%u,%u,%s", ei_region_get_x(r), ei_region_get_y(r), ei_region_get_width(r),
			       ei_region_get_height(r), ei_region_get_mapping_id(r) ? ei_region_get_mapping_id(r) : "-");
		printf("\n");
	} else if (strcmp(cmd, "abs") == 0 && n == 3) {
		ei_device_pointer_motion_absolute(dev_abs, a, b);
		frame(dev_abs);
		printf("OK\n");
	} else if (strcmp(cmd, "rel") == 0 && n == 3) {
		ei_device_pointer_motion(dev_rel, a, b);
		frame(dev_rel);
		printf("OK\n");
	} else if (strcmp(cmd, "button") == 0 && n == 3) {
		ei_device_button_button(dev_abs, (uint32_t)a, b != 0);
		frame(dev_abs);
		printf("OK\n");
	} else if (strcmp(cmd, "scroll") == 0 && n == 2) {
		ei_device_scroll_discrete(dev_abs, 0, (int32_t)a);
		frame(dev_abs);
		printf("OK\n");
	} else if (strcmp(cmd, "key") == 0 && n == 3) {
		ei_device_keyboard_key(dev_kbd, (uint32_t)a, b != 0);
		frame(dev_kbd);
		printf("OK\n");
	} else {
		printf("ERR bad command: %s", line);
	}
	fflush(stdout);
}

static gboolean on_stdin(GIOChannel *ch, GIOCondition cond, gpointer data)
{
	gchar *line = NULL;
	GIOStatus st = g_io_channel_read_line(ch, &line, NULL, NULL, NULL);

	if (st == G_IO_STATUS_EOF || st == G_IO_STATUS_ERROR) {
		exit_code = 0;
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	if (line)
		run_command(line);
	g_free(line);
	return G_SOURCE_CONTINUE;
}

static void on_started(GObject *src, GAsyncResult *res, gpointer data)
{
	GError *err = NULL;

	if (!xdp_session_start_finish(session, res, &err))
		return fail("Start", err);
	printf("STARTED devices=0x%x clipboard=%d\n", xdp_session_get_devices(session),
	       xdp_session_is_clipboard_enabled(session));
	fflush(stdout);

	int fd = xdp_session_connect_to_eis(session, &err);
	if (fd < 0)
		return fail("ConnectToEIS", err);

	ei = ei_new_sender(NULL);
	ei_configure_name(ei, "rd-test-client");
	int r = ei_setup_backend_fd(ei, fd);
	if (r < 0) {
		printf("ERR ei_setup_backend_fd: %s\n", strerror(-r));
		g_main_loop_quit(loop);
		return;
	}
	g_unix_fd_add(ei_get_fd(ei), G_IO_IN, on_ei, NULL);

	GIOChannel *ch = g_io_channel_unix_new(0);
	g_io_add_watch(ch, G_IO_IN | G_IO_HUP, on_stdin, NULL);
}

static void on_created(GObject *src, GAsyncResult *res, gpointer data)
{
	GError *err = NULL;

	session = xdp_portal_create_remote_desktop_session_finish(portal, res, &err);
	if (!session)
		return fail("CreateSession", err);
	/* Deskflow asks for the clipboard before starting, like this */
	g_signal_connect(session, "selection-transfer", G_CALLBACK(on_selection_transfer), NULL);
	xdp_session_request_clipboard(session);
	xdp_session_start(session, NULL, NULL, on_started, NULL);
}

int main(void)
{
	GError *err = NULL;

	setvbuf(stdout, NULL, _IOLBF, 0);
	loop = g_main_loop_new(NULL, FALSE);
	portal = xdp_portal_initable_new(&err);
	if (!portal) {
		fail("portal", err);
		return 1;
	}
	/* the same call Deskflow makes */
	xdp_portal_create_remote_desktop_session_full(portal, XDP_DEVICE_KEYBOARD | XDP_DEVICE_POINTER,
						      XDP_OUTPUT_NONE, XDP_REMOTE_DESKTOP_FLAG_NONE,
						      XDP_CURSOR_MODE_HIDDEN, XDP_PERSIST_MODE_NONE, NULL, NULL,
						      on_created, NULL);
	g_main_loop_run(loop);
	if (session)
		xdp_session_close(session);
	if (ei)
		ei_unref(ei);
	return exit_code;
}
