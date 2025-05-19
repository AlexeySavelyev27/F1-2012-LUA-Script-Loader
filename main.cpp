// F1 2012 LUA Loader
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <thread>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <wrl/client.h>
#include <fstream>
#include <sstream>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <MinHook.h>
#include <lua.hpp>
#include <chrono>
#include <iomanip>
#include <atomic>
#include <excpt.h>

// Handle filesystem based on compiler support
#if defined(_MSC_VER) && _MSC_VER >= 1914
#include <filesystem>
namespace fs = std::filesystem;
#elif defined(__has_include)
#if __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#error "No filesystem support found"
#endif
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "libMinHook.x86.lib")
#pragma comment(lib, "lua51.lib")

using namespace std;
using Clock = std::chrono::steady_clock;

// Helper functions for older C++ standards
bool string_ends_with(const std::string& str, const std::string& suffix) {
    if (str.length() < suffix.length()) {
        return false;
    }
    return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}

template<typename Map, typename Key>
bool map_contains(const Map& m, const Key& k) {
    return m.find(k) != m.end();
}

// --- Structures ---
struct ProcessorRegisters {
    DWORD eax;
    DWORD ebx;
    DWORD ecx;
    DWORD edx;
    DWORD esi;
    DWORD edi;
    DWORD ebp;
    DWORD esp;
    DWORD eip;  // Added instruction pointer for debugging
};

struct BreakpointInfo {
    BYTE originalByte;
    std::string callbackName;
    bool active;
};

struct HookConfig {
    string name = "F1 2012 LUA Loader";
    string version = "1.0";
    string toggleKey = "F9";
    string closeKey = "F10";
    string reloadKey = "F8";
    string pluginFolder = "plugins";
    bool showOnStartup = false;
    int colorR = 0, colorG = 64, colorB = 0, colorA = 100;
    string overlayPosition = "bottom";
    bool enableLogging = true;
};

struct Plugin {
    string name, version, author;
    string statusInfo, status;
    string luaPath;
    string executionResult;
    map<string, string> iniData;
};

// --- Globals ---
HookConfig config;
vector<Plugin> plugins;
int currentPlugin = 0;
bool overlayVisible = false;
bool initialized = false;
ProcessorRegisters currentRegisters = {};
HWND hwnd = nullptr;
WNDPROC oWndProc = nullptr;
Microsoft::WRL::ComPtr<ID3D11Device> device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> mainRenderTargetView;
lua_State* L = nullptr;
std::atomic<bool> stopMonitoring{ false };
std::unordered_map<std::string, Plugin> loadedPlugins;
std::thread monitorThread;
int currentWidth = 0;
int currentHeight = 0;
std::map<DWORD, BreakpointInfo> breakpointInfo;
bool insideBreakpointHandler = false;
PVOID vehHandle = nullptr;
int lua_ProtectMemory(lua_State* L);


// --- Forward declarations ---
DWORD WINAPI MainThread(LPVOID);
void Log(const string& msg);
void InitLog(bool enableLogging);
LONG CALLBACK BreakpointExceptionHandler(EXCEPTION_POINTERS* ExceptionInfo);
void LoadPluginsWithoutExecution();
void ExecuteAllPlugins();
void RebuildPluginsVector();
void UpdateSinglePlugin(const std::string& baseName);
void MonitorDirectoryChanges(const std::string& directory);
void InitHook();
void RenderOverlay();
void CallPluginOnFrame();
string ExecuteLuaScript(const string& scriptPath);
int GetVirtualKeyFromName(const string& keyName);
void SetupLuaKeyboardAPI();
void RefreshCurrentPluginStatus();

// --- Logging ---
ofstream logFile;

void InitLog(bool enableLogging) {
    if (!enableLogging) return;

    if (logFile.is_open()) {
        logFile.close();
    }

    logFile.open("dinput8_log.txt", ios::out);
    if (logFile.is_open()) {
        logFile << "[LOG] --- New Session ---" << endl;
    }
}

void Log(const string& msg) {
    if (!config.enableLogging) return;

    if (!logFile.is_open()) {
        logFile.open("dinput8_log.txt", ios::app);
        if (!logFile.is_open()) return;
    }

    time_t now = time(nullptr);
    logFile << "[" << now << "] " << msg << endl;
    logFile.flush();
}

// --- INI Parser ---
map<string, string> ParseIni(const string& path) {
    ifstream file(path);
    map<string, string> data;
    string line, section;

    while (getline(file, line)) {
        if (line.empty() || line[0] == ';') continue;

        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }

        size_t eq = line.find('=');
        if (eq != string::npos) {
            string key = line.substr(0, eq);
            string value = line.substr(eq + 1);
            data[section + "." + key] = value;
        }
    }
    return data;
}

// --- Config Loading ---
void LoadConfig() {
    auto ini = ParseIni("dinput8_config.ini");

    // Use map_contains instead of contains for compatibility
    config.name = map_contains(ini, "hook.name") ? ini["hook.name"] : "F1 2012 LUA Loader";
    config.version = map_contains(ini, "hook.version") ? ini["hook.version"] : "1.0";
    config.toggleKey = map_contains(ini, "hook.toggleKey") ? ini["hook.toggleKey"] : "F9";
    config.closeKey = map_contains(ini, "hook.closeKey") ? ini["hook.closeKey"] : "F10";
    config.reloadKey = map_contains(ini, "hook.reloadKey") ? ini["hook.reloadKey"] : "F8";
    config.pluginFolder = map_contains(ini, "hook.pluginFolder") ? ini["hook.pluginFolder"] : "plugins";
    config.showOnStartup = map_contains(ini, "hook.showOnStartup") ? (ini["hook.showOnStartup"] == "1") : true;

    if (map_contains(ini, "hook.overlayColor")) {
        sscanf_s(ini["hook.overlayColor"].c_str(), "%d,%d,%d,%d",
            &config.colorR, &config.colorG, &config.colorB, &config.colorA);
    }

    config.overlayPosition = map_contains(ini, "hook.overlayPosition") ? ini["hook.overlayPosition"] : "bottom";
    config.enableLogging = map_contains(ini, "hook.enableLogging") ? (ini["hook.enableLogging"] == "1") : true;

    InitLog(config.enableLogging);

    if (config.enableLogging) {
        Log("Loaded config from dinput8_config.ini");
        Log("Logging is enabled");
        Log("Reload key set to: " + config.reloadKey);
        Log("Overlay position set to: " + config.overlayPosition);
        Log("Show on startup: " + string(config.showOnStartup ? "Yes" : "No"));
    }
}

// --- Key Mapping ---
int GetVirtualKeyFromName(const string& keyName) {
    static const std::unordered_map<std::string, int> keyMap = {
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
        {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
        {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12}
    };

    auto it = keyMap.find(keyName);
    if (it != keyMap.end()) {
        return it->second;
    }

    if (!keyName.empty()) {
        return toupper(keyName[0]);
    }
    return 0;
}

// --- Lua Script Execution ---
string ExecuteLuaScript(const string& scriptPath) {
    if (!L) return "Lua engine not initialized";

    int top = lua_gettop(L);

    // Set default result to empty
    lua_pushstring(L, "");
    lua_setglobal(L, "SCRIPT_RESULT");

    Log("Executing lua script: " + scriptPath);

    // Execute the script
    int result = luaL_dofile(L, scriptPath.c_str());
    string executionResult;

    if (result != LUA_OK) {
        // Error occurred
        executionResult = "Error: " + string(lua_tostring(L, -1));
        lua_pop(L, 1);
        Log("Failed to execute Lua: " + scriptPath + " - " + executionResult);
    }
    else {
        // Check for SCRIPT_RESULT
        lua_getglobal(L, "SCRIPT_RESULT");
        if (lua_isstring(L, -1)) {
            executionResult = lua_tostring(L, -1);
            // If empty or nil, provide a default successful message
            if (executionResult.empty()) {
                executionResult = "Loaded successfully";
            }
            Log("Script executed successfully: " + scriptPath + " - Result: " + executionResult);
        }
        else {
            executionResult = "Loaded successfully";
            Log("Script executed successfully: " + scriptPath);
        }
        lua_pop(L, 1);
    }

    lua_settop(L, top);
    return executionResult;
}

// Force refresh the status of the current plugin
void RefreshCurrentPluginStatus() {
    if (plugins.empty()) return;

    auto& plugin = plugins[currentPlugin];
    plugin.executionResult = ExecuteLuaScript(plugin.luaPath);
    plugin.status = plugin.executionResult;

    // Update in loadedPlugins too
    for (auto& pair : loadedPlugins) {
        if (pair.second.luaPath == plugin.luaPath) {
            pair.second.executionResult = plugin.executionResult;
            pair.second.status = plugin.status;
            break;
        }
    }

    Log("Refreshed plugin status: " + plugin.name + " = " + plugin.executionResult);
}

// Define C-style functions for Lua API
int lua_IsKeyDown(lua_State* L) {
    int keyCode = luaL_checkinteger(L, 1);
    bool isDown = (GetAsyncKeyState(keyCode) & 0x8000) != 0;
    lua_pushboolean(L, isDown);
    return 1;
}

int lua_IsKeyPressed(lua_State* L) {
    int keyCode = luaL_checkinteger(L, 1);
    bool isPressed = (GetAsyncKeyState(keyCode) & 1) != 0;
    lua_pushboolean(L, isPressed);
    return 1;
}

int lua_ReadMemory(lua_State* L) {
    DWORD64 address = (DWORD64)luaL_checkinteger(L, 1);
    size_t size = luaL_checkinteger(L, 2);
    vector<BYTE> buffer(size);
    SIZE_T bytesRead;

    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, buffer.data(), size, &bytesRead)) {
        switch (size) {
        case 1: lua_pushinteger(L, buffer[0]); break;
        case 2: lua_pushinteger(L, *(WORD*)buffer.data()); break;
        case 4: lua_pushinteger(L, *(DWORD*)buffer.data()); break;
        case 8: lua_pushinteger(L, *(DWORD64*)buffer.data()); break;
        default: lua_pushnil(L); break;
        }
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int lua_WriteMemory(lua_State* L) {
    DWORD64 address = (DWORD64)luaL_checkinteger(L, 1);
    DWORD64 value = (DWORD64)luaL_checkinteger(L, 2);
    size_t size = luaL_checkinteger(L, 3);
    DWORD oldProtect;

    VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect);
    SIZE_T bytesWritten;
    bool result = WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, &value, size, &bytesWritten);
    VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);

    lua_pushboolean(L, result);
    return 1;
}

int lua_AllocateMemory(lua_State* L) {
    size_t size = luaL_checkinteger(L, 1);
    void* memory = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (memory) {
        lua_pushinteger(L, (lua_Integer)memory);
    }
    else {
        lua_pushnil(L);
    }
    return 1;
}

// Frees memory previously allocated with AllocateMemory
int lua_FreeMemory(lua_State* L) {
    void* memory = (void*)luaL_checkinteger(L, 1);
    BOOL result = VirtualFree(memory, 0, MEM_RELEASE);
    lua_pushboolean(L, result);
    return 1;
}

int lua_GetModuleBase(lua_State* L) {
    const char* moduleName = luaL_checkstring(L, 1);
    HMODULE hModule = GetModuleHandleA(moduleName);
    if (hModule) {
        lua_pushinteger(L, (lua_Integer)hModule);
    }
    else {
        lua_pushnil(L);
    }
    return 1;
}

int lua_GetRegisters(lua_State* L) {
    lua_newtable(L);

    lua_pushstring(L, "eax"); lua_pushinteger(L, currentRegisters.eax); lua_settable(L, -3);
    lua_pushstring(L, "ebx"); lua_pushinteger(L, currentRegisters.ebx); lua_settable(L, -3);
    lua_pushstring(L, "ecx"); lua_pushinteger(L, currentRegisters.ecx); lua_settable(L, -3);
    lua_pushstring(L, "edx"); lua_pushinteger(L, currentRegisters.edx); lua_settable(L, -3);
    lua_pushstring(L, "esi"); lua_pushinteger(L, currentRegisters.esi); lua_settable(L, -3);
    lua_pushstring(L, "edi"); lua_pushinteger(L, currentRegisters.edi); lua_settable(L, -3);
    lua_pushstring(L, "ebp"); lua_pushinteger(L, currentRegisters.ebp); lua_settable(L, -3);
    lua_pushstring(L, "esp"); lua_pushinteger(L, currentRegisters.esp); lua_settable(L, -3);
    lua_pushstring(L, "eip"); lua_pushinteger(L, currentRegisters.eip); lua_settable(L, -3);

    return 1;
}

int lua_SetBreakpoint(lua_State* L) {
    DWORD address = static_cast<DWORD>(luaL_checkinteger(L, 1));

    const char* callbackName = "OnBreakpoint";  // Default name
    if (lua_isstring(L, 2)) {
        callbackName = lua_tostring(L, 2);
    }

    if (map_contains(breakpointInfo, address)) {
        Log("Breakpoint already exists at 0x" + std::to_string(address));
        lua_pushboolean(L, true);
        return 1;
    }

    BYTE origByte;
    DWORD oldProtect;
    BOOL success = FALSE;

    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, PAGE_EXECUTE_READWRITE, &oldProtect);

    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
        &origByte, 1, nullptr)) {
        BYTE int3 = 0xCC;
        if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address),
            &int3, 1, nullptr)) {
            success = TRUE;
            BreakpointInfo info = { origByte, callbackName, true };
            breakpointInfo[address] = info;
            Log("Breakpoint set at 0x" + std::to_string(address) + " callback: " + callbackName);
        }
    }

    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, oldProtect, &oldProtect);

    lua_pushboolean(L, success);
    return 1;
}

int lua_RemoveBreakpoint(lua_State* L) {
    DWORD address = static_cast<DWORD>(luaL_checkinteger(L, 1));

    if (!map_contains(breakpointInfo, address)) {
        lua_pushboolean(L, false);
        return 1;
    }

    BYTE origByte = breakpointInfo[address].originalByte;
    DWORD oldProtect;
    BOOL success = FALSE;

    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, PAGE_EXECUTE_READWRITE, &oldProtect);

    if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address),
        &origByte, 1, nullptr)) {
        success = TRUE;
        Log("Breakpoint removed from 0x" + std::to_string(address));
        breakpointInfo.erase(address);
    }

    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, oldProtect, &oldProtect);

    lua_pushboolean(L, success);
    return 1;
}

int lua_EnableBreakpoint(lua_State* L) {
    DWORD address = static_cast<DWORD>(luaL_checkinteger(L, 1));
    bool enable = lua_toboolean(L, 2);

    if (!map_contains(breakpointInfo, address)) {
        lua_pushboolean(L, false);
        return 1;
    }

    BOOL success = FALSE;
    DWORD oldProtect;

    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, PAGE_EXECUTE_READWRITE, &oldProtect);

    if (enable && !breakpointInfo[address].active) {
        BYTE int3 = 0xCC;
        if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address),
            &int3, 1, nullptr)) {
            breakpointInfo[address].active = true;
            success = TRUE;
            Log("Breakpoint enabled at 0x" + std::to_string(address));
        }
    }
    else if (!enable && breakpointInfo[address].active) {
        if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address),
            &breakpointInfo[address].originalByte, 1, nullptr)) {
            breakpointInfo[address].active = false;
            success = TRUE;
            Log("Breakpoint disabled at 0x" + std::to_string(address));
        }
    }
    else {
        success = TRUE;
    }

    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, oldProtect, &oldProtect);

    lua_pushboolean(L, success);
    return 1;
}

int lua_ListBreakpoints(lua_State* L) {
    lua_newtable(L);
    int index = 1;

    for (const auto& pair : breakpointInfo) {
        DWORD address = pair.first;
        const BreakpointInfo& info = pair.second;

        lua_pushnumber(L, index++);

        lua_newtable(L);
        lua_pushstring(L, "address");
        lua_pushinteger(L, static_cast<lua_Integer>(address));
        lua_settable(L, -3);

        lua_pushstring(L, "active");
        lua_pushboolean(L, info.active);
        lua_settable(L, -3);

        lua_pushstring(L, "callback");
        lua_pushstring(L, info.callbackName.c_str());
        lua_settable(L, -3);

        lua_settable(L, -3);
    }

    return 1;
}

// Функция для изменения прав доступа к памяти
int lua_ProtectMemory(lua_State* L) {
    DWORD64 address = (DWORD64)luaL_checkinteger(L, 1);
    size_t size = luaL_checkinteger(L, 2);
    DWORD newProtect = luaL_checkinteger(L, 3);
    DWORD oldProtect;

    BOOL result = VirtualProtect((LPVOID)address, size, newProtect, &oldProtect);

    lua_pushboolean(L, result);
    lua_pushinteger(L, oldProtect);
    return 2;
}

// --- Lua API Setup ---
void SetupLuaKeyboardAPI() {
    if (!L) return;

    // Keyboard API
    lua_newtable(L);

    lua_pushstring(L, "IsKeyDown");
    lua_pushcfunction(L, lua_IsKeyDown);
    lua_settable(L, -3);

    lua_pushstring(L, "IsKeyPressed");
    lua_pushcfunction(L, lua_IsKeyPressed);
    lua_settable(L, -3);

    lua_setglobal(L, "Keyboard");

    // Virtual Key Constants
    lua_newtable(L);

    // Basic keys
    lua_pushstring(L, "VK_PLUS"); lua_pushinteger(L, VK_OEM_PLUS); lua_settable(L, -3);
    lua_pushstring(L, "VK_MINUS"); lua_pushinteger(L, VK_OEM_MINUS); lua_settable(L, -3);
    lua_pushstring(L, "VK_SPACE"); lua_pushinteger(L, VK_SPACE); lua_settable(L, -3);
    lua_pushstring(L, "VK_ENTER"); lua_pushinteger(L, VK_RETURN); lua_settable(L, -3);
    lua_pushstring(L, "VK_ESCAPE"); lua_pushinteger(L, VK_ESCAPE); lua_settable(L, -3);

    // Numbers
    for (int i = 0; i <= 9; i++) {
        string keyName = "VK_" + to_string(i);
        lua_pushstring(L, keyName.c_str());
        lua_pushinteger(L, 0x30 + i);
        lua_settable(L, -3);
    }

    // Letters
    for (char c = 'A'; c <= 'Z'; c++) {
        string keyName = "VK_";
        keyName += c;
        lua_pushstring(L, keyName.c_str());
        lua_pushinteger(L, c);
        lua_settable(L, -3);
    }

    // Function keys
    for (int i = 1; i <= 12; i++) {
        string keyName = "VK_F" + to_string(i);
        lua_pushstring(L, keyName.c_str());
        lua_pushinteger(L, VK_F1 + i - 1);
        lua_settable(L, -3);
    }

    lua_setglobal(L, "Keys");

    // Memory API
    lua_newtable(L);

    lua_pushstring(L, "ReadMemory");
    lua_pushcfunction(L, lua_ReadMemory);
    lua_settable(L, -3);

    lua_pushstring(L, "WriteMemory");
    lua_pushcfunction(L, lua_WriteMemory);
    lua_settable(L, -3);

    lua_pushstring(L, "GetModuleBase");
    lua_pushcfunction(L, lua_GetModuleBase);
    lua_settable(L, -3);

    lua_pushstring(L, "AllocateMemory");
    lua_pushcfunction(L, lua_AllocateMemory);
    lua_settable(L, -3);

    // FreeMemory
    lua_pushstring(L, "FreeMemory");
    lua_pushcfunction(L, lua_FreeMemory);
    lua_settable(L, -3);

    lua_pushstring(L, "ProtectMemory");
    lua_pushcfunction(L, lua_ProtectMemory);
    lua_settable(L, -3);

    lua_setglobal(L, "Memory");

    // Registers API
    lua_newtable(L);

    lua_pushstring(L, "Get");
    lua_pushcfunction(L, lua_GetRegisters);
    lua_settable(L, -3);

    lua_setglobal(L, "Registers");

    // Debug API
    lua_newtable(L);

    // SetBreakpoint
    lua_pushstring(L, "SetBreakpoint");
    lua_pushcfunction(L, lua_SetBreakpoint);
    lua_settable(L, -3);

    // RemoveBreakpoint
    lua_pushstring(L, "RemoveBreakpoint");
    lua_pushcfunction(L, lua_RemoveBreakpoint);
    lua_settable(L, -3);

    // EnableBreakpoint
    lua_pushstring(L, "EnableBreakpoint");
    lua_pushcfunction(L, lua_EnableBreakpoint);
    lua_settable(L, -3);

    // ListBreakpoints
    lua_pushstring(L, "ListBreakpoints");
    lua_pushcfunction(L, lua_ListBreakpoints);
    lua_settable(L, -3);

    // Set the Debug table globally
    lua_setglobal(L, "Debug");

    Log("Lua API setup complete");
}

// --- Plugin Management ---
void RebuildPluginsVector() {
    plugins.clear();
    for (const auto& pair : loadedPlugins) {
        plugins.push_back(pair.second);
    }
}

void LoadPluginsWithoutExecution() {
    std::unordered_map<std::string, Plugin> newPlugins;

    if (!fs::exists(config.pluginFolder)) {
        try {
            fs::create_directory(config.pluginFolder);
            Log("Created plugin folder: " + config.pluginFolder);
        }
        catch (const std::exception& e) {
            Log("Failed to create plugin folder: " + std::string(e.what()));
            return;
        }
    }

    for (auto& entry : fs::directory_iterator(config.pluginFolder)) {
        if (entry.path().extension() != ".ini") continue;

        string base = entry.path().stem().string();
        string iniPath = entry.path().string();
        string luaPath = entry.path().parent_path().string() + "/" + base + ".lua";

        if (!fs::exists(luaPath)) continue;

        auto ini = ParseIni(iniPath);
        Plugin plugin;
        plugin.name = map_contains(ini, "meta.name") ? ini["meta.name"] : "Unnamed";
        plugin.version = map_contains(ini, "meta.version") ? ini["meta.version"] : "Unknown";
        plugin.author = map_contains(ini, "meta.author") ? ini["meta.author"] : "Anonymous";
        plugin.statusInfo = map_contains(ini, "status.info") ? ini["status.info"] : "";
        plugin.luaPath = luaPath;
        plugin.iniData = ini;
        plugin.executionResult = "Pending execution";
        plugin.status = plugin.executionResult;

        Log("Parsing plugin: " + entry.path().string());
        Log("Name: " + plugin.name + " | Version: " + plugin.version + " | Author: " + plugin.author);

        newPlugins[base] = plugin;
    }

    loadedPlugins = newPlugins;
    RebuildPluginsVector();
    Log("Loaded " + std::to_string(plugins.size()) + " plugins (not executed yet)");
}

void ExecuteAllPlugins() {
    Log("Executing all " + std::to_string(plugins.size()) + " plugins");

    for (size_t i = 0; i < plugins.size(); ++i) {
        auto& plugin = plugins[i];
        try {
            Log("Executing plugin " + std::to_string(i + 1) + "/" +
                std::to_string(plugins.size()) + ": " + plugin.name);

            // Execute the plugin and capture its result
            std::string result = ExecuteLuaScript(plugin.luaPath);

            // Make sure we have a meaningful result
            if (result.empty()) {
                result = "Loaded successfully";
            }

            // Update the status in both places
            plugin.executionResult = result;
            plugin.status = result;

            // Find and update in loadedPlugins map
            for (auto& pair : loadedPlugins) {
                if (pair.second.luaPath == plugin.luaPath) {
                    pair.second.executionResult = result;
                    pair.second.status = result;
                    break;
                }
            }

            Log("Plugin " + plugin.name + " executed with result: " + result);
        }
        catch (const std::exception& e) {
            std::string errorMsg = "Error: " + std::string(e.what());
            Log("EXCEPTION executing " + plugin.name + ": " + errorMsg);

            // Update error status in both places
            plugin.executionResult = errorMsg;
            plugin.status = errorMsg;

            // Find and update in loadedPlugins map
            for (auto& pair : loadedPlugins) {
                if (pair.second.luaPath == plugin.luaPath) {
                    pair.second.executionResult = errorMsg;
                    pair.second.status = errorMsg;
                    break;
                }
            }
        }
    }

    // Rebuild plugins vector to ensure all changes are reflected
    RebuildPluginsVector();
    Log("All plugins executed");
}

void UpdateSinglePlugin(const std::string& baseName) {
    static std::unordered_map<std::string, Plugin> lastPluginState;

    std::string iniPath = config.pluginFolder + "/" + baseName + ".ini";
    std::string luaPath = config.pluginFolder + "/" + baseName + ".lua";

    // Check if files exist
    if (GetFileAttributesA(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(luaPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (map_contains(loadedPlugins, baseName)) {
            Log("Plugin removed: " + baseName);
            loadedPlugins.erase(baseName);
            lastPluginState.erase(baseName);
        }
        return;
    }

    auto ini = ParseIni(iniPath);
    Plugin plugin;
    plugin.name = map_contains(ini, "meta.name") ? ini["meta.name"] : "Unnamed";
    plugin.version = map_contains(ini, "meta.version") ? ini["meta.version"] : "Unknown";
    plugin.author = map_contains(ini, "meta.author") ? ini["meta.author"] : "Anonymous";
    plugin.statusInfo = map_contains(ini, "status.info") ? ini["status.info"] : "";
    plugin.luaPath = luaPath;
    plugin.iniData = ini;

    // Check if plugin has changed
    bool isChanged = true;
    if (map_contains(lastPluginState, baseName)) {
        const auto& lastPlugin = lastPluginState[baseName];
        isChanged = (lastPlugin.name != plugin.name ||
            lastPlugin.version != plugin.version ||
            lastPlugin.author != plugin.author ||
            lastPlugin.statusInfo != plugin.statusInfo);
    }

    if (!isChanged) {
        return;
    }

    bool isNew = !map_contains(loadedPlugins, baseName);

    try {
        Log("Executing updated plugin: " + plugin.name);
        plugin.executionResult = ExecuteLuaScript(plugin.luaPath);
        plugin.status = plugin.executionResult;
    }
    catch (const std::exception& e) {
        Log("EXCEPTION executing " + plugin.name + ": " + e.what());
        plugin.executionResult = "Error: " + std::string(e.what());
        plugin.status = plugin.executionResult;
    }

    loadedPlugins[baseName] = plugin;
    lastPluginState[baseName] = plugin;

    if (isNew) {
        Log("New plugin detected and executed: " + baseName);
    }
    else {
        Log("Plugin updated and re-executed: " + baseName);
    }
}

void MonitorDirectoryChanges(const std::string& directory) {
    std::wstring dirW(directory.begin(), directory.end());
    HANDLE hDir = CreateFileW(dirW.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (hDir == INVALID_HANDLE_VALUE) {
        Log("Failed to monitor directory: " + directory);
        return;
    }

    char buffer[1024];
    DWORD bytesReturned;

    while (!stopMonitoring) {
        if (ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                &bytesReturned, nullptr, nullptr)) {
            FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
            bool changed = false;
            do {
                std::wstring wname(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                std::string fileName(wname.begin(), wname.end());
                if (string_ends_with(fileName, ".lua") || string_ends_with(fileName, ".ini")) {
                    std::string baseName = fileName.substr(0, fileName.find_last_of('.'));
                    UpdateSinglePlugin(baseName);
                    changed = true;
                }
                if (fni->NextEntryOffset == 0) break;
                fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(fni) + fni->NextEntryOffset);
            } while (true);

            if (changed) {
                RebuildPluginsVector();
            }
        } else {
            Sleep(500);
        }
    }

    CloseHandle(hDir);
}

// --- Exception Handler for Breakpoints ---
LONG CALLBACK BreakpointExceptionHandler(EXCEPTION_POINTERS* ExceptionInfo) {
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        // Get the exception address (where INT3 was triggered)
        DWORD exceptionAddress = (DWORD)ExceptionInfo->ExceptionRecord->ExceptionAddress;

        // Check if this is our breakpoint
        if (map_contains(breakpointInfo, exceptionAddress) &&
            breakpointInfo[exceptionAddress].active && !insideBreakpointHandler) {

            insideBreakpointHandler = true; // Prevent recursion

            // Temporarily restore the original byte
            BYTE origByte = breakpointInfo[exceptionAddress].originalByte;
            DWORD oldProtect;
            VirtualProtect((LPVOID)exceptionAddress, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
            WriteProcessMemory(GetCurrentProcess(), (LPVOID)exceptionAddress, &origByte, 1, nullptr);
            VirtualProtect((LPVOID)exceptionAddress, 1, oldProtect, &oldProtect);

            // Save current registers from exception context
            currentRegisters.eax = ExceptionInfo->ContextRecord->Eax;
            currentRegisters.ebx = ExceptionInfo->ContextRecord->Ebx;
            currentRegisters.ecx = ExceptionInfo->ContextRecord->Ecx;
            currentRegisters.edx = ExceptionInfo->ContextRecord->Edx;
            currentRegisters.esi = ExceptionInfo->ContextRecord->Esi;
            currentRegisters.edi = ExceptionInfo->ContextRecord->Edi;
            currentRegisters.ebp = ExceptionInfo->ContextRecord->Ebp;
            currentRegisters.esp = ExceptionInfo->ContextRecord->Esp;
            currentRegisters.eip = exceptionAddress;

            Log("Breakpoint hit at 0x" + std::to_string(exceptionAddress));

            // Call Lua callback
            if (L) {
                std::string callbackName = breakpointInfo[exceptionAddress].callbackName;
                lua_getglobal(L, callbackName.c_str());
                if (lua_isfunction(L, -1)) {
                    lua_pushinteger(L, exceptionAddress);
                    if (lua_pcall(L, 1, 0, 0) != 0) {
                        std::string error = lua_tostring(L, -1);
                        Log("Error in breakpoint callback: " + error);
                        lua_pop(L, 1);
                    }
                }
                else {
                    lua_pop(L, 1); // Pop non-function
                    Log("Breakpoint callback function not found: " + callbackName);
                }
            }

            // Create a copy of the address for the thread
            DWORD addressCopy = exceptionAddress;

            // Create a thread to reset the breakpoint after the original instruction is executed
            std::thread([addressCopy]() {
                // Give time for the original instruction to execute
                Sleep(1);

                if (!stopMonitoring && map_contains(breakpointInfo, addressCopy)) {
                    // Put INT3 back
                    BYTE int3 = 0xCC;
                    DWORD oldProtect;
                    VirtualProtect((LPVOID)addressCopy, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
                    WriteProcessMemory(GetCurrentProcess(), (LPVOID)addressCopy, &int3, 1, nullptr);
                    VirtualProtect((LPVOID)addressCopy, 1, oldProtect, &oldProtect);
                }

                insideBreakpointHandler = false;
                }).detach();

            // Continue execution with the original instruction
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // For other exceptions or breakpoints that aren't ours
    return EXCEPTION_CONTINUE_SEARCH;
}

// --- Window Processing ---
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (overlayVisible && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (device && wParam != SIZE_MINIMIZED) {
            Log("WM_SIZE received in WndProc");
            if (mainRenderTargetView) {
                mainRenderTargetView.Reset();
            }
        }
        break;
    }
    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

// --- Render Functions ---
void RenderOverlay() {
    if (plugins.empty()) return;
    static bool firstShow = true;

    // Get the currently selected plugin
    auto& p = plugins[currentPlugin];

    // If this is the first time showing the overlay, force plugin execution
    if (firstShow && overlayVisible) {
        // We'll check if the status seems empty
        if (p.executionResult.empty() || p.executionResult == "Pending execution") {
            Log("First-time display: Refreshing plugin status for " + p.name);
            p.executionResult = ExecuteLuaScript(p.luaPath);
            p.status = p.executionResult;

            // Update loadedPlugins map too
            for (auto& pair : loadedPlugins) {
                if (pair.second.luaPath == p.luaPath) {
                    pair.second.executionResult = p.executionResult;
                    pair.second.status = p.status;
                    break;
                }
            }
        }
        firstShow = false;
    }

    ImGuiIO& io = ImGui::GetIO();

    // Two-pass rendering to determine exact height
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(config.colorR / 255.f, config.colorG / 255.f, config.colorB / 255.f, config.colorA / 100.f));

    // First pass - use invisible window to calculate height
    ImGui::SetNextWindowPos(ImVec2(-9999, -9999)); // Place off-screen
    ImGui::Begin("HeightCalc", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);

    // Set fixed width equal to screen width
    ImGui::SetWindowSize(ImVec2(io.DisplaySize.x, -1)); // Width = screen, height = automatic

    // Display content with current plugin info
    ImGui::Text("%s v%s | Plugin %d/%d | %s", config.name.c_str(), config.version.c_str(),
        currentPlugin + 1, static_cast<int>(plugins.size()), p.luaPath.c_str());
    ImGui::Separator();
    ImGui::Text("%s | v. %s", p.name.c_str(), p.version.c_str());
    ImGui::Text("by %s", p.author.c_str());
    ImGui::Spacing();
    ImGui::Text("Info: %s", p.statusInfo.c_str());
    ImGui::Text("Plugin Status: %s", p.executionResult.c_str());

    // Get exact content size
    float contentHeight = ImGui::GetWindowHeight();
    ImGui::End();

    // Second pass - draw real window with correct height
    ImVec2 windowPos;
    if (config.overlayPosition == "top") {
        windowPos = ImVec2(0, 0); // Top of screen
    }
    else {
        windowPos = ImVec2(0, io.DisplaySize.y - contentHeight); // Bottom of screen (default)
    }

    ImGui::SetNextWindowPos(windowPos);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, contentHeight));
    ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

    // Repeat content for actual display with current plugin info
    ImGui::Text("%s v%s | Plugin %d/%d | %s", config.name.c_str(), config.version.c_str(),
        currentPlugin + 1, static_cast<int>(plugins.size()), p.luaPath.c_str());
    ImGui::Separator();
    ImGui::Text("%s | v. %s", p.name.c_str(), p.version.c_str());
    ImGui::Text("by %s", p.author.c_str());
    ImGui::Spacing();
    ImGui::Text("Info: %s", p.statusInfo.c_str());
    ImGui::Text("Plugin Status: %s", p.executionResult.c_str());

    ImGui::End();
    ImGui::PopStyleColor();
}

// --- Plugin OnFrame Execution ---
void CallPluginOnFrame() {
    if (plugins.empty() || !overlayVisible) return;

    // Get the currently selected plugin
    auto& plugin = plugins[currentPlugin];
    if (!L) return;

    int top = lua_gettop(L);
    lua_getglobal(L, "OnFrame");

    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 1, 0) != 0) {
            std::string error = lua_tostring(L, -1);
            Log("Error in OnFrame: " + error);
        }
        else {
            if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
                lua_getglobal(L, "SCRIPT_RESULT");
                if (lua_isstring(L, -1)) {
                    std::string newResult = lua_tostring(L, -1);

                    // Update status only for the currently selected plugin
                    if (newResult != plugin.executionResult) {
                        plugin.executionResult = newResult;
                        plugin.status = newResult;

                        // Find and update the matching plugin in loadedPlugins map
                        for (auto& pair : loadedPlugins) {
                            if (pair.second.luaPath == plugin.luaPath) {
                                pair.second.executionResult = newResult;
                                pair.second.status = newResult;
                                break;
                            }
                        }
                    }
                }
                lua_pop(L, 1);
            }
        }
    }
    lua_settop(L, top);
}

// --- DirectX Hook ---
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
PresentFn oPresent = nullptr;

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwap, UINT SyncInterval, UINT Flags) {
    static bool firstRun = true;

    if (!initialized) {
        if (SUCCEEDED(pSwap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.GetAddressOf())))) {
            device->GetImmediateContext(context.GetAddressOf());
            DXGI_SWAP_CHAIN_DESC sd;
            pSwap->GetDesc(&sd);
            hwnd = sd.OutputWindow;

            currentWidth = sd.BufferDesc.Width;
            currentHeight = sd.BufferDesc.Height;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
            pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf()));
            if (backBuffer) {
                device->CreateRenderTargetView(backBuffer.Get(), nullptr, mainRenderTargetView.GetAddressOf());
            }

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

            ImGui_ImplWin32_Init(hwnd);
            ImGui_ImplDX11_Init(device.Get(), context.Get());

            oWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);

            initialized = true;

            if (config.showOnStartup) {
                overlayVisible = true;
                Log("Overlay shown (showOnStartup = true)");
            }

            Log("DX11 + ImGui initialized");
        }
    }
    else {
        // Save register values using inline assembly
#if defined(_MSC_VER) && defined(_M_IX86)
        __asm {
            mov currentRegisters.eax, eax
            mov currentRegisters.ebx, ebx
            mov currentRegisters.ecx, ecx
            mov currentRegisters.edx, edx
            mov currentRegisters.esi, esi
            mov currentRegisters.edi, edi
            mov currentRegisters.ebp, ebp
            mov currentRegisters.esp, esp
        }
#endif

        if (!mainRenderTargetView) {
            Log("Recreating render target view");
            Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
            HRESULT hr = pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf()));
            if (SUCCEEDED(hr) && backBuffer) {
                device->CreateRenderTargetView(backBuffer.Get(), nullptr, mainRenderTargetView.GetAddressOf());

                ImGui_ImplDX11_InvalidateDeviceObjects();
                ImGui_ImplDX11_CreateDeviceObjects();
                Log("Render target recreated");
            }
        }
    }

    int toggleKey = GetVirtualKeyFromName(config.toggleKey);
    int closeKey = GetVirtualKeyFromName(config.closeKey);
    int reloadKey = GetVirtualKeyFromName(config.reloadKey);

    if ((GetAsyncKeyState(toggleKey) & 1)) {
        if (!overlayVisible) {
            overlayVisible = true;
            Log("Overlay shown (" + config.toggleKey + ")");
        }
        else if (!plugins.empty()) {
            currentPlugin = (currentPlugin + 1) % plugins.size();
            Log("Plugin switched to: " + plugins[currentPlugin].name);
        }
    }

    if ((GetAsyncKeyState(reloadKey) & 1) && overlayVisible && !plugins.empty()) {
        auto& plugin = plugins[currentPlugin];
        plugin.executionResult = ExecuteLuaScript(plugin.luaPath);
        plugin.status = plugin.executionResult;
        Log("Manually re-executed current plugin: " + plugin.name + " using key " + config.reloadKey);
    }

    if (GetAsyncKeyState(closeKey) & 1) {
        overlayVisible = false;
        Log("Overlay hidden (" + config.closeKey + ")");
    }

    if (initialized && overlayVisible) {
        // Make sure the current plugin status is set
        if (!plugins.empty() && (plugins[currentPlugin].executionResult.empty() ||
            plugins[currentPlugin].executionResult == "Pending execution")) {
            RefreshCurrentPluginStatus();
        }

        CallPluginOnFrame();
    }

    if (initialized && mainRenderTargetView) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (overlayVisible) {
            RenderOverlay();
        }

        ImGui::Render();
        context->OMSetRenderTargets(1, mainRenderTargetView.GetAddressOf(), nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return oPresent(pSwap, SyncInterval, Flags);
}

// --- Hook Initialization ---
void InitHook() {
    Log("Initializing DirectX hook...");

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    scDesc.BufferCount = 1;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow = GetForegroundWindow();
    scDesc.SampleDesc.Count = 1;
    scDesc.Windowed = TRUE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    Microsoft::WRL::ComPtr<IDXGISwapChain> tempSwap;
    Microsoft::WRL::ComPtr<ID3D11Device> tempDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> tempContext;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scDesc,
        tempSwap.GetAddressOf(), tempDevice.GetAddressOf(), &featureLevel, tempContext.GetAddressOf()
    );

    if (SUCCEEDED(hr) && tempSwap) {
        void** vTable = *reinterpret_cast<void***>(tempSwap.Get());

        MH_Initialize();
        MH_CreateHook(vTable[8], &hkPresent, reinterpret_cast<void**>(&oPresent));
        MH_EnableHook(vTable[8]);


        Log("Present hook installed successfully");
    }
    else {
        Log("Failed to create dummy swap chain");
    }
}

// --- Main Thread ---
DWORD WINAPI MainThread(LPVOID) {
    Log("Main thread started");

    // Initialize Lua first
    L = luaL_newstate();
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON);
    if (!L) {
        Log("CRITICAL: Failed to initialize Lua!");
        return 1;
    }
    luaL_openlibs(L);
    luaL_dostring(L, "require('ffi')");
    SetupLuaKeyboardAPI();
    Log("Lua engine initialized successfully");

    lua_getglobal(L, "_VERSION");
    const char* version = lua_tostring(L, -1);
    Log(std::string("Lua version: ") + version);  // Должно быть "LuaJIT 2.1.x"

    // Load plugins without execution
    LoadPluginsWithoutExecution();

    // Initialize DirectX hook
    InitHook();

    // We'll wait until DirectX is initialized before executing plugins
    int waitCount = 0;
    while (!initialized && waitCount < 50) { // Wait up to 5 seconds
        Sleep(100);
        waitCount++;
    }

    // Execute all plugins after DirectX initialization
    ExecuteAllPlugins();
    Log("All plugins executed automatically on startup");

    // Ensure the first plugin is selected
    if (!plugins.empty()) {
        currentPlugin = 0;
        Log("Initial plugin selected: " + plugins[currentPlugin].name);
        Log("Initial plugin status: " + plugins[currentPlugin].executionResult);
    }

    // Start monitoring the plugins directory
    monitorThread = std::thread(MonitorDirectoryChanges, config.pluginFolder);

    return 0;
}

// --- DllMain ---
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        // Set up exception handler before starting the main thread
        SetUnhandledExceptionFilter(BreakpointExceptionHandler);
        vehHandle = AddVectoredExceptionHandler(1, BreakpointExceptionHandler);
        LoadConfig();
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        stopMonitoring = true;
        if (monitorThread.joinable()) {
            monitorThread.join();
        }
		
        if (hwnd && oWndProc) {
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
            oWndProc = nullptr;
            hwnd = nullptr;
        }

        // Remove all breakpoints on exit
        for (auto& pair : breakpointInfo) {
            DWORD address = pair.first;
            BreakpointInfo& info = pair.second;

            DWORD oldProtect;
            VirtualProtect((LPVOID)address, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
            WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, &info.originalByte, 1, nullptr);
            VirtualProtect((LPVOID)address, 1, oldProtect, &oldProtect);
        }
        breakpointInfo.clear();

        if (L) {
            lua_close(L);
            L = nullptr;
        }

        if (mainRenderTargetView) {
            mainRenderTargetView.Reset();
        }

        if (context) {
            context.Reset();
        }

        if (device) {
            device.Reset();
        }

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();

        if (vehHandle) {
            RemoveVectoredExceptionHandler(vehHandle);
            vehHandle = nullptr;
        }

        if (logFile.is_open()) {
            Log("DLL detached - shutting down");
            logFile.close();
        }
        break;
    }
    return TRUE;
}

// --- DirectInput8Create Export ---
extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riid,
    LPVOID* ppvOut,
    LPUNKNOWN punkOuter)
{
    Log("DirectInput8Create called");

    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\dinput8.dll");

    HMODULE realDInput = LoadLibraryA(sysPath);
    if (!realDInput) {
        Log("Failed to load system dinput8.dll");
        return E_FAIL;
    }

    using DInputCreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto original = (DInputCreateFn)GetProcAddress(realDInput, "DirectInput8Create");
    if (!original) {
        Log("Failed to get DirectInput8Create address");
        return E_FAIL;
    }

    return original(hinst, dwVersion, riid, ppvOut, punkOuter);
}