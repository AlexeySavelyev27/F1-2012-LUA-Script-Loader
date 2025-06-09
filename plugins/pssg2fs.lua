local ffi = require('ffi')
local kernel32 = ffi.load('kernel32')

local function writeLog(message, clear)
    local mode = clear and 'w' or 'a'
    local logFile = io.open('pssg2fs.log', mode)
    if logFile then
        logFile:write(os.date('%Y-%m-%d %H:%M:%S') .. ': ' .. message .. '\n')
        logFile:close()
    end
end

writeLog('Starting new session', true)

ffi.cdef[[
unsigned int GetModuleFileNameA(void* hModule, char* lpFilename, unsigned int size);
]]

local pluginDir = debug.getinfo(1, 'S').source:match('@(.+)[/\\]')
local basePath = pluginDir .. '/pssg2fs/'
writeLog('Plugin directory: ' .. pluginDir)
writeLog('Base path: ' .. basePath)

local exeBase = Memory.GetModuleBase('F1_2012.exe')
local buf = ffi.new('char[260]')
local gameDir = ''
if exeBase then
    kernel32.GetModuleFileNameA(ffi.cast('void*', exeBase), buf, 260)
    local exePath = ffi.string(buf)
    gameDir = exePath:match('^(.*)[/\\][^/\\]+$') or ''
    writeLog('Game directory: ' .. gameDir)
    writeLog(string.format('Base at 0x%X', exeBase))
end

local function escapePattern(text)
    return text:gsub('([%^%$%(%)%%%.%[%]%*%+%-%?])', '%%%1')
end

local breakpointSet = false

local function readString(addr)
    local bytes = {}
    local offset = 0
    local b = Memory.ReadMemory(addr, 1)
    local nextByte = Memory.ReadMemory(addr + 1, 1)
    local wide = nextByte == 0
    while true do
        if wide then
            local c1 = Memory.ReadMemory(addr + offset, 1)
            local c2 = Memory.ReadMemory(addr + offset + 1, 1)
            if not c1 or not c2 or (c1 == 0 and c2 == 0) then break end
            bytes[#bytes + 1] = string.char(c1)
            offset = offset + 2
        else
            local c = Memory.ReadMemory(addr + offset, 1)
            if not c or c == 0 then break end
            bytes[#bytes + 1] = string.char(c)
            offset = offset + 1
        end
    end
    return table.concat(bytes)
end

function OnBreakpoint(address)
    local regs = Registers.Get()
    local ptr = Memory.ReadMemory(regs.esp + 4, 4)
    if not ptr or ptr == 0 then return end
    local original = readString(ptr)
    writeLog('Intercepted path: ' .. original)
    local relative = original
    if gameDir ~= '' then
        local pattern = '^' .. escapePattern(gameDir) .. '[\\/]*'
        relative = relative:gsub(pattern, '')
    end
    writeLog('Relative path: ' .. relative)
    local newPath = basePath .. relative
    writeLog('Looking for: ' .. newPath)
    local file = io.open(newPath, 'rb')
    if file then
        file:close()
        writeLog('Redirecting to: ' .. newPath)
        local mem = Memory.AllocateMemory(#newPath + 1)
        for i = 1, #newPath do
            Memory.WriteMemory(mem + i - 1, newPath:byte(i), 1)
        end
        Memory.WriteMemory(mem + #newPath, 0, 1)
        Memory.WriteMemory(regs.esp + 4, mem, 4)
    else
        writeLog('File not found, using original')
    end
end

function OnFrame()
    if not breakpointSet then
        local base = Memory.GetModuleBase('F1_2012.exe')
        if base and Debug.SetBreakpoint(base + 0x4C2706, 'OnBreakpoint') then
            SCRIPT_RESULT = 'pssg2fs active'
            writeLog(string.format('Breakpoint set at 0x%X', base + 0x4C2706))
            breakpointSet = true
        else
            SCRIPT_RESULT = 'pssg2fs failed'
            writeLog(string.format('Failed to set breakpoint at 0x%X', base + 0x4C2706))
        end
        return true
    end
    return false
end
