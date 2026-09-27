# Spaxer

An external Counter-Strike 2 toolkit for Linux. Spaxer never injects code into the game process for its features: it reads and writes game memory from the outside and draws everything on a transparent overlay on top of CS2. A small preloaded library dumps the game's schema offsets on launch, so updates to CS2 don't break things.

It's built for and tested on CachyOS with Hyprland (Wayland), but any modern desktop Linux with a compositor should work.

> Using this on official Valve servers can and will get your account banned. Play on your own servers, against bots, or on community servers that allow it. You're responsible for how you use it.

---

## What's inside

**Visuals**
- **ESP**: boxes that hug the actual skeleton, animated health bars, names, weapons, skeleton and head circles. Players fade in smoothly and hidden enemies are drawn slightly dimmer.
- **Chams**: the real player model, re-rendered on the overlay and visible through walls. There are two materials, *Metallic* (lit, shiny, with rim light) and *Flat*. You can also hide the in-game model so only the chams remain.
- **Glow**: the game's own outline glow, with custom colors.
- **Sound ESP**: expanding rings on the floor wherever an enemy steps.
- **Weapon ESP**: dropped weapons on the ground, with a distance limit.
- **Arrows**: arrows around the crosshair pointing at off-screen enemies.
- **Grenade prediction**, **hitmarkers**, **radar**, **bomb timer**, **spectator list**, **custom crosshair**, **sniper crosshair**.

**Aim**
- **Aimbot**: FOV in degrees (1–180), multipoint (head, neck, chest, pelvis), aim key, target priority, AimLock, switch delay and visibility check.
- **Humanization**: randomized speed, shake and release speed (see below).
- **Triggerbot**: hitbox-accurate. It checks the crosshair ray against the target's bones before firing, waits for scope accuracy on snipers and respects recoil.
- **RCS**: recoil control.

**Movement**
- **Bunny hop** with auto jump.
- **Auto strafe**, driven by your mouse movement.

**Other**
- **Lua scripting**: toggle and tune any feature, add your own binds and timers, draw HUDs and poke at memory (see below).
- **Configs**: save and load them from the settings window.

---

## Requirements

- Linux with X11 or XWayland (the overlay uses GTK3 + gtk-layer-shell)
- `g++` with C++20, `make`, `pkg-config`
- Libraries: `gtk3`, `gtk-layer-shell`, `cairo`, `libx11`, `libxtst`, `luajit`
- Python 3 with `numpy`, only needed once to generate the chams models
- Read access to `/dev/input/*` (be in the `input` group) and `/dev/uinput` for key simulation

On Arch / CachyOS:

```sh
sudo pacman -S base-devel gtk3 gtk-layer-shell cairo libx11 libxtst luajit python-numpy
sudo usermod -aG input $USER   # log out and back in afterwards
```

---

## Building

```sh
make
```

This produces two binaries:

- `spaxer`: the overlay, which does all the work
- `spaxer-gui`: the settings window

Build the offset dumper once:

```sh
cd dump_inject
g++ -std=c++20 -O2 -fPIC -shared dump.cpp -o libspaxer_dump.so -lpthread -ldl
```

---

## First-time setup

### 1. Launch options

In Steam, open CS2 → Properties → Launch Options and add:

```
LD_PRELOAD=/full/path/to/dump_inject/libspaxer_dump.so %command%
```

Every time the game starts, the dumper walks the schema system and writes fresh offsets to `/tmp/offsets_dump.h`. The overlay picks them up automatically, even if the file shows up after the overlay started.

### 2. Chams models

Chams draw the real agent meshes, which have to be pulled out of your game files once. The converter uses [Source2Viewer CLI](https://github.com/ValveResourceFormat/ValveResourceFormat), expected at `~/.local/share/spaxer/tools/s2v/`, and writes every agent model to `~/.config/spaxer/models/` (around 100 MB):

```sh
mkdir -p ~/.local/share/spaxer/tools && cd ~/.local/share/spaxer/tools
curl -L -o cli.zip https://github.com/ValveResourceFormat/ValveResourceFormat/releases/latest/download/cli-linux-x64.zip
unzip -o cli.zip -d s2v && chmod +x s2v/Source2Viewer-CLI
cd - && python3 tools/chams_models.py
```

Re-run `python3 tools/chams_models.py` if Valve ever changes the agent models. If a model is missing, chams fall back to a simple bone-based silhouette.

### 3. Run it

```sh
./spaxer &
./spaxer-gui &
```

Start CS2, join a match and press **Insert** to show or hide the settings window. Everything you change is applied instantly.

Settings live in `~/.config/spaxer/settings.bin`, and saved configs go to `~/.config/spaxer/configs/`.

---

## Aimbot humanization

A perfectly smooth, perfectly consistent aimbot is easy to spot. You'll find the humanization options under **Legit bot → Humanization**:

| Setting | What it does |
|---|---|
| **Enabled** | Turns humanization on. When it's off, the aimbot uses the regular *Speed* slider. |
| **Speed min / Speed max** | Speed isn't fixed anymore. Every 120–380 ms a new target speed is picked at random between min and max, and the aim eases toward it, so the flick speed keeps drifting the way a real hand does. Higher means faster. |
| **Shake** | Adds a small, smooth tremble while tracking, made from a couple of low-frequency waves rather than random noise, so it looks like hand jitter and not a glitch. At 100 the tremble is about ±4 pixels. |
| **Release speed** | What happens when the target is lost: it dies, leaves FOV, or you let go of the aim key. Instead of stopping dead, the crosshair keeps a bit of momentum and slows to a stop. Low values give a long, lazy follow-through and high values stop almost immediately. |

A good starting point: speed 20–45, shake 10–20, release 30–40.

---

## Chams tips

- **Material**: *Metallic* gives the shiny, lit look. *Flat* is a solid silhouette.
- **Visible / Hidden / Teammate color**: enemies you can currently see use *Visible*, and enemies behind walls use *Hidden*. The alpha channel controls transparency.
- **Latency comp (ms)**: the overlay is always a frame or two behind the game, so a running player's real model can poke out ahead of the chams. This setting pushes the chams forward along each player's velocity. Tune it on a moving bot: raise it if the real model leads, lower it if the chams overshoot. Around 30 ms is typical.
- **Hide game model**: *Transparent* makes the in-game player model invisible and *No draw* tells the game not to render it at all. Try *Transparent* first. If the model doesn't disappear, or the chams freeze in place, switch to *No draw*.

---

## Lua scripting

Scripts can toggle and tune any feature, set up their own binds and timers, draw on the overlay and read or write game memory. Drop `.lua` files into `~/.config/spaxer/scripts/` and manage them on the **Scripts** page of the settings window.

The full API reference with examples is in **[LUA_API.md](LUA_API.md)**.

---

## Troubleshooting

**Nothing is drawn.** Make sure CS2 was launched with the `LD_PRELOAD` option and that `/tmp/offsets_dump.h` exists. The overlay log prints a line like `[offsets] applied N/N fields ... stale=0`. `stale=1` means the dump is from an older game session. Once the game writes a new one, the overlay reloads it on its own within a few seconds.

**ESP or chams look offset.** Check *Latency comp* first. If boxes sit in the wrong place even when everything is standing still, your screen resolution might not match the overlay size. Run the game fullscreen at your native resolution.

**Chams show a stick figure instead of the model.** The models haven't been generated yet, or that agent is missing. Run `python3 tools/chams_models.py`.

**Keys or mouse don't respond.** Your user needs to be in the `input` group, and `/dev/uinput` must be writable.

**The settings window doesn't open with Insert.** Make sure `spaxer-gui` is running, and that CS2 is the focused window when you press Insert.

---

## Project layout

```
src/
  memory/     process attach and memory read/write
  sdk/        offsets, schema dump loading, game helpers
  features/   aimbot, triggerbot, RCS, glow, chams writes, movement, ESP data, Lua engine
  render/     camera and the software model renderer used for chams
  overlay/    the overlay window, ESP/chams drawing and HUD
  gui/        the settings window
  config/     settings file and configs
  input/      evdev input, uinput keyboard, XTest mouse
dump_inject/  LD_PRELOAD schema dumper
tools/        chams model converter
```
