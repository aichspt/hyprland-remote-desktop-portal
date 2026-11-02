PREFIX ?= $(HOME)/.local
LIBEXECDIR ?= $(PREFIX)/libexec
DATADIR ?= $(PREFIX)/share
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user
PORTAL_CONF_DIR ?= $(HOME)/.config/xdg-desktop-portal

PKGS := libeis-1.0 libsystemd wayland-client xkbcommon
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

BIN := hyprland-remote-desktop-portal
PROTOCOLS := wlr-virtual-pointer-unstable-v1 virtual-keyboard-unstable-v1 xdg-output-unstable-v1
PROTO_H := $(PROTOCOLS:%=build/%-client-protocol.h)
PROTO_C := $(PROTOCOLS:%=build/%-protocol.c)
SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=build/%.o) $(PROTO_C:.c=.o)

TEST_PKGS := libei-1.0 libportal gio-2.0 gio-unix-2.0
TEST_BIN := build/rd-test-client

all: build/$(BIN)

build:
	mkdir -p build

build/%-client-protocol.h: protocol/%.xml | build
	wayland-scanner client-header $< $@

build/%-protocol.c: protocol/%.xml | build
	wayland-scanner private-code $< $@

build/%-protocol.o: build/%-protocol.c
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.c src/hrdp.h $(PROTO_H) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_BIN): tests/rd-test-client.c | build
	$(CC) -O2 -g -Wall $(shell pkg-config --cflags $(TEST_PKGS)) $< $(shell pkg-config --libs $(TEST_PKGS)) -o $@

test: build/$(BIN) $(TEST_BIN)
	tests/run-tests.sh

install: build/$(BIN)
	install -Dm755 build/$(BIN) $(DESTDIR)$(LIBEXECDIR)/$(BIN)
	install -Dm644 data/hyprland-remote.portal $(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals/hyprland-remote.portal
	sed 's|@LIBEXECDIR@|$(LIBEXECDIR)|' data/org.freedesktop.impl.portal.desktop.hyprland_remote.service.in \
		> build/org.freedesktop.impl.portal.desktop.hyprland_remote.service
	install -Dm644 build/org.freedesktop.impl.portal.desktop.hyprland_remote.service \
		$(DESTDIR)$(DATADIR)/dbus-1/services/org.freedesktop.impl.portal.desktop.hyprland_remote.service
	sed 's|@LIBEXECDIR@|$(LIBEXECDIR)|' data/$(BIN).service.in > build/$(BIN).service
	install -Dm644 build/$(BIN).service $(DESTDIR)$(SYSTEMD_USER_DIR)/$(BIN).service
	install -Dm644 data/hyprland-portals.conf $(DESTDIR)$(PORTAL_CONF_DIR)/hyprland-portals.conf

uninstall:
	rm -f $(DESTDIR)$(LIBEXECDIR)/$(BIN) \
		$(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals/hyprland-remote.portal \
		$(DESTDIR)$(DATADIR)/dbus-1/services/org.freedesktop.impl.portal.desktop.hyprland_remote.service \
		$(DESTDIR)$(SYSTEMD_USER_DIR)/$(BIN).service \
		$(DESTDIR)$(PORTAL_CONF_DIR)/hyprland-portals.conf

clean:
	rm -rf build

.PHONY: all test install uninstall clean
