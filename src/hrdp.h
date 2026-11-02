/* hyprland-remote-desktop-portal: a RemoteDesktop portal backend for Hyprland.
 *
 * Input arriving from portal clients (through libei, or through the legacy
 * Notify* D-Bus methods) is injected into the compositor with the
 * wlr-virtual-pointer and virtual-keyboard Wayland protocols.
 */
#pragma once

#include <libeis.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.hyprland_remote"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"

/* Device type bits from the RemoteDesktop portal spec. */
#define DEVICE_KEYBOARD 1u
#define DEVICE_POINTER 2u
#define DEVICE_TOUCHSCREEN 4u
#define SUPPORTED_DEVICES (DEVICE_KEYBOARD | DEVICE_POINTER)

#define KEY_COUNT 768 /* KEY_MAX + 1 */
#define MAX_BUTTONS 32

/* ---- logging ---------------------------------------------------------- */

extern int log_verbose;
void log_msg(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define log_debug(...) do { if (log_verbose) log_msg("debug", __VA_ARGS__); } while (0)
#define log_info(...) log_msg("info", __VA_ARGS__)
#define log_warn(...) log_msg("warn", __VA_ARGS__)
#define log_error(...) log_msg("error", __VA_ARGS__)

/* ---- configuration ---------------------------------------------------- */

struct config {
	/* Apps (by portal app id) that are allowed without a prompt. */
	char **allowed_apps;
	size_t n_allowed_apps;
	/* Apps without an app id are unsandboxed host programs. They can already
	 * use the virtual input protocols directly, so prompting adds little. */
	bool ask_unidentified;
	/* Explicit XKB names; empty fields fall back to Hyprland's settings. */
	char *xkb_rules, *xkb_model, *xkb_layout, *xkb_variant, *xkb_options;
};

void config_load(struct config *cfg);
bool config_app_allowed(const struct config *cfg, const char *app_id);
void config_remember_app(struct config *cfg, const char *app_id);

/* ---- wayland ---------------------------------------------------------- */

struct output {
	struct wl_list link;
	uint32_t global_name;
	struct wl_output *wl_output;
	struct zxdg_output_v1 *xdg_output;
	char *name;
	int32_t x, y, width, height; /* logical layout geometry */
	int32_t mode_width;          /* physical pixels, for the scale factor */
	bool has_geometry;
};

struct rect {
	int32_t x, y, width, height;
};

struct wl_state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
	struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct wl_list outputs; /* struct output.link */
	bool layout_changed;
};

int wl_init(struct wl_state *wl);
void wl_finish(struct wl_state *wl);
struct rect wl_layout_bounds(struct wl_state *wl);

/* Virtual devices owned by one portal session. */
struct vinput {
	struct zwlr_virtual_pointer_v1 *pointer;
	struct zwp_virtual_keyboard_v1 *keyboard;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	uint32_t mods_depressed, mods_latched, mods_locked, group;
	uint8_t keys_down[KEY_COUNT];
	uint32_t buttons_down[MAX_BUTTONS];
	size_t n_buttons_down;
	bool pointer_dirty; /* events sent since the last frame */
};

int vinput_create(struct vinput *vi, struct wl_state *wl, uint32_t devices, struct xkb_keymap *keymap);
void vinput_destroy(struct vinput *vi);
void vinput_release_all(struct vinput *vi);
void vinput_release_keys(struct vinput *vi);
void vinput_release_buttons(struct vinput *vi);
void vinput_motion(struct vinput *vi, double dx, double dy);
void vinput_motion_absolute(struct vinput *vi, struct wl_state *wl, double x, double y);
void vinput_button(struct vinput *vi, uint32_t button, bool pressed);
void vinput_scroll(struct vinput *vi, double dx, double dy);
void vinput_scroll_discrete(struct vinput *vi, int32_t dx120, int32_t dy120);
void vinput_scroll_stop(struct vinput *vi, bool stop_x, bool stop_y);
void vinput_frame(struct vinput *vi);
void vinput_key(struct vinput *vi, uint32_t evdev_code, bool pressed);
bool vinput_keysym(struct vinput *vi, xkb_keysym_t keysym, bool pressed);

/* ---- keymap ----------------------------------------------------------- */

struct xkb_keymap *keymap_compile(struct xkb_context *ctx, const struct config *cfg);
/* Returns a sealed memfd holding the keymap text, or -1. */
int keymap_to_fd(struct xkb_keymap *keymap, size_t *size);

/* ---- sessions --------------------------------------------------------- */

struct session {
	struct wl_list link;
	char *handle;
	char *app_id;
	sd_bus_slot *slot;
	uint32_t devices; /* selected, then granted */
	bool started;

	struct vinput vi;

	/* libei connection, after ConnectToEIS */
	struct eis *eis;
	sd_event_source *eis_source;
	struct eis_client *client;
	struct eis_seat *seat;
	struct eis_device *dev_pointer, *dev_absolute, *dev_keyboard;
	uint32_t bound_caps; /* EIS_DEVICE_CAP_* bits the client bound */

	/* pending Start call waiting for the confirmation dialog */
	sd_bus_message *pending_start;
	sd_event_source *dialog_source;
	int dialog_fd;
};

struct server {
	sd_event *event;
	sd_bus *bus;
	struct wl_state wl;
	struct xkb_context *xkb;
	struct config cfg;
	struct wl_list sessions; /* struct session.link */
};

extern struct server server;

int portal_init(void);
struct session *session_find(const char *handle);
void session_destroy(struct session *s, bool emit_closed);
int session_start_input(struct session *s);
int session_connect_eis(struct session *s);
void sessions_layout_changed(void);
