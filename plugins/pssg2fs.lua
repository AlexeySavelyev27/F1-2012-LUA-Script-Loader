local ffi = require('ffi')
local bit = require('bit')
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
typedef struct {
    unsigned int dwFileAttributes;
    unsigned int ftCreationTimeLow;
    unsigned int ftCreationTimeHigh;
    unsigned int ftLastAccessTimeLow;
    unsigned int ftLastAccessTimeHigh;
    unsigned int ftLastWriteTimeLow;
    unsigned int ftLastWriteTimeHigh;
    unsigned int nFileSizeHigh;
    unsigned int nFileSizeLow;
    unsigned int dwReserved0;
    unsigned int dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA;
void* FindFirstFileA(const char* lpFileName, WIN32_FIND_DATAA* lpFindFileData);
int FindNextFileA(void* hFindFile, WIN32_FIND_DATAA* lpFindFileData);
int FindClose(void* hFindFile);
]]

local pluginDir = debug.getinfo(1, 'S').source:match('@(.+)[/\\]')
local basePath = pluginDir .. '/pssg2fs/'
writeLog('Plugin directory: ' .. pluginDir)
writeLog('Base path: ' .. basePath)

local FILE_ATTRIBUTE_DIRECTORY = 0x10
local INVALID_HANDLE_VALUE = ffi.cast('void*', -1)

local fileCatalog = {}

local function scanDir(dir, rel)
    local search = dir .. '\\*'
    local data = ffi.new('WIN32_FIND_DATAA')
    local handle = kernel32.FindFirstFileA(search, data)
    if handle == INVALID_HANDLE_VALUE then return end
    repeat
        local name = ffi.string(data.cFileName)
        if name ~= '.' and name ~= '..' then
            local full = dir .. '\\' .. name
            local r = rel .. name
            if bit.band(data.dwFileAttributes, FILE_ATTRIBUTE_DIRECTORY) ~= 0 then
                scanDir(full, r .. '\\')
            else
                fileCatalog[r:lower()] = full
            end
        end
    until kernel32.FindNextFileA(handle, data) == 0
    kernel32.FindClose(handle)
end

scanDir(basePath, '')
local count = 0
for _ in pairs(fileCatalog) do count = count + 1 end
writeLog('Cataloged ' .. tostring(count) .. ' files')

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
    local ptr = Memory.ReadMemory(exeBase + 0x509654, 4)
    if not ptr or ptr == 0 then return end
    local original = readString(ptr)
    writeLog('Intercepted path: ' .. original)
    local relative = original
    if gameDir ~= '' then
        local pattern = '^' .. escapePattern(gameDir) .. '[\\/]*'
        relative = relative:gsub(pattern, '')
    end
    writeLog('Relative path: ' .. relative)
    local lookup = fileCatalog[relative:lower()]
    if lookup then
        writeLog('Redirecting to: ' .. lookup)
        local mem = Memory.AllocateMemory(#lookup + 1)
        for i = 1, #lookup do
            Memory.WriteMemory(mem + i - 1, lookup:byte(i), 1)
        end
        Memory.WriteMemory(mem + #lookup, 0, 1)
        Memory.WriteMemory(exeBase + 0x509654, mem, 4)
    else
        writeLog('File not found, using original')
    end
end

function OnFrame()
    if not breakpointSet then
        local base = Memory.GetModuleBase('F1_2012.exe')
        if base and Debug.SetBreakpoint(base + 0x4C3306, 'OnBreakpoint') then
            SCRIPT_RESULT = 'pssg2fs active'
            writeLog(string.format('Breakpoint set at 0x%X', base + 0x4C3306))
            breakpointSet = true
        else
            SCRIPT_RESULT = 'pssg2fs failed'
            writeLog(string.format('Failed to set breakpoint at 0x%X', base + 0x4C3306))
        end
        return true
    end
    return false
end
