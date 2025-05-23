local ffi = require('ffi')
local bit = require('bit')
local user32 = ffi.load('user32')

ffi.cdef[[
typedef struct { long x; long y; } POINT;
bool GetCursorPos(POINT* lpPoint);
bool SetCursorPos(int X, int Y);
short GetAsyncKeyState(int vKey);
int GetSystemMetrics(int nIndex);
]]

-- Helper to parse simple ini format
local function parseIni(path)
    local data = {}
    local section
    for line in io.lines(path) do
        line = line:gsub('[\r\n]+','')
        if line:match('^%s*;') or line:match('^%s*$') then
        elseif line:match('^%[') then
            section = line:match('%[(.-)%]')
        else
            local key, value = line:match('([^=]+)=([^=]+)')
            if key and value and section == 'Controls' then
                data[key:match('^%s*(.-)%s*$')] = value:match('^%s*(.-)%s*$')
            end
        end
    end
    return data
end

-- Key conversion helper
local vkeys = {
    BACKSPACE = 0x08, TAB = 0x09, ENTER = 0x0D, SHIFT = 0x10, CTRL = 0x11,
    ALT = 0x12, PAUSE = 0x13, CAPSLOCK = 0x14, ESC = 0x1B, SPACE = 0x20,
    PAGEUP = 0x21, PAGEDOWN = 0x22, END = 0x23, HOME = 0x24,
    LEFT = 0x25, UP = 0x26, RIGHT = 0x27, DOWN = 0x28,
    INSERT = 0x2D, DELETE = 0x2E,
    LSHIFT = 0xA0, RSHIFT = 0xA1, LCTRL = 0xA2, RCTRL = 0xA3,
    LALT = 0xA4, RALT = 0xA5
}
for i=0,9 do vkeys[tostring(i)] = 0x30 + i end
for i=0,25 do vkeys[string.char(65+i)] = 0x41 + i end
for i=1,12 do vkeys['F'..i] = 0x6F + i end

local function keyCode(name)
    if not name then return 0 end
    name = name:upper()
    if tonumber(name) then return tonumber(name) end
    if vkeys[name] then return vkeys[name] end
    if name:sub(1,2) == '0X' then return tonumber(name) end
    return 0
end

local iniPath = debug.getinfo(1, 'S').source:match('@(.+)[/\\]') .. '/free_cam.ini'
local controls = parseIni(iniPath)

local toggleName = controls.FreeCamToggle or 'F1'
local cfg = {
    toggle = keyCode(toggleName),
    forward = keyCode(controls.CamForward or 'W'),
    backward = keyCode(controls.CamBackward or 'S'),
    left = keyCode(controls.CamLeft or 'A'),
    right = keyCode(controls.CamRight or 'D'),
    up = keyCode(controls.CamUp or 'SPACE'),
    down = keyCode(controls.CamDown or 'SHIFT'),
    rollLeft = keyCode(controls.CamRollLeft or 'Q'),
    rollRight = keyCode(controls.CamRollRight or 'E'),
    speedUp = keyCode(controls.SpeedUp or 'LSHIFT'),
    slowDown = keyCode(controls.SlowDown or 'LCTRL'),
    fovUp = keyCode(controls.FovUp or 'PAGEUP'),
    fovDown = keyCode(controls.FovDown or 'PAGEDOWN'),
    moveSpeed = tonumber(controls.MovementSpeed) or 0.05,
    mouseSens = tonumber(controls.MouseSensitivity) or 0.005
}

-- Memory helpers
local function readFloat(addr)
    return ffi.cast('float*', addr)[0]
end
local function writeFloat(addr, val)
    ffi.cast('float*', addr)[0] = val
end

local base = Memory.GetModuleBase('F1_2012.exe')
local CamStructure

local function findCamStructure()
    local ptr = Memory.ReadMemory(base + 0xE374CC, 4)
    if ptr and ptr ~= 0 then
        CamStructure = ptr
        return true
    end
    CamStructure = nil
    return false
end

local renderOffsets = {0x26C0B7,0x26C0C5,0x26C0D3,0x26C0E1,0x26C0F0,0x26C100,0x26C110,0x26C120,0x26C130,0x26C140,0x26C150,0x26C15D}
local camOffsets    = {0x26C16A,0x26C178,0x26C186,0x26C194,0x26C1A3,0x26C1B3,0x26C1C3,0x26C1D3,0x26C1E3,0x26C1F3,0x26C203,0x26C211}
local patchSize     = {7,7,7,7,8,8,8,8,8,8,8,6}
local savedBytes = {}
local active = false

local function patch(addrs)
    for i,off in ipairs(addrs) do
        local addr = base + off
        local size = patchSize[i]
        local bytes = {}
        for j=0,size-1 do
            bytes[j+1] = Memory.ReadMemory(addr+j,1)
            Memory.WriteMemory(addr+j,0x90,1)
        end
        savedBytes[addr] = {size=size,data=bytes}
    end
end

local function restore()
    for addr,info in pairs(savedBytes) do
        for i=0,info.size-1 do
            Memory.WriteMemory(addr+i, info.data[i+1],1)
        end
    end
    savedBytes = {}
end

-- Orientation and position
local orient = {
    right = {1,0,0},
    up = {0,1,0},
    forward = {0,0,1}
}
local pos = {0,0,0}
local fov = 0
local yaw, pitch, roll = 0, 0, 0

local function vecDot(a,b)
    return a[1]*b[1]+a[2]*b[2]+a[3]*b[3]
end
local function vecCross(a,b)
    return {a[2]*b[3]-a[3]*b[2], a[3]*b[1]-a[1]*b[3], a[1]*b[2]-a[2]*b[1]}
end
local function vecNorm(a)
    local l = math.sqrt(vecDot(a,a))
    if l==0 then return {0,0,0} end
    return {a[1]/l,a[2]/l,a[3]/l}
end
local function rotateVec(v,axis,angle)
    local c = math.cos(angle)
    local s = math.sin(angle)
    local d = vecDot(axis,v)
    local cross = vecCross(axis,v)
    return {
        v[1]*c + cross[1]*s + axis[1]*d*(1-c),
        v[2]*c + cross[2]*s + axis[2]*d*(1-c),
        v[3]*c + cross[3]*s + axis[3]*d*(1-c)
    }
end
local function updateOrientation()
    local sy, cy = math.sin(yaw), math.cos(yaw)
    local sp, cp = math.sin(pitch), math.cos(pitch)

    orient.forward = { sy*cp, -sp, cy*cp }

    local world_up = {0,1,0}
    orient.right = vecNorm(vecCross(world_up, orient.forward))
    orient.up = vecNorm(vecCross(orient.forward, orient.right))

    if roll ~= 0 then
        orient.right = vecNorm(rotateVec(orient.right, orient.forward, roll))
        orient.up = vecNorm(rotateVec(orient.up, orient.forward, roll))
    end
end

local function readOrientation()
    orient.up     = { readFloat(CamStructure+0x630), readFloat(CamStructure+0x634), readFloat(CamStructure+0x638) }
    orient.right  = { readFloat(CamStructure+0x640), readFloat(CamStructure+0x644), readFloat(CamStructure+0x648) }
    orient.forward= { readFloat(CamStructure+0x650), readFloat(CamStructure+0x654), readFloat(CamStructure+0x658) }
end

local function readPosition()
    pos = { readFloat(CamStructure+0x660), readFloat(CamStructure+0x664), readFloat(CamStructure+0x668) }
end
local function writeOrientation()
    local vals = {
        orient.up[1], orient.up[2], orient.up[3],
        orient.right[1], orient.right[2], orient.right[3],
        orient.forward[1], orient.forward[2], orient.forward[3]
    }
    local o1 = {0x630,0x634,0x638,0x640,0x644,0x648,0x650,0x654,0x658}
    local o2 = {0x6A0,0x6A4,0x6A8,0x6B0,0x6B4,0x6B8,0x6C0,0x6C4,0x6C8}
    for i=1,9 do
        writeFloat(CamStructure+o1[i], vals[i])
        writeFloat(CamStructure+o2[i], vals[i])
    end
end
local function writePosition()
    local offs1 = {0x660,0x664,0x668}
    local offs2 = {0x6D0,0x6D4,0x6D8}
    for i=1,3 do
        writeFloat(CamStructure+offs1[i], pos[i])
        writeFloat(CamStructure+offs2[i], pos[i])
    end
end
local function writeFov()
    writeFloat(CamStructure+0x670, fov)
    writeFloat(CamStructure+0x6E0, fov)
end

local screenCenter = {x=0, y=0}
local VK_LBUTTON = 0x01
local VK_RBUTTON = 0x02
local VK_MBUTTON = 0x04
local lmbHeld = false

local function isKeyDown(vk)
    return bit.band(user32.GetAsyncKeyState(vk), 0x8000) ~= 0
end

-- Forward declaration for toEuler
local toEuler
local function mouseDelta()
    local pt = ffi.new('POINT[1]')
    if user32.GetCursorPos(pt) then
        local dx = pt[0].x - screenCenter.x
        local dy = pt[0].y - screenCenter.y
        user32.SetCursorPos(screenCenter.x, screenCenter.y)
        return dx, dy
    end
    return 0,0
end

local function enable()
    if not findCamStructure() then
        return false
    end
    patch(renderOffsets)
    patch(camOffsets)
    readOrientation()
    readPosition()
    fov = readFloat(CamStructure+0x670)
    local p,y,r = toEuler()
    pitch = math.rad(p)
    yaw = math.rad(y)
    roll = math.rad(r)
    screenCenter.x = user32.GetSystemMetrics(0) / 2
    screenCenter.y = user32.GetSystemMetrics(1) / 2
    user32.SetCursorPos(screenCenter.x, screenCenter.y)
    active = true
    return true
end

local function disable()
    restore()
    active = false
end

toEuler = function()
    local pitch = math.deg(math.asin(-orient.forward[2]))
    local yaw = math.deg(math.atan2(orient.forward[1], orient.forward[3]))
    local roll = math.deg(math.atan2(orient.up[1], orient.up[2]))
    return pitch,yaw,roll
end

local function status(state)
    local pitch,yaw,roll = toEuler()
    return string.format(
        "FreeCam %s. Press %s to toggle.\nCurrent coordinates: x = %.2f, y = %.2f, z = %.2f\nCurrent oriantation: pitch = %.2f, yaw = %.2f, roll = %.2f\nCurrent fov: %.2f",
        state, toggleName, pos[1], pos[2], pos[3], pitch, yaw, roll, fov)
end

function OnFrame()
    if not CamStructure and not findCamStructure() then
        if active then
            disable()
        end
        SCRIPT_RESULT = 'Waiting for camera...'
        return true
    end

    if Keyboard.IsKeyPressed(cfg.toggle) then
        if active then
            disable()
            SCRIPT_RESULT = status('DISABLED')
        else
            if enable() then
                SCRIPT_RESULT = status('ENABLED')
            else
                SCRIPT_RESULT = 'Camera not found'
            end
        end
        return true
    end

    if not active then
        readOrientation()
        readPosition()
        fov = readFloat(CamStructure+0x670)
        local p,y,r = toEuler()
        pitch = math.rad(p)
        yaw = math.rad(y)
        roll = math.rad(r)
        SCRIPT_RESULT = status('DISABLED')
        return true
    end

    local speedMod = 1
    if Keyboard.IsKeyDown(cfg.speedUp) then speedMod = speedMod * 10 end
    if Keyboard.IsKeyDown(cfg.slowDown) then speedMod = speedMod / 10 end
    local speed = cfg.moveSpeed * speedMod

    if Keyboard.IsKeyDown(cfg.forward) then
        pos[1] = pos[1] + orient.forward[1]*speed
        pos[2] = pos[2] + orient.forward[2]*speed
        pos[3] = pos[3] + orient.forward[3]*speed
    end

    if Keyboard.IsKeyDown(cfg.backward) then
        pos[1] = pos[1] - orient.forward[1]*speed
        pos[2] = pos[2] - orient.forward[2]*speed
        pos[3] = pos[3] - orient.forward[3]*speed
    end

    if Keyboard.IsKeyDown(cfg.left) then
        pos[1] = pos[1] + orient.right[1]*speed
        pos[2] = pos[2] + orient.right[2]*speed
        pos[3] = pos[3] + orient.right[3]*speed
    end

    if Keyboard.IsKeyDown(cfg.right) then
        pos[1] = pos[1] - orient.right[1]*speed
        pos[2] = pos[2] - orient.right[2]*speed
        pos[3] = pos[3] - orient.right[3]*speed
    end

    if Keyboard.IsKeyDown(cfg.up) then
        pos[1] = pos[1] + orient.up[1]*speed
        pos[2] = pos[2] + orient.up[2]*speed
        pos[3] = pos[3] + orient.up[3]*speed
    end

    if Keyboard.IsKeyDown(cfg.down) then
        pos[1] = pos[1] - orient.up[1]*speed
        pos[2] = pos[2] - orient.up[2]*speed
        pos[3] = pos[3] - orient.up[3]*speed
    end

    local fovSpeed = speed * 0.1
    if Keyboard.IsKeyDown(cfg.fovUp) then
        fov = fov + fovSpeed
    elseif Keyboard.IsKeyDown(cfg.fovDown) then
        fov = fov - fovSpeed
    end

    local rollSpeed = 0.01 * speedMod
    if Keyboard.IsKeyDown(cfg.rollLeft) then
        roll = roll - rollSpeed
    elseif Keyboard.IsKeyDown(cfg.rollRight) then
        roll = roll + rollSpeed
    end

    local dx, dy = 0, 0
    local lmbDown = isKeyDown(VK_LBUTTON)
    if lmbDown then
        if not lmbHeld then
            user32.SetCursorPos(screenCenter.x, screenCenter.y)
            dx, dy = 0, 0
        else
            dx, dy = mouseDelta()
        end
    end
    lmbHeld = lmbDown
    local lookSpeed = cfg.mouseSens * 0.5
    if isKeyDown(VK_RBUTTON) then
        lookSpeed = lookSpeed * 2
    end
    if isKeyDown(VK_MBUTTON) then
        lookSpeed = lookSpeed / 10
    end
    if dx ~= 0 then yaw = yaw - dx * lookSpeed end
    if dy ~= 0 then pitch = pitch + dy * lookSpeed end

    if pitch > math.pi/2 - 0.001 then pitch = math.pi/2 - 0.001 end
    if pitch < -math.pi/2 + 0.001 then pitch = -math.pi/2 + 0.001 end

    updateOrientation()

    writeOrientation()
    writePosition()
    writeFov()
    SCRIPT_RESULT = status('ENABLED')
    return true
end

if findCamStructure() then
    readOrientation()
    readPosition()
    fov = readFloat(CamStructure+0x670)
    local p,y,r = toEuler()
    pitch = math.rad(p)
    yaw = math.rad(y)
    roll = math.rad(r)
    SCRIPT_RESULT = status('loaded')
else
    SCRIPT_RESULT = 'Waiting for camera...'
end