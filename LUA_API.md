# Spaxer Lua API

Lua scripts can do pretty much anything the built-in features can: switch any feature on or off, change any setting, set up their own key binds, run timers, draw on the overlay, and read or write game memory.

Scripts live in `~/.config/spaxer/scripts/` and load in alphabetical order when the overlay starts. The **Scripts** page in the settings window lists them. From there you can turn each one on or off (a disabled script is renamed to `name.lua.disabled`), open it in your editor, or hit **Reload scripts** to apply changes without restarting anything. The `scripts/` folder in this repo has examples you can copy over.

**Safety net.** Scripts run on LuaJIT with the JIT compiler switched off, which keeps them safely interruptible. Every call gets a **50 ms time budget**: a script that loops forever is stopped, not frozen. A callback that throws an error is logged and **disabled** so it can't spam the console every frame. A failing bind or timer is removed the same way. Output and errors go to the overlay's stderr, prefixed with `[LUA]`.

**Colors** are `r, g, b, a` in the **0–255** range, with alpha defaulting to 255. Settings that store a color use a single `0xRRGGBBAA` number.

## Quick taste

```lua
-- F6 toggles chams, holding Mouse5 enables the triggerbot only while held
input.bind("F6", function()
    local on = config.toggle("chams")
    client.log("chams " .. (on and "on" or "off"))
end)

input.bind("MOUSE5", function(down)
    config.set("trigger_enabled", down)
end, "hold")

-- Switch to a slower, more human aim when you're scoped in
client.every(0.1, function()
    local me = entities.get_local()
    if not me then return end
    config.set("aimbot_speed_max_x100", me.scoped and 25 or 45)
end)
```

## `client`

| Function | Description |
|---|---|
| `client.register_callback(event, fn)` | Events: `"paint"` (every drawn frame, rendering allowed), `"frame"` (every display frame, even when CS2 isn't focused), `"tick"` (every ~100 ms), `"unload"` (before scripts unload). |
| `client.delay(seconds, fn)` | Run `fn` once after a delay. Returns a timer id. |
| `client.every(seconds, fn)` | Run `fn` repeatedly. Returns a timer id. |
| `client.cancel(id)` | Stop a timer. |
| `client.log(text)` | Print a line prefixed with `[LUA]`. |
| `client.load_script(path)` | Run another Lua file. Returns `true` on success. |
| `client.reload()` | Reload all scripts on the next frame. Safe to call from a callback. |
| `client.get_time()` | Monotonic time in seconds (float). |
| `client.script_dir()` | Path of the scripts folder. |

## `config`: every feature and setting

Every setting from the settings window is reachable by name. `config.list()` returns all of them as `{ name, type }`, where `type` is one of the following:

| Type | Lua value | Example |
|---|---|---|
| `toggle` | boolean | `config.set("esp", false)` |
| `number` | number | `config.set("aimbot_shake_x100", 30)` |
| `color` | `0xRRGGBBAA` or `{r, g, b, a}` | `config.set("chams_visible_rgba", {255, 150, 30, 230})` |
| `bind` | X11 key name or `nil` | `config.set("bind_esp", "F4")` |

| Function | Description |
|---|---|
| `config.get(name)` | Read a setting. |
| `config.set(name, value)` | Change a setting. The change applies instantly everywhere, the settings window included. |
| `config.toggle(name)` | Flip a toggle. Returns the new state. |
| `config.list()` | All setting names and types. |
| `config.save(name)` / `config.load(name)` | Save or load a config. Returns `true` on success. |
| `config.configs()` | Names of the saved configs. |

For convenience, `aimbot_fov`, `aimbot_smooth`, `rcs_strength` and `trigger_fov` also work in plain units (degrees, 0–1) instead of the `_x100` fields. An unknown setting name raises an error, so typos don't fail silently.

Some useful names: `esp`, `esp_box`, `esp_skeleton`, `chams`, `chams_material`, `chams_hide_model`, `glow`, `sound_esp`, `esp_dropped_weapons`, `arrows`, `aimbot_enabled`, `aimbot_lock`, `aimbot_humanize`, `trigger_enabled`, `rcs_enabled`, `bunnyhop`, `bhop_auto_jump`, `auto_strafe`, `radar`, `hitmarker`, `crosshair`. Run `for _, f in ipairs(config.list()) do client.log(f.name .. " " .. f.type) end` for the full list.

## `input`: binds, keys and mouse

| Function | Description |
|---|---|
| `input.bind(key, fn, mode, global)` | Bind a key and return its id. `key` is a name (`"F6"`, `"SPACE"`, `"LEFTSHIFT"`, `"MOUSE4"`) or a key code. `mode` is `"press"` (the default), `"release"`, `"hold"` (fn receives `true` or `false`) or `"toggle"` (fn receives the new state). Binds only fire while CS2 is focused unless `global` is `true`. |
| `input.unbind(id)` | Remove a bind. |
| `input.is_pressed(key)` | Whether a key (by name or code) is held right now. |
| `input.is_key_down(code)` | Whether a key is held, by Linux key code. |
| `input.is_mouse_down(button)` | `1` left, `2` middle, `3` right, `4` back, `5` forward. |
| `input.set_key(code, down)` | Hold or release a key on the virtual keyboard. |
| `input.press_key(code)` | Tap a key. |
| `input.set_mouse_button(button, down)` | Hold or release a mouse button. |
| `input.click(button)` | Click a mouse button (left by default). |
| `input.click_left()` | Left click. |
| `input.mouse_move(dx, dy)` | Relative mouse movement. |

The global `keys` table maps names to Linux key codes, for example `keys.SPACE`, `keys.F5`, `keys.A` and `keys.LEFTCTRL`.

## `render` (only inside `"paint"`)

| Function | Description |
|---|---|
| `render.screen_size()` | Returns `w, h`. |
| `render.text(x, y, text, r, g, b, a, size, bold)` | Text with its top-left corner at `x, y`. |
| `render.measure_text(text, size, bold)` | Returns `width, height`. |
| `render.line(x1, y1, x2, y2, r, g, b, a, width)` | Line. |
| `render.rect(x, y, w, h, r, g, b, a, width, radius)` | Rectangle outline, with optional rounded corners. |
| `render.rect_filled(x, y, w, h, r, g, b, a, radius)` | Filled rectangle. |
| `render.circle(x, y, radius, r, g, b, a, width)` | Circle outline. |
| `render.circle_filled(x, y, radius, r, g, b, a)` | Filled circle. |
| `render.triangle(...)` / `render.triangle_filled(...)` | Three points followed by a color (and a width for the outline). |
| `render.world_to_screen(x, y, z)` | Projects with the same camera the ESP uses. Returns `sx, sy, on_screen`, or `nil, nil, false` if the point is behind you. |

## `engine`

`engine.is_in_game()`, `engine.is_focused()`, `engine.get_ping()`, `engine.get_fps()`, `engine.get_local_team()` (`2` = T, `3` = CT) and `engine.get_screen_size()`.

## `entities`

| Function | Description |
|---|---|
| `entities.get_players()` | List of `{ pawn, name, weapon, model, hp, team, visible, spotted, head_pos, feet_pos }`. |
| `entities.get_local()` | `{ pawn, controller, hp, team, weapon_id, scoped, on_ground, crouching, origin, eye, angles, velocity }`, or `nil` when you're not in game. |
| `entities.get_entity(index)` | Raw entity address from the entity list, or `nil`. |
| `entities.get_bomb()` | `{ visible, blow_secs, site, being_defused, defuse_secs }` |
| `entities.get_spectators()` | List of `{ name, team }`. |
| `entities.get_dropped_items()` | List of `{ name, x, y, z }`. |

Vectors are tables `{ x, y, z }`.

## `memory` and `offsets`

Direct access to CS2 memory. Addresses are plain numbers.

- Read: `read_u8`, `read_bool`, `read_i32`, `read_u32`, `read_i64`, `read_u64`, `read_ptr`, `read_float`, `read_vec3`, `read_string(addr, max_len)`
- Write: `write_u8`, `write_bool`, `write_i32`, `write_u32`, `write_u64`, `write_float`, `write_vec3(addr, {x, y, z})`
- Module bases: `memory.get_client_base()` and `memory.get_engine_base()`

`offsets.get(name)` returns a schema offset from the live dump, such as `offsets.get("m_iHealth")`, or `nil` if it's unknown. `offsets.list()` returns every available name.

```lua
local me = entities.get_local()
if me then
    local health = memory.read_i32(me.pawn + offsets.get("m_iHealth"))
    client.log("health from memory: " .. health)
end
```
