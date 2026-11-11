# hyprland-remote-desktop-portal

A RemoteDesktop portal backend (`org.freedesktop.impl.portal.RemoteDesktop`)
for Hyprland.

`xdg-desktop-portal-hyprland` doesn't implement RemoteDesktop. Without it,
programs that need to inject input through the portal fail with:

```
No such interface "org.freedesktop.portal.RemoteDesktop"
```

Deskflow in client mode is one of these programs. This backend provides only
that interface, alongside the stock Hyprland portal.

## Install

You need Hyprland with `xdg-desktop-portal` and `xdg-desktop-portal-hyprland`.
Pick one of the options below, then [restart the portal](#after-installing).

### Arch Linux: AUR

```sh
yay -S hyprland-remote-desktop-portal-git
```

To build the same package yourself without an AUR helper:

```sh
git clone https://github.com/aichspt/hyprland-remote-desktop-portal.git
cd hyprland-remote-desktop-portal/packaging/aur
makepkg -si
```

The package installs to `/usr` and puts the portal routing in
`/etc/xdg/xdg-desktop-portal/hyprland-portals.conf`, so it works for every
user. Install `hyprland-guiutils` too if Flatpak apps will use it (it provides
the permission prompt).

### From source (per user, no root)

Install the dependencies. On Arch:

```sh
sudo pacman -S --needed base-devel git libei systemd-libs wayland libxkbcommon
```

Other distros need the development packages for `libei` (which includes
`libeis`), `libsystemd`, `wayland-client`, `xkbcommon`, and `wayland-scanner`.

Then build and install:

```sh
git clone https://github.com/aichspt/hyprland-remote-desktop-portal.git
cd hyprland-remote-desktop-portal
make
make install
```

This installs:

| File | Purpose |
|------|---------|
| `~/.local/libexec/hyprland-remote-desktop-portal` | the daemon |
| `~/.local/share/xdg-desktop-portal/portals/hyprland-remote.portal` | registers the backend |
| `~/.local/share/dbus-1/services/…hyprland_remote.service` | D-Bus activation |
| `~/.config/systemd/user/hyprland-remote-desktop-portal.service` | the service D-Bus activates |
| `~/.config/xdg-desktop-portal/hyprland-portals.conf` | routes RemoteDesktop here |

To uninstall, run `make uninstall` and then restart `xdg-desktop-portal`.

### After installing

Reload systemd and D-Bus, then restart the portal:

```sh
systemctl --user daemon-reload
busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig
systemctl --user restart xdg-desktop-portal
```

You don't need to enable anything. The daemon starts on demand the first time
an app asks for a RemoteDesktop session. To check that it works, run
`make test` from a source checkout (see [Test](#test)).

### Portal configuration

`xdg-desktop-portal` uses only the first `hyprland-portals.conf` it finds, in
this order: `~/.config/xdg-desktop-portal/`, `/etc/xdg/xdg-desktop-portal/`,
`/usr/share/xdg-desktop-portal/`. The copy shipped here repeats the system
default (`hyprland;gtk`) and adds the RemoteDesktop line.

If you already have your own `~/.config/xdg-desktop-portal/hyprland-portals.conf`,
it takes precedence over the package's copy. `make install` would overwrite
it, so back it up first. In both cases, add this line to your file under
`[preferred]`:

```ini
org.freedesktop.impl.portal.RemoteDesktop=hyprland-remote
```

## How it works

```
app (Deskflow) ──libportal──▶ xdg-desktop-portal ──D-Bus──▶ this backend
      │                                                        │
      └──────────────── libei (fd from ConnectToEIS) ─────────▶ libeis
                                                               │
                         zwlr_virtual_pointer_v1 / zwp_virtual_keyboard_v1
                                                               ▼
                                                           Hyprland
```

- **Sessions:** `CreateSession`, `SelectDevices`, `Start` and `ConnectToEIS`
  (interface version 2). Each started session gets its own virtual pointer and
  virtual keyboard.
- **libei:** each session offers a relative pointer, an absolute pointer and a
  keyboard. The absolute pointer has one region per monitor in logical
  coordinates. The regions are rebuilt when monitors are added, removed or
  moved.
- **Keyboard:** uses Hyprland's own `kb_layout`, `kb_variant`, `kb_options`,
  `kb_model` and `kb_rules`, read at session start. The same keymap is given to
  the client and to the virtual keyboard, so the client's keycodes produce the
  characters it expects. Modifiers are tracked with xkbcommon.
- **Legacy D-Bus input:** the `Notify*` methods work too (pointer, buttons,
  axis, keycode and keysym).
- **Stuck input:** keys and buttons still held are released when a client
  stops emulating, closes a device, disconnects, or its session closes.

## Test

The tests also need `libportal` and `python-gobject` with GTK 4.

```sh
make test
```

The test opens a session through the real `xdg-desktop-portal`, using the same
libportal and libei calls Deskflow makes, and checks:

- absolute motion to the centre and every corner and edge of each monitor,
  read back with `hyprctl cursorpos`
- relative motion
- left and right clicks, and wheel scrolling, in a GTK probe window
- typing, including Shift+A → `A`
- a key held down when the client disconnects is released

The test takes over the pointer for a few seconds. Don't touch the mouse or
keyboard while it runs.

## Access control

| Caller | Default |
|--------|---------|
| No app id (an unsandboxed host program, e.g. Deskflow from pacman) | allowed without asking |
| App with an app id (Flatpak, Snap) | asks with `hyprland-dialog`: *Deny*, *Allow once* or *Always allow* |

Host programs aren't prompted by default because they can already use the
virtual pointer and keyboard protocols directly, so a prompt wouldn't protect
anything.

Settings go in `~/.config/hyprland-remote-desktop-portal/config`:

```ini
# Apps allowed without a prompt
allow_apps = com.example.App, org.other.App
# Also prompt for programs without an app id
ask_unidentified = true
# Override the keymap instead of following Hyprland
xkb_layout = us
xkb_variant =
xkb_options =
```

Apps you choose *Always allow* for are saved to
`~/.local/state/hyprland-remote-desktop-portal/allowed-apps`.

## Debugging

```sh
journalctl --user -u hyprland-remote-desktop-portal -f
# or stop the service and run it in the foreground with debug logging:
systemctl --user stop hyprland-remote-desktop-portal
build/hyprland-remote-desktop-portal --verbose
```

## Limitations

- No touch input. `AvailableDeviceTypes` is keyboard and pointer only.
- No clipboard, and no ScreenCast streams. Absolute coordinates in the
  `NotifyPointerMotionAbsolute` D-Bus method are relative to the whole layout,
  not to a stream.
- `persist_mode` is ignored. Every session is allowed again according to the
  rules above.
- Keymap changes in Hyprland apply to new sessions only.
