#pragma once

inline constexpr const char* kCompatPrelude = R"lua(
local spx, draw = _spx, _spx_draw
local unpack = unpack or table.unpack

local function make_type(name, fields)
    local mt = { __name = name }
    mt.__index = mt
    local ctor = setmetatable({ mt = mt }, {
        __call = function(_, ...)
            local args, obj = { ... }, {}
            for i, field in ipairs(fields) do obj[field] = args[i] or 0 end
            return setmetatable(obj, mt)
        end,
    })
    return ctor, mt
end

local function is(value, ctor) return type(value) == "table" and getmetatable(value) == ctor.mt end

vec2_t, vec2_mt = make_type("vec2_t", { "x", "y" })
vec3_t, vec3_mt = make_type("vec3_t", { "x", "y", "z" })
vec4_t, vec4_mt = make_type("vec4_t", { "x", "y", "z", "w" })
angle_t, angle_mt = make_type("angle_t", { "pitch", "yaw", "roll" })
color_t, color_mt = make_type("color_t", { "r", "g", "b", "a" })

local color_ctor = getmetatable(color_t).__call
getmetatable(color_t).__call = function(self, r, g, b, a)
    local c = color_ctor(self, r or 255, g or 255, b or 255, a or 255)
    if a == nil then c.a = 255 end
    return c
end

local function fmt(n) return string.format("%.3f", n) end
function vec2_mt.__tostring(v) return "vec2_t(" .. fmt(v.x) .. ", " .. fmt(v.y) .. ")" end
function vec3_mt.__tostring(v) return "vec3_t(" .. fmt(v.x) .. ", " .. fmt(v.y) .. ", " .. fmt(v.z) .. ")" end
function vec4_mt.__tostring(v) return "vec4_t(" .. fmt(v.x) .. ", " .. fmt(v.y) .. ", " .. fmt(v.z) .. ", " .. fmt(v.w) .. ")" end
function angle_mt.__tostring(v) return "angle_t(" .. fmt(v.pitch) .. ", " .. fmt(v.yaw) .. ", " .. fmt(v.roll) .. ")" end
function color_mt.__tostring(c) return "color_t(" .. c.r .. ", " .. c.g .. ", " .. c.b .. ", " .. c.a .. ")" end

local function arith(mt, ctor, keys)
    local function combine(a, b, op)
        local out = {}
        for i, k in ipairs(keys) do
            local av = type(a) == "number" and a or a[k]
            local bv = type(b) == "number" and b or b[k]
            out[i] = op(av, bv)
        end
        return ctor(unpack(out))
    end
    mt.__add = function(a, b) return combine(a, b, function(x, y) return x + y end) end
    mt.__sub = function(a, b) return combine(a, b, function(x, y) return x - y end) end
    mt.__mul = function(a, b) return combine(a, b, function(x, y) return x * y end) end
    mt.__div = function(a, b) return combine(a, b, function(x, y) return x / y end) end
    mt.__unm = function(a) return combine(a, -1, function(x, y) return x * y end) end
    mt.__eq = function(a, b)
        for _, k in ipairs(keys) do if a[k] ~= b[k] then return false end end
        return true
    end
end
arith(vec2_mt, vec2_t, { "x", "y" })
arith(vec3_mt, vec3_t, { "x", "y", "z" })
arith(vec4_mt, vec4_t, { "x", "y", "z", "w" })
arith(angle_mt, angle_t, { "pitch", "yaw", "roll" })

function vec2_mt:length() return math.sqrt(self.x * self.x + self.y * self.y) end
function vec2_mt:length_sqr() return self.x * self.x + self.y * self.y end
function vec2_mt:dist_to(o) return (self - o):length() end
function vec2_mt:lerp(o, t) return self + (o - self) * t end
function vec2_mt:normalized()
    local l = self:length()
    if l == 0 then return vec2_t(0, 0) end
    return self / l
end

function vec3_mt:length_sqr() return self.x * self.x + self.y * self.y + self.z * self.z end
function vec3_mt:length() return math.sqrt(self:length_sqr()) end
function vec3_mt:length_2d_sqr() return self.x * self.x + self.y * self.y end
function vec3_mt:length_2d() return math.sqrt(self:length_2d_sqr()) end
function vec3_mt:dist_to(o) return (self - o):length() end
function vec3_mt:dist_to_2d(o) return (self - o):length_2d() end
function vec3_mt:dot(o) return self.x * o.x + self.y * o.y + self.z * o.z end
function vec3_mt:cross(o) return vec3_t(self.y * o.z - self.z * o.y, self.z * o.x - self.x * o.z, self.x * o.y - self.y * o.x) end
function vec3_mt:lerp(o, t) return self + (o - self) * t end
function vec3_mt:normalized()
    local l = self:length()
    if l == 0 then return vec3_t(0, 0, 0) end
    return self / l
end
function vec3_mt:normalize()
    local l = self:length()
    if l ~= 0 then self.x, self.y, self.z = self.x / l, self.y / l, self.z / l end
    return l
end

function color_mt:lerp(o, t)
    return color_t(self.r + (o.r - self.r) * t, self.g + (o.g - self.g) * t, self.b + (o.b - self.b) * t, self.a + (o.a - self.a) * t)
end

local function rgba(c)
    if c == nil then return 255, 255, 255, 255 end
    return c.r, c.g, c.b, c.a
end

local rad, deg = math.pi / 180, 180 / math.pi

function math.normalize_angle(a)
    a = math.fmod(a + 180, 360)
    if a < 0 then a = a + 360 end
    return a - 180
end

function math.vector_angles(forward)
    local yaw, pitch
    if forward.x == 0 and forward.y == 0 then
        yaw = 0
        pitch = forward.z > 0 and 270 or 90
    else
        yaw = math.atan2(forward.y, forward.x) * deg
        if yaw < 0 then yaw = yaw + 360 end
        pitch = math.atan2(-forward.z, math.sqrt(forward.x * forward.x + forward.y * forward.y)) * deg
        if pitch < 0 then pitch = pitch + 360 end
    end
    return angle_t(math.normalize_angle(pitch), math.normalize_angle(yaw), 0)
end

function math.angle_vectors(a)
    local sp, cp = math.sin(a.pitch * rad), math.cos(a.pitch * rad)
    local sy, cy = math.sin(a.yaw * rad), math.cos(a.yaw * rad)
    local sr, cr = math.sin(a.roll * rad), math.cos(a.roll * rad)
    local forward = vec3_t(cp * cy, cp * sy, -sp)
    local right = vec3_t(-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp)
    local up = vec3_t(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp)
    return forward, right, up
end

function math.calc_angle(src, dst) return math.vector_angles(dst - src) end

function math.calc_fov(src, dst)
    local a = math.angle_vectors(src)
    local b = math.angle_vectors(dst)
    local dot = math.max(-1, math.min(1, a:dot(b)))
    return math.acos(dot) * deg
end

local owners, unloaded, handlers = {}, {}, {}
local known_events = { paint = true, unload = true, game_event = true, frame = true, tick = true,
    override_view = false, console_input = false, player_chat = false, string_cmd = false }
local warned = {}

local function script_name() return _SPX_SCRIPT or "" end

local function run_owned(owner, fn, ...)
    if unloaded[owner] then return end
    local previous = _SPX_SCRIPT
    _SPX_SCRIPT = owner
    local ok, result = pcall(fn, ...)
    _SPX_SCRIPT = previous
    if not ok then error(result, 0) end
    return result
end

function register_callback(name, fn)
    assert(type(name) == "string", "event name must be a string")
    assert(type(fn) == "function", "callback must be a function")
    local owner = script_name()
    if known_events[name] == false and not warned[name] then
        warned[name] = true
        client.log("event '" .. name .. "' is not available in an external client and will never fire")
    end
    handlers[name] = handlers[name] or {}
    table.insert(handlers[name], { owner = owner, fn = fn })
    if name == "paint" or name == "unload" or name == "frame" or name == "tick" then
        client.register_callback(name, function() run_owned(owner, fn) end)
    end
end

function get_script_name()
    local name = script_name()
    return ((name:match("([^/]+)$") or name):gsub("%.lua$", ""))
end

function unload_script()
    local owner = script_name()
    if unloaded[owner] then return end
    for _, entry in ipairs(handlers.unload or {}) do
        if entry.owner == owner then pcall(entry.fn) end
    end
    unloaded[owner] = true
    if _spx_ui_remove then _spx_ui_remove(get_script_name()) end
end

local color_codes = { { 255, 0, 0, 31 }, { 0, 255, 0, 32 }, { 255, 255, 0, 33 }, { 0, 0, 255, 34 }, { 255, 0, 255, 35 }, { 0, 255, 255, 36 } }
function color_print(text, color)
    text = tostring(text)
    local newline = text:sub(-1) ~= "\0"
    if not newline then text = text:sub(1, -2) end
    local code = 37
    if color then
        local best = math.huge
        for _, c in ipairs(color_codes) do
            local d = (c[1] - color.r) ^ 2 + (c[2] - color.g) ^ 2 + (c[3] - color.b) ^ 2
            if d < best then best, code = d, c[4] end
        end
    end
    io.stderr:write("\27[" .. code .. "m[LUA] " .. text .. "\27[0m" .. (newline and "\n" or ""))
end

function print(...)
    local parts = {}
    for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
    client.log(table.concat(parts, "\t"))
end

find_pattern = spx.find_pattern
find_export = spx.find_export
get_user_name = spx.user_name
get_game_directory = spx.game_dir

ui = {}
function ui.is_menu_opened() return (spx.menu()) == true end
function ui.get_menu_rect()
    local open, x, y, w, h = spx.menu()
    return vec4_t(x or 0, y or 0, (x or 0) + (w or 0), (y or 0) + (h or 0))
end

local class_by_designer = {
    cs_player_controller = "CCSPlayerController", player = "C_CSPlayerPawn", cs_player_pawn = "C_CSPlayerPawn",
    observer = "C_CSObserverPawn", cs_observer_pawn = "C_CSObserverPawn", cs_gamerules_proxy = "C_CSGameRulesProxy",
    planted_c4 = "C_PlantedC4", weapon_c4 = "C_C4", cs_gamerules = "C_CSGameRulesProxy", cs_team_manager = "C_CSTeam",
    smokegrenade_projectile = "C_SmokeGrenadeProjectile", flashbang_projectile = "C_FlashbangProjectile",
    hegrenade_projectile = "C_HEGrenadeProjectile", molotov_projectile = "C_MolotovProjectile",
    decoy_projectile = "C_DecoyProjectile", inferno = "C_Inferno", chicken = "C_Chicken", hostage_entity = "C_Hostage",
    post_processing_volume = "C_PostProcessingVolume", env_fog_controller = "C_FogController", env_sky = "C_EnvSky",
    weapon_awp = "C_WeaponAWP", weapon_ak47 = "C_AK47", weapon_deagle = "C_DEagle", weapon_m4a1 = "C_WeaponM4A1",
    weapon_m4a1_silencer = "C_WeaponM4A1Silencer", weapon_glock = "C_WeaponGlock", weapon_hkp2000 = "C_WeaponHKP2000",
    weapon_usp_silencer = "C_WeaponUSPSilencer", weapon_ssg08 = "C_WeaponSSG08", weapon_hegrenade = "C_HEGrenade",
    weapon_flashbang = "C_Flashbang", weapon_smokegrenade = "C_SmokeGrenade", weapon_molotov = "C_MolotovGrenade",
    weapon_incgrenade = "C_IncendiaryGrenade", weapon_decoy = "C_DecoyGrenade",
}

local parents = {
    CCSPlayerController = "CBasePlayerController", CBasePlayerController = "C_BaseModelEntity",
    C_CSPlayerPawn = "C_CSPlayerPawnBase", C_CSPlayerPawnBase = "C_BasePlayerPawn", C_BasePlayerPawn = "C_BaseCombatCharacter",
    C_BaseCombatCharacter = "C_BaseFlex", C_BaseFlex = "C_BaseAnimGraph", C_BaseAnimGraph = "C_BaseModelEntity",
    C_BaseModelEntity = "C_BaseEntity", C_BaseEntity = "CEntityInstance",
    C_CSWeaponBaseGun = "C_CSWeaponBase", C_CSWeaponBase = "C_BasePlayerWeapon", C_BasePlayerWeapon = "C_EconEntity",
    C_EconEntity = "C_BaseFlex", C_BaseCSGrenade = "C_CSWeaponBase", C_C4 = "C_CSWeaponBase",
    C_BaseCSGrenadeProjectile = "C_BaseGrenade", C_BaseGrenade = "C_BaseFlex", C_PlantedC4 = "C_BaseAnimGraph",
}

local function class_name_for(designer)
    local known = class_by_designer[designer]
    if known then return known end
    if designer:match("^weapon_") then
        local grenade = designer:match("grenade") or designer:match("flashbang") or designer:match("molotov") or designer:match("decoy")
        return grenade and "C_BaseCSGrenade" or "C_CSWeaponBaseGun"
    end
    if designer:match("_projectile$") then return "C_BaseCSGrenadeProjectile" end
    if designer:match("gamerules") then return "C_CSGameRulesProxy" end
    if designer:match("player_pawn") then return "C_CSPlayerPawn" end
    return "C_BaseEntity"
end

local function inherits(class, wanted)
    while class do
        if class == wanted then return true end
        class = parents[class]
    end
    return false
end

base_entity_mt = { __name = "base_entity_t" }
local entity_methods = {}

local function wrap(address)
    if not address or address == 0 then return nil end
    return setmetatable({ __address = address }, base_entity_mt)
end

local function read_typed(address, typename)
    local t = typename or "?"
    if t == "bool" then return memory.read_bool(address) end
    if t == "float32" or t == "float" or t == "GameTime_t" or t:match("^CNetworkedQuantizedFloat") then return memory.read_float(address) end
    if t == "int32" or t == "int" or t == "GameTick_t" or t:match("^Move") or t:match("^Render") or t:match("^Collision") then return memory.read_i32(address) end
    if t == "uint32" or t:match("^CHandle") or t:match("^CEntityHandle") then return memory.read_u32(address) end
    if t == "uint8" or t == "int8" or t == "char" or t == "byte" then return memory.read_u8(address) end
    if t == "uint16" or t == "int16" then return memory.read_u32(address) % 65536 end
    if t == "uint64" or t == "int64" or t == "CUtlStringToken" then return memory.read_u64(address) end
    if t == "Vector" or t == "VectorWS" or t == "Vector3" then
        local v = memory.read_vec3(address)
        return vec3_t(v.x, v.y, v.z)
    end
    if t == "QAngle" then
        local v = memory.read_vec3(address)
        return angle_t(v.x, v.y, v.z)
    end
    if t == "Vector2D" then return vec2_t(memory.read_float(address), memory.read_float(address + 4)) end
    if t == "Vector4D" or t == "Quaternion" then
        return vec4_t(memory.read_float(address), memory.read_float(address + 4), memory.read_float(address + 8), memory.read_float(address + 12))
    end
    if t == "Color" then return color_t(memory.read_u8(address), memory.read_u8(address + 1), memory.read_u8(address + 2), memory.read_u8(address + 3)) end
    if t:match("^char%[") or t == "CUtlSymbolLarge" or t == "CUtlString" then
        if t:match("^char%[") then return memory.read_string(address, tonumber(t:match("%[(%d+)%]")) or 128) end
        local ptr = memory.read_ptr(address)
        return ptr ~= 0 and memory.read_string(ptr, 256) or ""
    end
    if t:match("%*$") then return wrap(memory.read_ptr(address)) end
    return wrap(address)
end

local function write_typed(address, typename, value)
    local t = typename or "?"
    if t == "bool" then return memory.write_bool(address, value) end
    if t == "float32" or t == "float" or t == "GameTime_t" then return memory.write_float(address, value) end
    if t == "int32" or t == "int" then return memory.write_i32(address, value) end
    if t == "uint32" or t:match("^CHandle") then return memory.write_u32(address, value) end
    if t == "uint8" or t == "int8" or t == "char" then return memory.write_u8(address, value) end
    if t == "uint64" or t == "int64" then return memory.write_u64(address, value) end
    if t == "Vector" or t == "VectorWS" then return memory.write_vec3(address, { x = value.x, y = value.y, z = value.z }) end
    if t == "QAngle" then return memory.write_vec3(address, { x = value.pitch, y = value.yaw, z = value.roll }) end
    if t == "Color" then
        memory.write_u8(address, value.r); memory.write_u8(address + 1, value.g)
        memory.write_u8(address + 2, value.b); memory.write_u8(address + 3, value.a)
        return
    end
    error("cannot write netvar of type " .. t)
end

local builtin_types = {
    m_iHealth = "int32", m_iTeamNum = "uint8", m_fFlags = "uint32", m_pGameSceneNode = "CGameSceneNode*", m_lifeState = "uint8",
    m_vecVelocity = "Vector", m_hOwnerEntity = "CHandle", m_fEffects = "uint32", m_pWeaponServices = "CPlayer_WeaponServices*",
    m_pCameraServices = "CPlayer_CameraServices*", m_pObserverServices = "CPlayer_ObserverServices*", m_pItemServices = "CPlayer_ItemServices*",
    m_hObserverTarget = "CHandle", m_iObserverMode = "uint8", m_bIsThirdPersonView = "bool", m_hPlayerPawn = "CHandle",
    m_bIsLocalPlayerController = "bool", m_iszPlayerName = "char[128]", m_vecAbsOrigin = "VectorWS", m_bDormant = "bool",
    m_angEyeAngles = "QAngle", m_aimPunchAngle = "QAngle", m_iShotsFired = "int32", m_bIsScoped = "bool", m_iIDEntIndex = "int32",
    m_flFlashDuration = "float32", m_flFlashMaxAlpha = "float32", m_hActiveWeapon = "CHandle", m_iClip1 = "int32",
    m_iItemDefinitionIndex = "uint16", m_ArmorValue = "int32", m_bIsDefusing = "bool", m_bHasHelmet = "bool", m_bHasDefuser = "bool",
    m_bInReload = "bool", m_fAccuracyPenalty = "float32", m_iFOV = "uint32", m_clrRender = "Color", m_nRenderMode = "uint8",
    m_bBombTicking = "bool", m_bBombDefused = "bool", m_bBeingDefused = "bool", m_nBombSite = "int32", m_flC4Blow = "GameTime_t",
    m_flTimerLength = "float32", m_flDefuseLength = "float32", m_flDefuseCountDown = "GameTime_t", m_modelState = "CModelState",
    m_entitySpottedState = "EntitySpottedState_t", m_bSpotted = "bool", m_bSpottedByMask = "uint32", m_Glow = "CGlowProperty",
    m_bGlowing = "bool", m_iGlowType = "int32", m_glowColorOverride = "Color", m_hThrower = "CHandle", m_bPinPulled = "bool",
    m_flThrowStrength = "float32", m_bDidSmokeEffect = "bool", m_vSmokeColor = "Vector", m_nSmokeEffectTickBegin = "int32",
    m_AttributeManager = "C_AttributeContainer", m_Item = "C_EconItemView", m_flMinExposure = "float32", m_flMaxExposure = "float32",
    m_bExposureControl = "bool",
}

local function netvar(key, class)
    local offset, typename = spx.netvar(key, class)
    if offset and (typename == nil or typename == "?") then typename = builtin_types[key] or "int32" end
    return offset, typename
end

base_entity_mt.__index = function(self, key)
    local method = entity_methods[key]
    if method then return method end
    local address = rawget(self, "__address")
    if type(key) == "number" then return address + key end
    if type(key) ~= "string" then return nil end
    local offset, typename = netvar(key)
    if not offset then return nil end
    return read_typed(address + offset, typename)
end

base_entity_mt.__newindex = function(self, key, value)
    local offset, typename = netvar(key)
    if not offset then error("unknown netvar '" .. tostring(key) .. "'") end
    write_typed(rawget(self, "__address") + offset, typename, value)
end

base_entity_mt.__eq = function(a, b) return rawget(a, "__address") == rawget(b, "__address") end
base_entity_mt.__tostring = function(self)
    return string.format("base_entity_t(0x%X, %s)", rawget(self, "__address"), self:get_class_name())
end

function entity_methods:get_address() return rawget(self, "__address") end
function entity_methods:get_class_name()
    local designer = spx.entity_info(rawget(self, "__address"))
    return class_name_for(designer or "")
end
function entity_methods:get_designer_name() return (spx.entity_info(rawget(self, "__address"))) or "" end
function entity_methods:get_entity_handle()
    local _, handle = spx.entity_info(rawget(self, "__address"))
    return handle or 0xFFFFFFFF
end
function entity_methods:get_abs_origin()
    local node = self.m_pGameSceneNode
    return node and node.m_vecAbsOrigin or vec3_t(0, 0, 0)
end
function entity_methods:get_abs_rotation()
    local node = self.m_pGameSceneNode
    return node and node.m_angAbsRotation or angle_t(0, 0, 0)
end

entitylist = {}
function entitylist.get_local_player_controller()
    local controller = spx.local_entities()
    return wrap(controller)
end
function entitylist.get_local_player_pawn()
    local _, pawn = spx.local_entities()
    return wrap(pawn)
end
function entitylist.get_entity_from_handle(handle)
    if not handle or handle == 0xFFFFFFFF then return nil end
    return wrap(spx.entity_by_index(handle % 32768))
end
function entitylist.get_entities(class_name, include_inherits, callback)
    local out = {}
    for _, slot in ipairs(spx.entities(1, 2048)) do
        local class = class_name_for(slot.designer)
        local match = not class_name or class == class_name or slot.designer == class_name or (include_inherits and inherits(class, class_name))
        if match then
            local entity = wrap(slot.address)
            if callback then callback(entity) else out[#out + 1] = entity end
        end
    end
    if not callback then return out end
end
function entitylist.get_game_rules()
    for _, slot in ipairs(spx.entities(1, 2048)) do
        if slot.designer:match("gamerules") then
            local proxy = wrap(slot.address)
            return proxy and proxy.m_pGameRules or nil
        end
    end
end

local hitbox_bones = { [0] = 7, 6, 1, 2, 3, 4, 5, 20, 17, 21, 18, 22, 19, 15, 11, 13, 14, 9, 10 }
local hitbox_names = { [0] = "head_0", "neck_0", "pelvis", "spine_0", "spine_1", "spine_2", "spine_3", "leg_upper_R", "leg_upper_L",
    "leg_lower_R", "leg_lower_L", "ankle_R", "ankle_L", "hand_R", "hand_L", "arm_upper_R", "arm_lower_R", "arm_upper_L", "arm_lower_L" }
local hitbox_radius = { [0] = 4.2, 3.6, 6.5, 6.5, 6.8, 6.8, 6.2, 5.0, 5.0, 4.0, 4.0, 3.0, 3.0, 2.6, 2.6, 3.0, 2.6, 3.0, 2.6 }

engine = engine or {}
function engine.get_level_name() return spx.level_name() end
function engine.camera_in_thirdperson() return spx.thirdperson() end
function engine.get_product_version_string() return spx.version() end
function engine.play_sound(name, volume) spx.sound(name, volume or 1) end
function engine.chat_print(text) spx.notice(tostring(text)) client.log(tostring(text)) end
function engine.execute_client_cmd(command)
    if not warned.execute_client_cmd then
        warned.execute_client_cmd = true
        client.log("engine.execute_client_cmd is not available in an external client")
    end
    return false
end
function engine.get_net_channel() return nil end
function engine.get_netvar_offset(module_name, table_name, prop_name)
    local offset = spx.netvar(prop_name, table_name)
    return offset
end
function engine.get_hitbox_pos(pawn, index)
    if not pawn then return nil end
    local bone = hitbox_bones[index]
    if not bone then return nil end
    local bones = spx.bones(pawn:get_address())
    local b = bones[bone]
    if not b then return nil end
    return vec3_t(b[1], b[2], b[3])
end
function engine.get_hitbox(pawn, index)
    local pos = engine.get_hitbox_pos(pawn, index)
    if not pos then return nil end
    local r = hitbox_radius[index] or 3
    return { name = hitbox_names[index], bone_name = hitbox_names[index], surface_property = "flesh", mins = pos - r, maxs = pos + r,
        shape_radius = r, bone_name_hash = 0, group_id = index, shape_type = 1, translation_only = false, crc = 0, hitbox_index = index }
end
function engine.trace_shape(ray, start, finish, filter)
    local grenades = filter and filter.interacts_with == "grenades"
    local blocked, fraction, hx, hy, hz, nx, ny, nz = spx.raycast(start.x, start.y, start.z, finish.x, finish.y, finish.z, grenades)
    local end_pos = vec3_t(hx, hy, hz)
    return { hit_entity = nil, hitbox = nil, contents = blocked and 1 or 0, shape_attributes = nil, start_pos = start, end_pos = end_pos,
        hit_normal = vec3_t(nx, ny, nz), hit_point = end_pos, hit_offset = 0, fraction = fraction, triangle = -1,
        hitbox_bone_index = -1, ray_type = 0, start_in_solid = false, exact_hit_point = true }
end
function engine.trace_bullet(from_pawn, from, to)
    if not from_pawn then return nil end
    return spx.damage(from_pawn:get_address(), from.x, from.y, from.z, to.x, to.y, to.z)
end

function ray_t(a, b, c)
    return { start_offset = a, radius = type(b) == "number" and b or c or 0, mins = a, maxs = type(b) == "table" and b or nil }
end
function trace_filter_t(interacts_with, collision_group, iterate_entities, callback)
    return { interacts_with = interacts_with, collision_group = collision_group, iterate_entities = iterate_entities, callback = callback }
end

local game_event_mt = { __name = "game_event_t" }
game_event_mt.__index = game_event_mt
function game_event_mt:get_name() return self._data._name end
function game_event_mt:get_int(key) return math.floor(tonumber(self._data[key]) or 0) end
function game_event_mt:get_float(key) return tonumber(self._data[key]) or 0 end
function game_event_mt:get_string(key) return tostring(self._data[key] or "") end
function game_event_mt:get_bool(key) local v = self._data[key] return v == true or (tonumber(v) or 0) ~= 0 end
function game_event_mt:get_controller(key)
    local index = tonumber(self._data[key])
    return index and wrap(spx.entity_by_index(index)) or nil
end
function game_event_mt:get_pawn(key)
    local controller = self:get_controller(key)
    local handle = controller and controller.m_hPlayerPawn
    return handle and entitylist.get_entity_from_handle(handle) or nil
end
function game_event_mt:set_int(key, value) self._data[key] = math.floor(value) end
function game_event_mt:set_float(key, value) self._data[key] = value end
function game_event_mt:set_string(key, value) self._data[key] = tostring(value) end

function _spx_dispatch_game_event(data)
    local event = setmetatable({ _data = data }, game_event_mt)
    for _, entry in ipairs(handlers.game_event or {}) do
        local ok, err = pcall(run_owned, entry.owner, entry.fn, event)
        if not ok then client.log("game_event error: " .. tostring(err)) end
    end
end

local convar_cache, convar_mtime = nil, 0
local function convar_values()
    if convar_cache and client.get_time() - convar_mtime < 3 then return convar_cache end
    convar_cache, convar_mtime = {}, client.get_time()
    local root = os.getenv("HOME") .. "/.local/share/Steam/userdata"
    local list = io.popen('ls -t "' .. root .. '"/*/730/local/cfg/cs2_user_convars_0_slot0.vcfg "' .. root .. '"/*/730/local/cfg/cs2_machine_convars.vcfg 2>/dev/null')
    if not list then return convar_cache end
    for path in list:lines() do
        local f = io.open(path, "r")
        if f then
            for line in f:lines() do
                local key, value = line:match('^%s*"([^"]+)"%s+"([^"]*)"')
                if key and convar_cache[key] == nil then convar_cache[key] = value end
            end
            f:close()
        end
    end
    list:close()
    return convar_cache
end

local convar_mt = { __name = "convar_t" }
convar_mt.__index = convar_mt
function convar_mt:get_name() return self._name end
function convar_mt:get_desc() return "" end
function convar_mt:get_string() return convar_values()[self._name] or "" end
function convar_mt:get_float() return tonumber(self:get_string()) or 0 end
function convar_mt:get_int() return math.floor(self:get_float()) end
function convar_mt:get_bool()
    local s = self:get_string()
    return s == "true" or (tonumber(s) or 0) ~= 0
end
cvars = setmetatable({}, { __index = function(_, name) return setmetatable({ _name = name }, convar_mt) end })

local fonts = {}
function render.setup_font(filename, size, flags)
    local family = spx.font_file(filename) or filename
    local font = { family = family, size = size or 14, bold = flags ~= nil and flags ~= 0 }
    fonts[#fonts + 1] = font
    return font
end

local default_font = { family = "Roboto", size = 14, bold = false }
local function font_of(font) return type(font) == "table" and font.family and font or default_font end

function render.calc_text_size(text, font, size)
    font = font_of(font)
    local w, h = spx.measure(tostring(text), font.family, size or font.size, font.bold)
    return vec2_t(w, h)
end

function render.text(text, font, pos, color, size)
    font = font_of(font)
    local r, g, b, a = rgba(color)
    spx.text(tostring(text), font.family, size or font.size, font.bold, pos.x, pos.y, r, g, b, a)
end

local function texture_object(id, w, h)
    if not id then return nil end
    return { id = id, size = vec2_t(w, h) }
end
function render.setup_texture(filename) return texture_object(spx.texture_file(filename)) end
function render.setup_texture_from_memory(buffer)
    if type(buffer) == "table" then
        local chars = {}
        for i = 1, #buffer do chars[i] = string.char(buffer[i]) end
        buffer = table.concat(chars)
    end
    return texture_object(spx.texture_memory(buffer))
end
function render.setup_texture_rgba(buffer, size) return texture_object(spx.texture_rgba(buffer, size.x, size.y)) end
function render.texture(texture, from, to, color, rounding)
    if not texture then return end
    local _, _, _, a = rgba(color)
    spx.texture_draw(texture.id, from.x, from.y, to.x, to.y, 255, 255, 255, a, rounding or 0)
end

function render.screen_size()
    local w, h = engine.get_screen_size()
    return vec2_t(w, h)
end
function render.frame_count() return (spx.frame_info()) end
function render.frame_time() local _, t = spx.frame_info() return t end
function render.world_to_screen(pos)
    local x, y = spx.world_to_screen(pos.x, pos.y, pos.z)
    if not x then return nil end
    return vec2_t(x, y)
end

function render.line(from, to, color, thickness)
    local r, g, b, a = rgba(color)
    draw.line(from.x, from.y, to.x, to.y, r, g, b, a, thickness or 1)
end
function render.rect(from, to, color, rounding, thickness)
    local r, g, b, a = rgba(color)
    draw.rect(math.min(from.x, to.x), math.min(from.y, to.y), math.abs(to.x - from.x), math.abs(to.y - from.y), r, g, b, a, thickness or 1, rounding or 0)
end
function render.rect_filled(from, to, color, rounding)
    local r, g, b, a = rgba(color)
    draw.rect_filled(math.min(from.x, to.x), math.min(from.y, to.y), math.abs(to.x - from.x), math.abs(to.y - from.y), r, g, b, a, rounding or 0)
end
function render.rect_filled_fade(from, to, c1, c2, c3, c4)
    spx.gradient_rect(from.x, from.y, to.x, to.y, { c1.r, c1.g, c1.b, c1.a, c2.r, c2.g, c2.b, c2.a, c3.r, c3.g, c3.b, c3.a, c4.r, c4.g, c4.b, c4.a })
end
function render.circle(pos, radius, segments, color, thickness)
    local r, g, b, a = rgba(color)
    draw.circle(pos.x, pos.y, radius, r, g, b, a, thickness or 1)
end
function render.circle_filled(pos, radius, segments, color)
    local r, g, b, a = rgba(color)
    draw.circle_filled(pos.x, pos.y, radius, r, g, b, a)
end
function render.circle_fade(pos, radius, color_in, color_out)
    spx.radial(pos.x, pos.y, radius, { color_in.r, color_in.g, color_in.b, color_in.a, color_out.r, color_out.g, color_out.b, color_out.a })
end
function render.arc(pos, radius, a_min, a_max, segments, color, thickness)
    local r, g, b, a = rgba(color)
    spx.arc(pos.x, pos.y, radius, a_min, a_max, r, g, b, a, thickness or 1)
end

local function flatten(points)
    local flat = {}
    for _, p in ipairs(points) do flat[#flat + 1] = p.x; flat[#flat + 1] = p.y end
    return flat
end
function render.polygon(points, color)
    local r, g, b, a = rgba(color)
    spx.poly(flatten(points), r, g, b, a, "fill", 1, true)
end
render.concave_polygon = render.polygon
function render.poly_line(points, color, thickness)
    local r, g, b, a = rgba(color)
    spx.poly(flatten(points), r, g, b, a, "stroke", thickness or 1, false)
end

function render.push_clip_rect(from, to, intersect) spx.clip_push(from.x, from.y, to.x, to.y, intersect ~= false) end
function render.pop_clip_rect() spx.clip_pop() end

local function circle_points_3d(pos, radius, normal)
    normal = (normal or vec3_t(0, 0, 1)):normalized()
    local helper = math.abs(normal.z) < 0.9 and vec3_t(0, 0, 1) or vec3_t(1, 0, 0)
    local u = normal:cross(helper):normalized()
    local v = normal:cross(u)
    local points = {}
    for i = 0, 47 do
        local t = i / 48 * math.pi * 2
        local screen = render.world_to_screen(pos + u * (math.cos(t) * radius) + v * (math.sin(t) * radius))
        if not screen then return nil end
        points[#points + 1] = screen
    end
    return points
end
function render.circle_3d(pos, radius, color, thickness, normal)
    local points = circle_points_3d(pos, radius, normal)
    if not points then return end
    points[#points + 1] = points[1]
    render.poly_line(points, color, thickness)
end
function render.circle_filled_3d(pos, radius, color, normal)
    local points = circle_points_3d(pos, radius, normal)
    if points then render.polygon(points, color) end
end
function render.circle_fade_3d(pos, radius, color_in, color_out, normal)
    local points = circle_points_3d(pos, radius, normal)
    local center = render.world_to_screen(pos)
    if not points or not center then return end
    for i = 1, #points do
        local next_point = points[i % #points + 1]
        spx.poly({ center.x, center.y, points[i].x, points[i].y, next_point.x, next_point.y }, color_out.r, color_out.g, color_out.b, color_out.a, "fill", 1, true)
    end
    render.circle_filled(center, 2, 12, color_in)
end
)lua";
