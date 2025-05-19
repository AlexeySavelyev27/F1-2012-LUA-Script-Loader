local ffi = require('ffi')
local user32 = ffi.load('user32')

ffi.cdef[[
typedef struct { long x; long y; } POINT;
bool GetCursorPos(POINT* lpPoint);
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
local function keyCode(name)
    if tonumber(name) then return tonumber(name) end
    if Keys and Keys[name] then return Keys[name] end
    if name:sub(1,2) == '0x' or name:sub(1,2) == '0X' then
        return tonumber(name)
    end
    if #name == 1 then
        return string.byte(name:upper())
    end
    return 0
end

local iniPath = debug.getinfo(1, 'S').source:match('@(.+)[/\\]') .. '/free_cam.ini'
local controls = parseIni(iniPath)

local cfg = {
    toggle = keyCode(controls.FreeCamToggle or 'VK_F1'),
    forward = keyCode(controls.CamForward or 'VK_W'),
    backward = keyCode(controls.CamBackward or 'VK_S'),
    left = keyCode(controls.CamLeft or 'VK_A'),
    right = keyCode(controls.CamRight or 'VK_D'),
    up = keyCode(controls.CamUp or 'VK_SPACE'),
    down = keyCode(controls.CamDown or 'VK_SHIFT'),
    rollLeft = keyCode(controls.CamRollLeft or 'VK_Q'),
    rollRight = keyCode(controls.CamRollRight or 'VK_E'),
    speedUp = keyCode(controls.SpeedUp or 'VK_LSHIFT'),
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
local function rotateAround(axis,angle)
    orient.right = vecNorm(rotateVec(orient.right,axis,angle))
    orient.up = vecNorm(rotateVec(orient.up,axis,angle))
    orient.forward = vecNorm(rotateVec(orient.forward,axis,angle))
end

local function readOrientation()
    orient.up    = { readFloat(CamStructure+0x630), readFloat(CamStructure+0x640), readFloat(CamStructure+0x650) }
    orient.right = { readFloat(CamStructure+0x634), readFloat(CamStructure+0x644), readFloat(CamStructure+0x654) }
    orient.forward= { readFloat(CamStructure+0x638), readFloat(CamStructure+0x648), readFloat(CamStructure+0x658) }
end
local function readPosition()
    pos = { readFloat(CamStructure+0x660), readFloat(CamStructure+0x664), readFloat(CamStructure+0x668) }
end
local function writeOrientation()
    local vals = {
        orient.up[1], orient.right[1], orient.forward[1],
        orient.up[2], orient.right[2], orient.forward[2],
        orient.up[3], orient.right[3], orient.forward[3]
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

local prevMouse = {x=0,y=0}
local function mouseDelta()
    local pt = ffi.new('POINT[1]')
    if user32.GetCursorPos(pt) then
        local dx = pt[0].x - prevMouse.x
        local dy = pt[0].y - prevMouse.y
        prevMouse.x = pt[0].x
        prevMouse.y = pt[0].y
        return dx, dy
    end
    return 0,0
end

local function enable()
    patch(renderOffsets)
    patch(camOffsets)
    CamStructure = ffi.cast('uint32_t*', base + 0xE374CC)[0]
    readOrientation()
    readPosition()
    fov = readFloat(CamStructure+0x670)
    local pt = ffi.new('POINT[1]')
    user32.GetCursorPos(pt)
    prevMouse.x = pt[0].x
    prevMouse.y = pt[0].y
    active = true
end

local function disable()
    restore()
    active = false
end

function OnFrame()
    if Keyboard.IsKeyPressed(cfg.toggle) then
        if active then disable() else enable() end
    end
    if not active then return end

    local speed = cfg.moveSpeed
    if Keyboard.IsKeyDown(cfg.speedUp) then speed = speed * 10 end

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

    local rollSpeed = 0.01
    if Keyboard.IsKeyDown(cfg.rollLeft) then
        rotateAround(orient.forward, -rollSpeed)
    elseif Keyboard.IsKeyDown(cfg.rollRight) then
        rotateAround(orient.forward, rollSpeed)
    end

    local dx, dy = mouseDelta()
    if dx ~= 0 then rotateAround(orient.up, -dx * cfg.mouseSens) end
    if dy ~= 0 then rotateAround(orient.right, -dy * cfg.mouseSens) end

    writeOrientation()
    writePosition()
    writeFov()
end

SCRIPT_RESULT = 'FreeCam loaded'

