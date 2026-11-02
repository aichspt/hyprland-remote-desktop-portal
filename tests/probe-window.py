#!/usr/bin/env python3
"""Small GTK window that prints the input events it receives, one per line."""
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

APP_ID = "dev.hrdp.Probe"


def out(*parts):
    print(*parts, flush=True)


def on_key_pressed(ctrl, keyval, keycode, state):
    out("key-press", Gdk.keyval_name(keyval), keycode - 8)
    return True


def on_key_released(ctrl, keyval, keycode, state):
    out("key-release", Gdk.keyval_name(keyval), keycode - 8)


def on_button(gesture, n_press, x, y, kind):
    out(kind, gesture.get_current_button())


def on_scroll(ctrl, dx, dy):
    out("scroll", f"{dx:g}", f"{dy:g}")
    return True


def activate(app):
    win = Gtk.ApplicationWindow(application=app, title="hrdp-probe")
    win.set_default_size(500, 400)
    win.set_child(Gtk.Label(label="Remote desktop portal test.\nPlease don't touch the mouse or keyboard."))

    keys = Gtk.EventControllerKey()
    keys.connect("key-pressed", on_key_pressed)
    keys.connect("key-released", on_key_released)
    win.add_controller(keys)

    click = Gtk.GestureClick(button=0)
    click.connect("pressed", on_button, "button-press")
    click.connect("released", on_button, "button-release")
    win.add_controller(click)

    scroll = Gtk.EventControllerScroll(flags=Gtk.EventControllerScrollFlags.BOTH_AXES)
    scroll.connect("scroll", on_scroll)
    win.add_controller(scroll)

    win.present()
    GLib.timeout_add(500, lambda: out("ready") or False)


app = Gtk.Application(application_id=APP_ID)
app.connect("activate", activate)
app.run()
