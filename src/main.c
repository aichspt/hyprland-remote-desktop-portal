#define _GNU_SOURCE
#include "hrdp.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct server server;
int log_verbose;

void log_msg(const char *level, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "[%s] ", level);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int wl_io(sd_event_source *src, int fd, uint32_t revents, void *data)
{
	if ((revents & (EPOLLERR | EPOLLHUP)) || wl_display_dispatch(server.wl.display) < 0) {
		log_error("lost the Wayland connection");
		sd_event_exit(server.event, 1);
	}
	return 0;
}

/* Runs after every batch of events: apply layout changes, flush requests. */
static int on_post(sd_event_source *src, void *data)
{
	if (server.wl.layout_changed) {
		server.wl.layout_changed = false;
		sessions_layout_changed();
	}
	if (wl_display_flush(server.wl.display) < 0 && errno != EAGAIN) {
		log_error("cannot flush the Wayland connection");
		sd_event_exit(server.event, 1);
	}
	return 0;
}

static int on_name_lost(sd_bus_message *m, void *data, sd_bus_error *err)
{
	const char *name;
	if (sd_bus_message_read(m, "s", &name) >= 0 && strcmp(name, BUS_NAME) == 0) {
		log_info("replaced by another instance; exiting");
		sd_event_exit(server.event, 0);
	}
	return 0;
}

static void usage(const char *argv0)
{
	printf("Usage: %s [-v] [-r]\n"
	       "  -v, --verbose   log debug messages\n"
	       "  -r, --replace   replace a running instance\n",
	       argv0);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{"verbose", no_argument, NULL, 'v'},
		{"replace", no_argument, NULL, 'r'},
		{"help", no_argument, NULL, 'h'},
		{0},
	};
	bool replace = false;
	int c, r, exit_code = EXIT_FAILURE;

	while ((c = getopt_long(argc, argv, "vrh", opts, NULL)) != -1) {
		switch (c) {
		case 'v':
			log_verbose = 1;
			break;
		case 'r':
			replace = true;
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* sd-event handles these; SIGCHLD is for the confirmation dialog */
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	wl_list_init(&server.sessions);
	config_load(&server.cfg);

	server.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!server.xkb) {
		log_error("cannot create an xkb context");
		return EXIT_FAILURE;
	}
	if (wl_init(&server.wl) < 0)
		goto out;

	if ((r = sd_event_default(&server.event)) < 0)
		goto out;
	sd_event_add_signal(server.event, NULL, SIGTERM, NULL, NULL);
	sd_event_add_signal(server.event, NULL, SIGINT, NULL, NULL);
	if ((r = sd_event_add_io(server.event, NULL, wl_display_get_fd(server.wl.display), EPOLLIN, wl_io, NULL)) < 0 ||
	    (r = sd_event_add_post(server.event, NULL, on_post, NULL)) < 0) {
		log_error("cannot set up the event loop: %s", strerror(-r));
		goto out;
	}

	if ((r = sd_bus_open_user(&server.bus)) < 0) {
		log_error("cannot connect to the session bus: %s", strerror(-r));
		goto out;
	}
	if ((r = sd_bus_attach_event(server.bus, server.event, SD_EVENT_PRIORITY_NORMAL)) < 0 || portal_init() < 0)
		goto out;
	uint64_t flags = SD_BUS_NAME_ALLOW_REPLACEMENT | (replace ? SD_BUS_NAME_REPLACE_EXISTING : 0);
	if ((r = sd_bus_request_name(server.bus, BUS_NAME, flags)) < 0) {
		log_error("cannot own %s: %s (already running? use --replace)", BUS_NAME, strerror(-r));
		goto out;
	}
	/* exit when another instance takes the name over */
	sd_bus_match_signal(server.bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
			    "NameLost", on_name_lost, NULL);
	log_info("ready on %s", BUS_NAME);

	r = sd_event_loop(server.event);
	exit_code = r < 0 ? EXIT_FAILURE : r;

out:
	while (!wl_list_empty(&server.sessions)) {
		struct session *s = wl_container_of(server.sessions.next, s, link);
		session_destroy(s, true);
	}
	if (server.bus) {
		sd_bus_flush(server.bus);
		sd_bus_flush_close_unref(server.bus);
	}
	clipboard_finish();
	wl_finish(&server.wl);
	if (server.event)
		sd_event_unref(server.event);
	xkb_context_unref(server.xkb);
	return exit_code;
}
