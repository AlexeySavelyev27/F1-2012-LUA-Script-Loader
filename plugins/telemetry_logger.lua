local ffi = require('ffi')

local function writeLog(message, clear)
    local mode = clear and 'w' or 'a'
    local logFile = io.open('telemetry_debug.log', mode)
    if logFile then
        logFile:write(os.date('%Y-%m-%d %H:%M:%S') .. ': ' .. message .. '\n')
        logFile:close()
    end
end

writeLog('Starting new session', true)

-- simple ini parser for controls section
local function parseIni(path)
    local data = {}
    local section
    for line in io.lines(path) do
        line = line:gsub('[\r\n]+', '')
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

-- virtual key conversion helper
local vkeys = {
    BACKSPACE = 0x08, TAB = 0x09, ENTER = 0x0D, SHIFT = 0x10, CTRL = 0x11,
    ALT = 0x12, PAUSE = 0x13, CAPSLOCK = 0x14, ESC = 0x1B, SPACE = 0x20,
    PAGEUP = 0x21, PAGEDOWN = 0x22, END = 0x23, HOME = 0x24,
    LEFT = 0x25, UP = 0x26, RIGHT = 0x27, DOWN = 0x28,
    INSERT = 0x2D, DELETE = 0x2E,
    LSHIFT = 0xA0, RSHIFT = 0xA1, LCTRL = 0xA2, RCTRL = 0xA3,
    LALT = 0xA4, RALT = 0xA5
}
for i = 0, 9 do vkeys[tostring(i)] = 0x30 + i end
for i = 0, 25 do vkeys[string.char(65 + i)] = 0x41 + i end
for i = 1, 12 do vkeys['F'..i] = 0x6F + i end

local function keyCode(name)
    if not name then return 0 end
    name = name:upper()
    if tonumber(name) then return tonumber(name) end
    if vkeys[name] then return vkeys[name] end
    if name:sub(1, 2) == '0X' then return tonumber(name) end
    return 0
end

local iniPath = debug.getinfo(1, 'S').source:match('@(.+)[/\\]') .. '/telemetry_logger.ini'
local controls = parseIni(iniPath)
local startName = controls.StartLogging or 'F9'
local startKey = keyCode(startName)

local status = string.format('Press %s to start logging', startName)

local function readFloat(addr)
    return ffi.cast('float*', addr)[0]
end

local base = Memory.GetModuleBase('F1_2012.exe')

local function readPtr(addr)
    return Memory.ReadMemory(addr, 4)
end

local function resolve(startAddr, offsets)
    local addr = readPtr(startAddr)
    if not addr or addr == 0 then
        return nil
    end
    for i = 1, #offsets do
        addr = readPtr(addr + offsets[i])
        if not addr or addr == 0 then
            return nil
        end
    end
    return addr
end

local function carBase()
    return resolve(base + 0xDD5ABC, {0x50, 0x4A8, 0x38})
end

local function lapAddr()
    local ptr = readPtr(base + 0xDD5A94)
    if not ptr then return nil end
    ptr = readPtr(ptr + 0x78)
    if not ptr then return nil end
    return ptr + 0xA18
end

local function lapNumber()
    local addr = lapAddr()
    if not addr then return nil end
    return Memory.ReadMemory(addr, 4), addr
end

local function speedAddr()
    local ptr = readPtr(base + 0xD976F4)
    if not ptr then return nil end
    return ptr + 0x4DC
end

local function gearBase()
    return resolve(base + 0xDD0D28, {0x24, 0x20, 0x70, 0x10, 0x38})
end

local function throttleAddr()
    return resolve(base + 0xD9A6D8, {0x64, 0x78, 0x8, 0x4, 0x4C})
end

local function brakeAddr()
    return resolve(base + 0xD96F94, {0xF0, 0x8, 0x3C, 0x60, 0x4C, 0x4})
end

local frame = 0
local curLap
local index = 0
local file
local points = {}
local function openLap(lap)
    if file then file:close() end
    local path = string.format('telemetry_lap_%d.csv', lap)
    file = io.open(path, 'w')
    if not file then

        status = string.format('Failed to open %s', path)
        SCRIPT_RESULT = status

        return false
    end
    file:write('index,x,y,z,speed,gear,throttle,brake,drs,kers\n')
    points = {}
    index = 0

    status = string.format('Lap %d: %d samples', lap, index)
    SCRIPT_RESULT = status

    return true
end

local function writeObj(lap)
    if #points == 0 then return end
    local path = string.format('lap_%d.obj', lap)
    local obj = io.open(path, 'w')
    if not obj then return end
    for _,p in ipairs(points) do
        obj:write(string.format('v %.6f %.6f %.6f\n', p[1], p[2], p[3]))
    end
    obj:close()
end

function OnFrame()
    frame = frame + 1
    if not active then
        if Keyboard.IsKeyPressed(startKey) then
            active = true
            SCRIPT_RESULT = 'Logging started'
        else
            SCRIPT_RESULT = status
        end
        return true
    end

    if frame % 5 ~= 0 then
        SCRIPT_RESULT = status
        return true
    end

    local cBase = carBase()
    if not cBase then
        status = 'Waiting for car...'
        SCRIPT_RESULT = status
        return true
    end

    local lap, lapPtr = lapNumber()
    lap = lap or 0
    if curLap ~= lap then
        if curLap then
            writeObj(curLap)
        end
        if lap == 0 then
            if file then file:close() end
            file = nil
            active = false
            SCRIPT_RESULT = 'Logging finished'
            return true
        end
        curLap = lap

        if not openLap(lap) then
            return true
        end
    end
    if not file then
        SCRIPT_RESULT = status
        return true

    end

    local x = readFloat(cBase + 0x1A0)
    local y = readFloat(cBase + 0x1A4)
    local z = readFloat(cBase + 0x1A8)

    local sAddr = speedAddr()
    local speed = sAddr and readFloat(sAddr) or 0

    local gBase = gearBase()
    local gear = gBase and Memory.ReadMemory(gBase + 0x244, 4) or 0

    local thrPtr = throttleAddr()
    local throttle = thrPtr and readFloat(thrPtr + 0x8) or 0

    local brkPtr = brakeAddr()
    local brake = brkPtr and readFloat(brkPtr + 0x8) or 0

    local drs = gBase and Memory.ReadMemory(gBase + 0x29C, 4) or 0
    local kers = gBase and Memory.ReadMemory(gBase + 0x294, 4) or 0

    writeLog(string.format(
        'cBase=0x%X lapPtr=0x%X lap=%d sAddr=0x%X speed=%.3f gBase=0x%X gear=%d '
        .. 'thrPtr=0x%X throttle=%.2f brkPtr=0x%X brake=%.2f drs=%d kers=%d',
        cBase or 0, lapPtr or 0, lap, sAddr or 0, speed, gBase or 0, gear,
        thrPtr or 0, throttle, brkPtr or 0, brake, drs, kers))

    index = index + 1
    file:write(string.format('%d,%.6f,%.6f,%.6f,%.3f,%d,%.2f,%.2f,%d,%d\n',
        index, x, y, z, speed, gear, throttle, brake, drs, kers))
    table.insert(points, {x, y, z})

    status = string.format('Lap %d: %d samples', curLap, index)
    SCRIPT_RESULT = status
    return true
end

function OnPluginEnd()
    if file then
        file:close()
        file = nil
    end
    if curLap then
        writeObj(curLap)
    end
    status = 'Telemetry log saved'
    SCRIPT_RESULT = status
end
