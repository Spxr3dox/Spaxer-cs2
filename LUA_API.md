# Spaxer Lua API

Spaxer runs Lua scripts on LuaJIT. The API follows the same layout and naming as the popular CS2 script APIs (`render`, `engine`, `entitylist`, `ui`, `cvars`, typed vectors and colors), so scripts written for them port over with little or no change. On top of that, Spaxer adds its own modules for its features: `config`, `input`, `memory`, `client` and `entities`.

Spaxer is an **external** client: it reads game memory from outside and draws on its own overlay. A few things that need code running inside the game process are not available and are marked as such below.

## Getting started

Put `.lua` files into `~/.config/spaxer/scripts/`. They load in alphabetical order when the overlay starts. The **Scripts** page of the settings window lists them, lets you turn each one on or off (a disabled script is renamed to `name.lua.disabled`) and has a **Reload scripts** button.

Scripts run with the JIT compiler off, so they can always be interrupted. Every call gets a **50 ms budget**: a script that loops forever is stopped instead of freezing the overlay. A callback that throws is logged and switched off. `print` and errors go to the overlay's stderr, prefixed with `[LUA]`.

The `ffi` library is available, but game memory lives in another process: read it with `memory.*` or netvar fields, not with FFI pointer casts.

```lua
local font = render.setup_font("Roboto", 16, 1)

register_callback("paint", function()
    local pawn = entitylist.get_local_player_pawn()
    if not pawn then return end
    render.text("HP: " .. pawn.m_iHealth, font, vec2_t(20, 20), color_t(80, 140, 255))
end)
```

## Events

Register handlers with `register_callback(name, fn)`.

| Event | Arguments | When it fires |
|---|---|---|
| `paint` | none | Every overlay frame while CS2 is focused. The only place where `render.*` works. |
| `game_event` | `game_event_t` | When Spaxer sees one of the events listed below. |
| `unload` | none | Before the script is unloaded or scripts are reloaded. |
| `frame` | none | Every display frame, even when CS2 isn't focused. Spaxer extension. |
| `tick` | none | About every 100 ms. Spaxer extension. |
| `override_view`, `console_input`, `player_chat`, `string_cmd` | | **Not available.** They need hooks inside the game. Registering them is allowed but they never fire. |

Spaxer does not hook the game's event manager. Instead it detects events from memory and raises them itself:

| Event | Keys |
|---|---|
| `player_hurt` | `userid`, `attacker`, `dmg_health`, `health` |
| `player_death` | `userid`, `attacker` |
| `bomb_planted` | `site` (0 = A, 1 = B) |
| `bomb_defused` | `site` |
| `bomb_exploded` | `site` |

`userid` and `attacker` are controller indices, so `event:get_controller("userid")` and `event:get_pawn("userid")` work. `player_hurt` fires for any drop in an enemy's health, and `attacker` is always the local player.

```lua
register_callback("game_event", function(event)
    if event:get_name() == "player_hurt" then
        print("hit for " .. event:get_int("dmg_health") .. ", " .. event:get_int("health") .. " left")
    end
end)
```

## ui

### Script settings

Scripts can add their own settings to the settings window. Each script gets its own card on the **Scripts** page, named after the script file, and every control you create appears there in the order you created it.

Values are remembered between restarts, saved into configs together with the rest of your settings (as `configs/<name>.lua.txt`) and restored when a config is loaded. Autosave picks up changes to them too.

| Function | Returns | Control |
|---|---|---|
| `ui.checkbox(name, default?)` | element | Switch. `get()` returns a boolean. |
| `ui.slider(name, min, max, default?, step?, suffix?)` | element | Slider. `step` defaults to 1 for whole-number ranges. `suffix` is shown after the value, for example `"px"` or `"%"`. |
| `ui.combo(name, items, default?)` | element | Drop-down list. `get()` returns the selected index, starting at 0. |
| `ui.multi_combo(name, items, defaults?)` | element | Drop-down with checkboxes. `get()` returns the list of selected item names. |
| `ui.color(name, default?)` | element | Color picker with alpha. `get()` returns a `color_t`. |
| `ui.bind(name, key?, mode?)` | element | Key bind. Left click to set a key or mouse button (Escape clears it), right click to choose `hold`, `toggle` or `always`. |
| `ui.button(name, callback)` | element | Button that calls `callback` when pressed. |
| `ui.input(name, default?)` | element | Text field. `get()` returns the text. |
| `ui.label(text)` | element | Hint text. |

`ui.add_checkbox`, `ui.add_slider`, `ui.add_combo`, `ui.add_multi_combo`, `ui.add_color_picker`, `ui.add_bind`, `ui.add_button`, `ui.add_input` and `ui.add_label` are the same functions under longer names.

Every element has these methods:

| Method | Description |
|---|---|
| `element:get()` | Current value. |
| `element:set(value)` | Changes the value. The settings window updates too. |
| `element:set_callback(fn)` | `fn(value)` runs whenever the value changes, whether from the settings window or from `set`. Returns the element, so it can be chained. |
| `element:set_visible(visible)` | Shows or hides the control. Handy for options that only matter when another one is on. |
| `element:get_name()` | The name the element was created with. |
| `element:is_active()` | For binds: whether the bind is active right now, following its mode. For checkboxes: the same as `get()`. |
| `element:get_mode()` / `element:set_mode(mode)` | Bind mode: `"hold"`, `"toggle"` or `"always"`. |

```lua
local enabled = ui.checkbox("Enabled", true)
local key = ui.bind("Show while held", "MOUSE5", "hold")
local size = ui.slider("Size", 8, 64, 20, 1, "px")
local style = ui.combo("Style", { "Circle", "Square" }, 0)
local tint = ui.color("Color", color_t(76, 141, 255))

style:set_callback(function(index) print("style is now " .. index) end)

register_callback("paint", function()
    if not enabled:get() or not key:is_active() then return end
    local center = render.screen_size() / 2
    if style:get() == 0 then
        render.circle(center, size:get(), 32, tint:get(), 2)
    else
        local half = vec2_t(size:get(), size:get())
        render.rect(center - half, center + half, tint:get(), 0, 2)
    end
end)
```

### Menu state

| Function | Returns | Description |
|---|---|---|
| `ui.is_menu_opened()` | `boolean` | Whether the settings window is shown. |
| `ui.get_menu_rect()` | `vec4_t` | Window bounds as `x, y` (top-left) and `z, w` (bottom-right). |

## Libraries and `require`

`require` looks in `scripts/lib/` first, then in `scripts/` itself, so shared code can live in its own files:

```lua
-- scripts/lib/colors.lua
local colors = {}
function colors.accent(alpha) return color_t(76, 141, 255, alpha or 255) end
return colors
```

```lua
-- scripts/my_script.lua
local colors = require("colors")
```

Files inside `scripts/lib/` are not loaded as scripts on their own: they run only when something requires them.

## render

Every function here works only inside the `paint` callback. Colors are `color_t` values in the 0–255 range.

**Information**

| Function | Returns | Description |
|---|---|---|
| `render.screen_size()` | `vec2_t` | Overlay size in pixels. |
| `render.frame_count()` | `number` | Frames drawn since the overlay started. |
| `render.frame_time()` | `number` | Seconds since the previous frame. |
| `render.world_to_screen(pos: vec3_t)` | `vec2_t` or `nil` | Projects a world point with the same camera the ESP uses. `nil` when the point is behind you. |

**Fonts and textures**

| Function | Returns | Description |
|---|---|---|
| `render.setup_font(file_or_family, size, flags?)` | `font_t` | Pass a font file path (`.ttf`, `.otf`) or an installed family name such as `"Roboto"`. Any non-zero `flags` makes it bold. |
| `render.calc_text_size(text, font, size?)` | `vec2_t` | Size of the text as it would be drawn. |
| `render.setup_texture(filename)` | `texture_t` | Loads PNG, JPEG, WebP, BMP and other common formats. |
| `render.setup_texture_rgba(buffer, size: vec2_t)` | `texture_t` | Builds a texture from a flat `{r, g, b, a, ...}` array. |
| `render.setup_texture_from_memory(buffer)` | `texture_t` | Decodes an encoded image from a byte string or a byte array. |

**Drawing**

| Function | Description |
|---|---|
| `render.text(text, font, pos, color?, size?)` | Text with its top-left corner at `pos`. |
| `render.line(from, to, color, thickness?)` | Line. |
| `render.rect(from, to, color, rounding?, thickness?)` | Rectangle outline. |
| `render.rect_filled(from, to, color, rounding?)` | Filled rectangle. |
| `render.rect_filled_fade(from, to, top_left, top_right, bottom_right, bottom_left)` | Rectangle with a separate color at each corner. |
| `render.circle(pos, radius, segments, color, thickness?)` | Circle outline. Spaxer draws true curves, so `segments` is ignored. |
| `render.circle_filled(pos, radius, segments, color)` | Filled circle. |
| `render.circle_fade(pos, radius, color_in, color_out)` | Circle fading from the center color to the edge color. |
| `render.arc(pos, radius, a_min, a_max, segments, color, thickness?)` | Arc between two angles in degrees. |
| `render.polygon(points, color)` | Filled polygon from a list of `vec2_t`. |
| `render.concave_polygon(points, color)` | Same as `polygon`; concave shapes are handled. |
| `render.poly_line(points, color, thickness?)` | Open line through the points. |
| `render.texture(texture, from, to, color?, rounding?)` | Draws a texture stretched between two corners. Only the alpha of `color` is used. |
| `render.push_clip_rect(from, to, intersect?)` | Limits drawing to a rectangle. `intersect` (default `true`) combines it with the current clip. |
| `render.pop_clip_rect()` | Removes the last clip. |
| `render.circle_3d(pos, radius, color, thickness?, normal?)` | Circle in the world, facing `normal` (up by default). |
| `render.circle_filled_3d(pos, radius, color, normal?)` | Filled world circle. |
| `render.circle_fade_3d(pos, radius, color_in, color_out, normal?)` | World circle with a fade. |

## engine

| Function | Returns | Description |
|---|---|---|
| `engine.get_level_name()` | `string` | Current map, for example `de_mirage`. Empty in the menu. |
| `engine.camera_in_thirdperson()` | `boolean` | Whether Spaxer's third person is on. |
| `engine.get_product_version_string()` | `string` | CS2 patch version from `steam.inf`. |
| `engine.play_sound(name, volume)` | | Plays a sound file through PipeWire. `name` can be a full path or a path inside `csgo/sounds/`. `volume` is 0–1. |
| `engine.chat_print(text)` | | Shows the text as a Spaxer HUD notification (the in-game chat can't be written from outside). |
| `engine.get_hitbox_pos(pawn, index)` | `vec3_t` or `nil` | Position of a hitbox (see the table below). |
| `engine.get_hitbox(pawn, index)` | `hitbox_t` or `nil` | Hitbox with an approximate box around it (`mins`, `maxs`, `shape_radius`, `bone_name`, `hitbox_index`). |
| `engine.get_netvar_offset(module, class, field)` | `number` or `nil` | Offset of a schema field. `module` is accepted for compatibility and ignored. |
| `engine.trace_shape(ray, start, end, filter)` | `trace_t` | Ray cast against the real map geometry (see **Traces**). |
| `engine.trace_bullet(from_pawn, from, to)` | `number` or `nil` | Damage a bullet from `from_pawn`'s current weapon would still do at `to`, including wall penetration. `nil` if it can't get through. |
| `engine.execute_client_cmd(command)` | `false` | **Not available** in an external client. |
| `engine.get_net_channel()` | `nil` | **Not available.** Use `engine.get_ping()` instead. |

Spaxer extensions: `engine.is_in_game()`, `engine.is_focused()`, `engine.get_ping()`, `engine.get_fps()`, `engine.get_local_team()` (2 = T, 3 = CT), `engine.get_screen_size()` (returns `w, h`).

**Hitbox indices**

| Index | Hitbox | Index | Hitbox |
|---|---|---|---|
| 0 | head | 10 | left calf |
| 1 | neck | 11 | right foot |
| 2 | pelvis | 12 | left foot |
| 3 | stomach | 13 | right hand |
| 4 | lower chest | 14 | left hand |
| 5 | chest | 15 | right upper arm |
| 6 | upper chest | 16 | right forearm |
| 7 | right thigh | 17 | left upper arm |
| 8 | left thigh | 18 | left forearm |
| 9 | right calf | | |

**Traces**

`ray_t(...)` and `trace_filter_t(...)` exist so ported scripts run, but Spaxer traces a thin line: the ray's shape and the filter's layers are ignored. Pass `trace_filter_t("grenades", ...)` to trace against grenade collision instead of line of sight. The returned `trace_t` fills `start_pos`, `end_pos`, `hit_point`, `hit_normal`, `fraction` (1 means nothing was hit) and `contents` (1 if something was hit). Entities are not traced, so `hit_entity` is always `nil`.

Traces need the converted map collision (`python3 tools/map_collision.py`, see the README).

## entitylist

| Function | Returns | Description |
|---|---|---|
| `entitylist.get_local_player_controller()` | `base_entity_t` | Your controller, or `nil` outside a match. |
| `entitylist.get_local_player_pawn()` | `base_entity_t` | Your pawn, or `nil` when dead or in the menu. |
| `entitylist.get_entity_from_handle(handle)` | `base_entity_t` | Entity behind a `CHandle` value. |
| `entitylist.get_game_rules()` | `base_entity_t` | The `C_CSGameRules` object. |
| `entitylist.get_entities(class_name?, include_inherits?)` | `base_entity_t[]` | Entities of a class, or all entities without a filter. |
| `entitylist.get_entities(class_name?, include_inherits?, callback)` | | Same, but calls `callback(entity)` for each one. |

Class names follow the game's schema names: `C_CSPlayerPawn`, `CCSPlayerController`, `C_PlantedC4`, `C_C4`, `C_SmokeGrenadeProjectile`, `C_WeaponAWP` and so on. With `include_inherits`, `C_CSWeaponBase` matches every weapon and `C_BaseCSGrenadeProjectile` every thrown grenade. The designer name (`"weapon_ak47"`, `"planted_c4"`) is accepted as a class name too.

```lua
entitylist.get_entities("C_CSPlayerPawn", false, function(pawn)
    if pawn.m_iHealth > 0 then
        print(pawn:get_abs_origin())
    end
end)
```

## base_entity_t

| Member | Returns | Description |
|---|---|---|
| `entity.m_<field>` | typed value | Reads any schema field by name (see **Netvars**). Assigning writes it back. |
| `entity[offset]` | `number` | Address of the entity plus `offset`, for use with `memory.*`. |
| `entity:get_abs_origin()` | `vec3_t` | World position. |
| `entity:get_abs_rotation()` | `angle_t` | World rotation. |
| `entity:get_class_name()` | `string` | Schema class name. |
| `entity:get_designer_name()` | `string` | Designer name such as `weapon_ak47`. Spaxer extension. |
| `entity:get_entity_handle()` | `number` | Entity handle. |
| `entity:get_address()` | `number` | Raw address in game memory. Spaxer extension. |

## Netvars

Schema fields are read by name and converted by their type: integers and floats become numbers, `bool` becomes a boolean, `Vector` becomes `vec3_t`, `QAngle` becomes `angle_t`, `Color` becomes `color_t`, strings become Lua strings, handles stay numbers, pointers and embedded structures become `base_entity_t`, so you can keep walking:

```lua
local pawn = entitylist.get_local_player_pawn()
local weapon = entitylist.get_entity_from_handle(pawn.m_pWeaponServices.m_hActiveWeapon)
print(weapon:get_class_name(), weapon.m_iClip1)
pawn.m_flFlashMaxAlpha = 0
```

The offset dumper that runs inside CS2 at launch writes every field of the common classes to `/tmp/schema_full.txt`: entities, pawns, controllers, weapons, grenades, the bomb, game rules and player services. Until the game has been started with the current dumper, only the fields Spaxer itself uses are available.

## cvars

`cvars.NAME` returns a `convar_t`. Spaxer reads console variables from your saved CS2 config (`cs2_user_convars` and `cs2_machine_convars`), so values change after the game saves them, which happens when you close the game or leave settings. Only saved, non-default values are known. Variables that aren't there return empty values.

| Method | Returns |
|---|---|
| `get_name()` | `string` |
| `get_desc()` | `string` (always empty) |
| `get_string()` | `string` |
| `get_float()` | `number` |
| `get_int()` | `number` |
| `get_bool()` | `boolean` |

```lua
print("sensitivity: " .. cvars.sensitivity:get_float())
```

## math

These are added to Lua's `math` table.

| Function | Returns | Description |
|---|---|---|
| `math.calc_angle(src: vec3_t, dst: vec3_t)` | `angle_t` | Angle that points from `src` to `dst`. |
| `math.calc_fov(src: angle_t, dst: angle_t)` | `number` | Angle in degrees between two view directions. |
| `math.normalize_angle(angle)` | `number` | Wraps an angle into −180…180. |
| `math.vector_angles(forward: vec3_t)` | `angle_t` | Direction vector to angles. |
| `math.angle_vectors(angles: angle_t)` | `vec3_t, vec3_t, vec3_t` | Forward, right and up vectors. |

## Types

| Type | Constructor | Fields | Methods |
|---|---|---|---|
| `vec2_t` | `vec2_t(x, y)` | `x`, `y` | `length`, `length_sqr`, `dist_to`, `lerp`, `normalized` |
| `vec3_t` | `vec3_t(x, y, z)` | `x`, `y`, `z` | `length`, `length_sqr`, `length_2d`, `length_2d_sqr`, `dist_to`, `dist_to_2d`, `dot`, `cross`, `lerp`, `normalized`, `normalize` (in place, returns the old length) |
| `vec4_t` | `vec4_t(x, y, z, w)` | `x`, `y`, `z`, `w` | |
| `angle_t` | `angle_t(pitch, yaw, roll)` | `pitch`, `yaw`, `roll` | |
| `color_t` | `color_t(r, g, b, a?)` | `r`, `g`, `b`, `a` (0–255, `a` defaults to 255) | `lerp(other, fraction)` |

Vectors and angles support `+`, `-`, `*`, `/` with each other or with a number, unary `-` and `==`. Every type prints nicely with `tostring`.

`font_t` is a table with `family`, `size` and `bold`. `texture_t` has `id` and `size` (`vec2_t`).

## environment

| Function | Returns | Description |
|---|---|---|
| `register_callback(name, fn)` | | See **Events**. |
| `find_pattern(module, pattern, offset?)` | `number` or `nil` | Scans a loaded CS2 module (for example `"libclient.so"` or just `"client"`) for an IDA-style pattern where `?` is a wildcard byte. Returns the address plus `offset`. |
| `find_export(module, name)` | `number` or `nil` | Address of an exported symbol of a CS2 module. |
| `get_user_name()` | `string` | Your Steam name. |
| `get_game_directory()` | `string` | The CS2 `game` folder. |
| `get_script_name()` | `string` | Name of the current script without `.lua`. |
| `unload_script()` | | Runs the script's `unload` handlers and stops all of its callbacks. |
| `color_print(text, color?)` | | Prints to the log in the nearest terminal color. End the text with `"\0"` to skip the newline. |

Addresses returned by `find_pattern` and `find_export` belong to the game process: use them with `memory.*`.

## Spaxer extensions

### config: every feature and setting

Every setting from the settings window is reachable by name.

| Function | Description |
|---|---|
| `config.get(name)` | Reads a setting. |
| `config.set(name, value)` | Changes a setting. The change applies everywhere instantly, the settings window included. |
| `config.toggle(name)` | Flips a toggle and returns the new state. |
| `config.list()` | Every setting as `{ name, type }`, where `type` is `toggle`, `number`, `color` or `bind`. |
| `config.save(name)` / `config.load(name)` | Saves or loads a config. Returns `true` on success. |
| `config.configs()` | Names of the saved configs. |

Toggles are booleans, numbers are numbers, colors are `0xRRGGBBAA` or `{r, g, b, a}`, binds are X11 key names (`"F4"`) or `nil`. `aimbot_fov`, `aimbot_smooth`, `rcs_strength` and `trigger_fov` also work in plain units instead of their `_x100` fields. An unknown name raises an error.

### input: binds, keys and mouse

| Function | Description |
|---|---|
| `input.bind(key, fn, mode?, global?)` | Binds a key and returns its id. `key` is a name (`"F6"`, `"SPACE"`, `"MOUSE4"`) or a key code. `mode` is `"press"` (default), `"release"`, `"hold"` (`fn` gets `true`/`false`) or `"toggle"` (`fn` gets the new state). Binds only fire while CS2 is focused unless `global` is `true`. |
| `input.unbind(id)` | Removes a bind. |
| `input.is_pressed(key)` | Whether a key is held right now. |
| `input.is_key_down(code)` / `input.is_mouse_down(button)` | Raw key (Linux key code) and mouse (1 left, 2 middle, 3 right, 4 back, 5 forward) state. |
| `input.set_key(code, down)` / `input.press_key(code)` | Holds, releases or taps a key on the virtual keyboard. |
| `input.set_mouse_button(button, down)` / `input.click(button?)` / `input.click_left()` | Mouse buttons. |
| `input.mouse_move(dx, dy)` | Relative mouse movement. |

The global `keys` table maps key names to codes (`keys.SPACE`, `keys.F5`, `keys.LEFTCTRL`).

### client: timers and script control

| Function | Description |
|---|---|
| `client.delay(seconds, fn)` | Runs `fn` once after a delay. Returns a timer id. |
| `client.every(seconds, fn)` | Runs `fn` repeatedly. Returns a timer id. |
| `client.cancel(id)` | Stops a timer. |
| `client.log(text)` | Prints a line to the log. |
| `client.get_time()` | Monotonic time in seconds. |
| `client.load_script(path)` | Runs another Lua file. |
| `client.reload()` | Reloads all scripts on the next frame. |
| `client.script_dir()` | Path of the scripts folder. |

### memory and offsets

Direct access to CS2 memory. Addresses are plain numbers.

- Read: `read_u8`, `read_bool`, `read_i32`, `read_u32`, `read_i64`, `read_u64`, `read_ptr`, `read_float`, `read_vec3`, `read_string(address, max_length)`
- Write: `write_u8`, `write_bool`, `write_i32`, `write_u32`, `write_u64`, `write_float`, `write_vec3(address, {x, y, z})`
- Module bases: `memory.get_client_base()`, `memory.get_engine_base()`

`offsets.get(name)` returns an offset Spaxer itself uses, `offsets.list()` lists them all.

### entities: ready-made game state

The same data the ESP uses, already filtered and cached.

| Function | Returns |
|---|---|
| `entities.get_players()` | `{ pawn, name, weapon, model, hp, team, visible, spotted, head_pos, feet_pos }` for every player |
| `entities.get_local()` | `{ pawn, controller, hp, team, weapon_id, scoped, on_ground, crouching, origin, eye, angles, velocity }` or `nil` |
| `entities.get_bomb()` | `{ visible, blow_secs, site, being_defused, defuse_secs }` |
| `entities.get_spectators()` | `{ name, team }` for everyone watching you |
| `entities.get_dropped_items()` | `{ name, x, y, z }` for weapons on the ground |
