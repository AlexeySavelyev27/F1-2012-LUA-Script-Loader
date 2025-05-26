local ffi = require('ffi')

local function readFloat(addr)
    return ffi.cast('float*', addr)[0]
end

local base = Memory.GetModuleBase('F1_2012.exe')

local function readPtr(addr)
    return Memory.ReadMemory(addr, 4)
end

local function resolve(startAddr, offsets)
    local addr = startAddr
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

local function lapNumber()
    local ptr = readPtr(base + 0xDD5A94)
    if not ptr then return nil end
    ptr = readPtr(ptr + 0x78)
    if not ptr then return nil end
    return Memory.ReadMemory(ptr + 0xA18, 4)
end

local function speedAddr()
    local ptr = readPtr(base + 0xD976F4)
    if not ptr then return nil end
    return ptr + 0x4DC
end

local function gearBase()
    return resolve(base + 0xDD0D28, {0x24, 0x20, 0x70, 0x10, 0x38})
end

local frame = 0
local curLap
local index = 0
local file
local points = {}
local status = 'Waiting...'

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

    local lap = lapNumber() or 0
    if curLap ~= lap then
        if curLap then writeObj(curLap) end
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

    local thrPtr = resolve(base + 0xD9A6D8, {0x64,0x78,0x8,0x4,0x4C})
    local throttle = thrPtr and readFloat(thrPtr + 0x8) or 0

    local brkPtr = resolve(base + 0xD96F94, {0xF0,0x8,0x3C,0x60,0x4C,0x4})
    local brake = brkPtr and readFloat(brkPtr + 0x8) or 0

    local drs = gBase and Memory.ReadMemory(gBase + 0x29C, 4) or 0
    local kers = gBase and Memory.ReadMemory(gBase + 0x294, 4) or 0

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
