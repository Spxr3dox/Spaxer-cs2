#pragma once

inline constexpr const char* kUiPrelude = R"lua(
local home = os.getenv("HOME") or "/tmp"
local schema_path = home .. "/.config/spaxer/lua_ui.txt"
local values_path = home .. "/.config/spaxer/lua_values.txt"
local scripts_dir = client.script_dir()

package.path = scripts_dir .. "/lib/?.lua;" .. scripts_dir .. "/lib/?/init.lua;" .. scripts_dir .. "/?.lua;" .. package.path

local elements, order = {}, {}
local stored = {}
local schema_dirty = true
local values_dirty = false
local last_poll = 0

local function sanitize(text) return (tostring(text):gsub("[\t\n\r]", " ")) end

local function script_key() return get_script_name() end

local function read_values()
    local out = {}
    local f = io.open(values_path, "r")
    if not f then return out end
    for line in f:lines() do
        local script, name, value = line:match("^([^\t]*)\t([^\t]*)\t(.*)$")
        if script then
            out[script] = out[script] or {}
            out[script][name] = value
        end
    end
    f:close()
    return out
end

local function write_values()
    local merged = read_values()
    for _, element in ipairs(order) do
        if element.kind ~= "label" then
            merged[element.script] = merged[element.script] or {}
            merged[element.script][element.name] = element.raw
        end
    end
    local lines = {}
    for script, values in pairs(merged) do
        for name, value in pairs(values) do lines[#lines + 1] = script .. "\t" .. name .. "\t" .. value end
    end
    table.sort(lines)
    local tmp = values_path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return end
    f:write(table.concat(lines, "\n"), "\n")
    f:close()
    os.rename(tmp, values_path)
end

local function write_schema()
    schema_dirty = false
    local lines = {}
    for _, element in ipairs(order) do
        if not element.removed then
            local fields = { element.script, element.kind, element.name, element.visible and "1" or "0" }
            for _, arg in ipairs(element.args) do fields[#fields + 1] = sanitize(arg) end
            lines[#lines + 1] = table.concat(fields, "\t")
        end
    end
    local tmp = schema_path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return end
    f:write(table.concat(lines, "\n"), "\n")
    f:close()
    os.rename(tmp, schema_path)
end

local key_aliases = {
    SHIFT_L = "LEFTSHIFT", SHIFT_R = "RIGHTSHIFT", CONTROL_L = "LEFTCTRL", CONTROL_R = "RIGHTCTRL",
    ALT_L = "LEFTALT", ALT_R = "RIGHTALT", RETURN = "ENTER", ESCAPE = "ESC", CAPS_LOCK = "CAPSLOCK",
    PRIOR = "PAGEUP", NEXT = "PAGEDOWN", BACKSPACE = "BACKSPACE", GRAVE = "GRAVE",
}

local function key_down(key)
    if not key or key == "" then return false end
    local upper = key:upper()
    local mouse = upper:match("^MOUSE(%d)$")
    if mouse then return input.is_mouse_down(tonumber(mouse)) end
    local code = keys[key_aliases[upper] or upper]
    return code ~= nil and input.is_key_down(code)
end

local decoders = {
    checkbox = function(raw) return raw == "1" end,
    slider = function(raw) return tonumber(raw) or 0 end,
    combo = function(raw) return math.floor(tonumber(raw) or 0) end,
    color = function(raw)
        local r, g, b, a = raw:match("^(%d+),(%d+),(%d+),(%d+)$")
        return color_t(tonumber(r) or 255, tonumber(g) or 255, tonumber(b) or 255, tonumber(a) or 255)
    end,
    bind = function(raw)
        local key, mode = raw:match("^([^|]*)|(.*)$")
        return { key = key or "", mode = mode ~= "" and mode or "hold" }
    end,
    button = function(raw) return tonumber(raw) or 0 end,
    input = function(raw) return raw end,
    label = function(raw) return raw end,
    multi = function(raw)
        local set = {}
        for index in raw:gmatch("%d+") do set[tonumber(index)] = true end
        return set
    end,
}

local encoders = {
    checkbox = function(v) return v and "1" or "0" end,
    slider = function(v) return tostring(v) end,
    combo = function(v) return tostring(math.floor(v)) end,
    color = function(c) return string.format("%d,%d,%d,%d", c.r, c.g, c.b, c.a) end,
    bind = function(v) return (v.key or "") .. "|" .. (v.mode or "hold") end,
    button = function(v) return tostring(v) end,
    input = function(v) return sanitize(v) end,
    label = function(v) return v end,
    multi = function(set)
        local list = {}
        for index, on in pairs(set) do if on then list[#list + 1] = index end end
        table.sort(list)
        return table.concat(list, ",")
    end,
}

local element_mt = { __name = "ui_element_t" }
element_mt.__index = element_mt

function element_mt:get()
    if self.kind == "bind" then return self.value.key end
    if self.kind == "multi" then
        local out = {}
        for index in pairs(self.value) do out[#out + 1] = self.items[index + 1] end
        return out
    end
    return self.value
end

function element_mt:set(value)
    if self.kind == "bind" then value = { key = value, mode = self.value.mode } end
    if self.kind == "multi" then
        local set = {}
        for _, item in ipairs(value) do
            for index, name in ipairs(self.items) do if name == item then set[index - 1] = true end end
        end
        value = set
    end
    self.value = value
    self.raw = encoders[self.kind](value)
    write_values()
    if self.callback then pcall(self.callback, self:get()) end
end

function element_mt:set_callback(fn)
    self.callback = fn
    return self
end

function element_mt:set_visible(visible)
    self.visible = visible ~= false
    schema_dirty = true
    return self
end

function element_mt:get_name() return self.name end

function element_mt:is_active()
    if self.kind ~= "bind" then return self.kind == "checkbox" and self.value == true end
    local mode = self.value.mode
    if mode == "always" then return true end
    local down = key_down(self.value.key)
    if mode == "hold" then return down end
    if mode == "toggle" then
        if down and not self.was_down then self.toggled = not self.toggled end
        self.was_down = down
        return self.toggled == true
    end
    return false
end

function element_mt:get_mode() return self.kind == "bind" and self.value.mode or nil end
function element_mt:set_mode(mode)
    if self.kind ~= "bind" then return end
    self.value.mode = mode
    self.raw = encoders.bind(self.value)
    write_values()
end

local function create(kind, name, default, args, extra)
    assert(type(name) == "string" and name ~= "", "element name must be a non-empty string")
    local script = script_key()
    local id = script .. "\t" .. name
    local existing = elements[id]
    if existing then return existing end
    local element = setmetatable({ script = script, kind = kind, name = name, args = args or {}, visible = true }, element_mt)
    for k, v in pairs(extra or {}) do element[k] = v end
    local saved = stored[script] and stored[script][name]
    element.raw = saved or encoders[kind](default)
    if not saved and kind ~= "label" then values_dirty = true end
    element.value = decoders[kind](element.raw)
    elements[id] = element
    order[#order + 1] = element
    schema_dirty = true
    return element
end

function ui.checkbox(name, default)
    return create("checkbox", name, default == true)
end

function ui.slider(name, min, max, default, step, suffix)
    min, max = min or 0, max or 100
    step = step or ((math.floor(min) == min and math.floor(max) == max) and 1 or 0.01)
    return create("slider", name, default or min, { min, max, step, suffix or "" })
end

function ui.combo(name, items, default)
    assert(type(items) == "table" and #items > 0, "combo needs a list of items")
    return create("combo", name, default or 0, { table.concat(items, "|") }, { items = items })
end

function ui.multi_combo(name, items, defaults)
    assert(type(items) == "table" and #items > 0, "multi combo needs a list of items")
    local set = {}
    for _, item in ipairs(defaults or {}) do
        for index, candidate in ipairs(items) do if candidate == item then set[index - 1] = true end end
    end
    return create("multi", name, set, { table.concat(items, "|") }, { items = items })
end

function ui.color(name, default)
    return create("color", name, default or color_t(255, 255, 255, 255))
end

function ui.bind(name, key, mode)
    return create("bind", name, { key = key or "", mode = mode or "hold" })
end

function ui.button(name, callback)
    local element = create("button", name, 0)
    element.callback = callback
    return element
end

function ui.input(name, default)
    return create("input", name, default or "")
end

function ui.label(text)
    return create("label", text, "", {})
end

ui.add_checkbox, ui.add_slider, ui.add_combo, ui.add_multi_combo = ui.checkbox, ui.slider, ui.combo, ui.multi_combo
ui.add_color_picker, ui.add_bind, ui.add_button, ui.add_input, ui.add_label = ui.color, ui.bind, ui.button, ui.input, ui.label

function _spx_ui_remove(script)
    for _, element in ipairs(order) do
        if element.script == script then element.removed = true end
    end
    schema_dirty = true
end

stored = read_values()
write_schema()

client.register_callback("frame", function()
    if values_dirty then
        values_dirty = false
        write_values()
    end
    if schema_dirty then write_schema() end
    local now = client.get_time()
    if now - last_poll < 0.15 then return end
    last_poll = now
    stored = read_values()
    for _, element in ipairs(order) do
        local raw = stored[element.script] and stored[element.script][element.name]
        if raw and raw ~= element.raw and element.kind ~= "label" then
            local before = element.raw
            element.raw = raw
            element.value = decoders[element.kind](raw)
            if element.callback then
                if element.kind == "button" then
                    if (tonumber(raw) or 0) > (tonumber(before) or 0) then pcall(element.callback) end
                else
                    pcall(element.callback, element:get())
                end
            end
        end
    end
end)
)lua";
