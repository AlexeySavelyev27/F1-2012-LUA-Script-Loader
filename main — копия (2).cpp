// F1 2012 LUA Loader
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <thread>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "libMinHook.x86.lib")
#pragma comment(lib, "lua5.1.lib")

using namespace std;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

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
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
ID3D11RenderTargetView* mainRenderTargetView = nullptr;
lua_State* L = nullptr;
std::atomic<bool> stopMonitoring{ false };
std::map<std::string, Plugin> loadedPlugins;
int currentWidth = 0;
int currentHeight = 0;

// --- Forward declarations ---
DWORD WINAPI MainThread(LPVOID);
void Log(const string& msg);
void InitLog(bool enableLogging);

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

    config.name = ini["hook.name"];
    config.version = ini["hook.version"];
    config.toggleKey = ini["hook.toggleKey"];
    config.closeKey = ini["hook.closeKey"];

    if (ini.contains("hook.reloadKey")) {
        config.reloadKey = ini["hook.reloadKey"];
    }
    else {
        config.reloadKey = "F8";
    }

    config.pluginFolder = ini["hook.pluginFolder"];
    config.showOnStartup = ini.contains("hook.showOnStartup") ? (ini["hook.showOnStartup"] == "1") : true;

    sscanf_s(ini["hook.overlayColor"].c_str(), "%d,%d,%d,%d",
        &config.colorR, &config.colorG, &config.colorB, &config.colorA);

    if (ini.contains("hook.overlayPosition")) {
        config.overlayPosition = ini["hook.overlayPosition"];
    }
    else {
        config.overlayPosition = "bottom";
    }

    config.enableLogging = ini.contains("hook.enableLogging") ?
        (ini["hook.enableLogging"] == "1") : true;

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
    if (keyName == "F1") return VK_F1;
    if (keyName == "F2") return VK_F2;
    if (keyName == "F3") return VK_F3;
    if (keyName == "F4") return VK_F4;
    if (keyName == "F5") return VK_F5;
    if (keyName == "F6") return VK_F6;
    if (keyName == "F7") return VK_F7;
    if (keyName == "F8") return VK_F8;
    if (keyName == "F9") return VK_F9;
    if (keyName == "F10") return VK_F10;
    if (keyName == "F11") return VK_F11;
    if (keyName == "F12") return VK_F12;

    if (!keyName.empty()) {
        return toupper(keyName[0]);
    }
    return 0;
}

// --- Lua Script Execution ---
string ExecuteLuaScript(const string& scriptPath) {
    if (!L) return "Lua engine not initialized";

    int top = lua_gettop(L);

    lua_pushstring(L, "");
    lua_setglobal(L, "SCRIPT_RESULT");

    Log("Executing lua script: " + scriptPath);

    int result = luaL_dofile(L, scriptPath.c_str());
    string executionResult;

    if (result != LUA_OK) {
        executionResult = "Error: " + string(lua_tostring(L, -1));
        lua_pop(L, 1);
        Log("Failed to execute Lua: " + scriptPath + " - " + executionResult);
    }
    else {
        lua_getglobal(L, "SCRIPT_RESULT");
        if (lua_isstring(L, -1)) {
            executionResult = lua_tostring(L, -1);
            Log("Script executed successfully: " + scriptPath + " - Result: " + executionResult);
        }
        else {
            executionResult = "OK";
            Log("Script executed successfully: " + scriptPath);
        }
        lua_pop(L, 1);
    }

    lua_settop(L, top);
    return executionResult;
}

// --- Lua API Setup ---
void SetupLuaKeyboardAPI() {
    if (!L) return;

    // Keyboard API
    lua_newtable(L);

    lua_pushstring(L, "IsKeyDown");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        int keyCode = luaL_checkinteger(L, 1);
        bool isDown = (GetAsyncKeyState(keyCode) & 0x8000) != 0;
        lua_pushboolean(L, isDown);
        return 1;
        });
    lua_settable(L, -3);

    lua_pushstring(L, "IsKeyPressed");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        int keyCode = luaL_checkinteger(L, 1);
        bool isPressed = (GetAsyncKeyState(keyCode) & 1) != 0;
        lua_pushboolean(L, isPressed);
        return 1;
        });
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
    lua_pushcfunction(L, [](lua_State* L) -> int {
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
        });
    lua_settable(L, -3);

    lua_pushstring(L, "WriteMemory");
    lua_pushcfunction(L, [](lua_State* L) -> int {
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
        });
    lua_settable(L, -3);

    lua_pushstring(L, "GetModuleBase");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        const char* moduleName = luaL_checkstring(L, 1);
        HMODULE hModule = GetModuleHandleA(moduleName);
        if (hModule) {
            lua_pushinteger(L, (lua_Integer)hModule);
        }
        else {
            lua_pushnil(L);
        }
        return 1;
        });
    lua_settable(L, -3);

    lua_setglobal(L, "Memory");

    // Registers API
    lua_newtable(L);

    lua_pushstring(L, "Get");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        lua_newtable(L);

        lua_pushstring(L, "eax"); lua_pushinteger(L, currentRegisters.eax); lua_settable(L, -3);
        lua_pushstring(L, "ebx"); lua_pushinteger(L, currentRegisters.ebx); lua_settable(L, -3);
        lua_pushstring(L, "ecx"); lua_pushinteger(L, currentRegisters.ecx); lua_settable(L, -3);
        lua_pushstring(L, "edx"); lua_pushinteger(L, currentRegisters.edx); lua_settable(L, -3);
        lua_pushstring(L, "esi"); lua_pushinteger(L, currentRegisters.esi); lua_settable(L, -3);
        lua_pushstring(L, "edi"); lua_pushinteger(L, currentRegisters.edi); lua_settable(L, -3);
        lua_pushstring(L, "ebp"); lua_pushinteger(L, currentRegisters.ebp); lua_settable(L, -3);
        lua_pushstring(L, "esp"); lua_pushinteger(L, currentRegisters.esp); lua_settable(L, -3);

        return 1;
        });
    lua_settable(L, -3);

    lua_setglobal(L, "Registers");

    Log("Lua API setup complete");
}

// --- Plugin Management ---
void RebuildPluginsVector() {
    plugins.clear();
    for (const auto& [_, p] : loadedPlugins) {
        plugins.push_back(p);
    }
}

void LoadPluginsWithoutExecution() {
    std::map<std::string, Plugin> newPlugins;
    for (auto& entry : fs::directory_iterator(config.pluginFolder)) {
        if (entry.path().extension() != ".ini") continue;

        string base = entry.path().stem().string();
        string iniPath = entry.path().string();
        string luaPath = entry.path().parent_path().string() + "/" + base + ".lua";

        if (!fs::exists(luaPath)) continue;

        auto ini = ParseIni(iniPath);
        Plugin plugin;
        plugin.name = ini.contains("meta.name") ? ini["meta.name"] : "Unnamed";
        plugin.version = ini.contains("meta.version") ? ini["meta.version"] : "Unknown";
        plugin.author = ini.contains("meta.author") ? ini["meta.author"] : "Anonymous";
        plugin.statusInfo = ini["status.info"];
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
            plugin.executionResult = ExecuteLuaScript(plugin.luaPath);
            plugin.status = plugin.executionResult;

            auto it = loadedPlugins.begin();
            for (; it != loadedPlugins.end(); ++it) {
                if (it->second.luaPath == plugin.luaPath) {
                    it->second.executionResult = plugin.executionResult;
                    it->second.status = plugin.status;
                    break;
                }
            }
        }
        catch (const std::exception& e) {
            Log("EXCEPTION executing " + plugin.name + ": " + e.what());
            plugin.executionResult = "Error: " + std::string(e.what());
            plugin.status = plugin.executionResult;
        }
    }
    Log("All plugins executed");
}

void UpdateSinglePlugin(const std::string& baseName) {
    static std::map<std::string, Plugin> lastPluginState;

    std::string iniPath = config.pluginFolder + "/" + baseName + ".ini";
    std::string luaPath = config.pluginFolder + "/" + baseName + ".lua";

    // Проверяем существование файлов
    if (GetFileAttributesA(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(luaPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (loadedPlugins.find(baseName) != loadedPlugins.end()) {
            Log("Plugin removed: " + baseName);
            loadedPlugins.erase(baseName);
            lastPluginState.erase(baseName);
        }
        return;
    }

    auto ini = ParseIni(iniPath);
    Plugin plugin;
    plugin.name = ini.contains("meta.name") ? ini["meta.name"] : "Unnamed";
    plugin.version = ini.contains("meta.version") ? ini["meta.version"] : "Unknown";
    plugin.author = ini.contains("meta.author") ? ini["meta.author"] : "Anonymous";
    plugin.statusInfo = ini["status.info"];
    plugin.luaPath = luaPath;
    plugin.iniData = ini;

    // Проверяем, изменился ли плагин
    bool isChanged = true;
    if (lastPluginState.find(baseName) != lastPluginState.end()) {
        const auto& lastPlugin = lastPluginState[baseName];
        isChanged = (lastPlugin.name != plugin.name ||
            lastPlugin.version != plugin.version ||
            lastPlugin.author != plugin.author ||
            lastPlugin.statusInfo != plugin.statusInfo);
    }

    if (!isChanged) {
        return;
    }

    bool isNew = loadedPlugins.find(baseName) == loadedPlugins.end();

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
    WIN32_FIND_DATAA findData;
    std::string searchPath = directory + "\\*.*";
    HANDLE hFind = INVALID_HANDLE_VALUE;
    std::map<std::string, FILETIME> lastWriteTimes;
    bool firstRun = true;

    while (!stopMonitoring) {
        hFind = FindFirstFileA(searchPath.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }

        bool changes = false;
        do {
            std::string fileName = findData.cFileName;
            if (fileName == "." || fileName == "..") continue;

            if (fileName.ends_with(".lua") || fileName.ends_with(".ini")) {
                std::string baseName = fileName.substr(0, fileName.find_last_of('.'));

                if (lastWriteTimes.find(fileName) == lastWriteTimes.end()) {
                    if (!firstRun) {
                        changes = true;
                        UpdateSinglePlugin(baseName);
                    }
                    lastWriteTimes[fileName] = findData.ftLastWriteTime;
                }
                else if (CompareFileTime(&lastWriteTimes[fileName], &findData.ftLastWriteTime) != 0) {
                    changes = true;
                    lastWriteTimes[fileName] = findData.ftLastWriteTime;
                    UpdateSinglePlugin(baseName);
                }
            }
        } while (FindNextFileA(hFind, &findData));

        FindClose(hFind);

        // Проверяем удаленные файлы
        auto it = lastWriteTimes.begin();
        while (it != lastWriteTimes.end()) {
            std::string fileName = it->first;
            std::string fullPath = directory + "\\" + fileName;

            if (GetFileAttributesA(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::string baseName = fileName.substr(0, fileName.find_last_of('.'));
                UpdateSinglePlugin(baseName);
                it = lastWriteTimes.erase(it);
                changes = true;
            }
            else {
                ++it;
            }
        }

        if (changes && !firstRun) {
            RebuildPluginsVector();
        }

        firstRun = false;
        Sleep(100);
    }
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
                mainRenderTargetView->Release();
                mainRenderTargetView = nullptr;
            }
        }
        break;
    }
    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

// --- Render Functions ---
void RenderOverlay() {
    if (plugins.empty()) return;
    auto& p = plugins[currentPlugin];
    ImGuiIO& io = ImGui::GetIO();

    // Двухпроходный рендеринг для определения точной высоты
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(config.colorR / 255.f, config.colorG / 255.f, config.colorB / 255.f, config.colorA / 100.f));

    // Первый проход - используем невидимое окно для расчета высоты
    ImGui::SetNextWindowPos(ImVec2(-9999, -9999)); // Размещаем за пределами экрана
    ImGui::Begin("HeightCalc", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);

    // Устанавливаем фиксированную ширину, равную ширине экрана
    ImGui::SetWindowSize(ImVec2(io.DisplaySize.x, -1)); // Ширина = экрану, высота = автоматически

    // Отображение содержимого
    ImGui::Text("%s v%s | %s", config.name.c_str(), config.version.c_str(), p.luaPath.c_str());
    ImGui::Separator();
    ImGui::Text("%s | v. %s", p.name.c_str(), p.version.c_str());
    ImGui::Text("by %s", p.author.c_str());
    ImGui::Spacing();
    ImGui::Text("Info: %s", p.statusInfo.c_str());
    ImGui::Text("Plugin Status: %s", p.executionResult.c_str());

    // Получаем точный размер содержимого
    float contentHeight = ImGui::GetWindowHeight();
    ImGui::End();

    // Второй проход - рисуем настоящее окно с правильной высотой
    ImVec2 windowPos;
    if (config.overlayPosition == "top") {
        windowPos = ImVec2(0, 0); // Сверху экрана
    }
    else {
        windowPos = ImVec2(0, io.DisplaySize.y - contentHeight); // Снизу экрана (по умолчанию)
    }

    ImGui::SetNextWindowPos(windowPos);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, contentHeight));
    ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

    // Повторяем содержимое для реального отображения
    ImGui::Text("%s v%s | %s", config.name.c_str(), config.version.c_str(), p.luaPath.c_str());
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
                    if (newResult != plugin.executionResult) {
                        plugin.executionResult = newResult;
                        for (auto& [key, p] : loadedPlugins) {
                            if (p.luaPath == plugin.luaPath) {
                                p.executionResult = newResult;
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
        if (SUCCEEDED(pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&device))) {
            device->GetImmediateContext(&context);
            DXGI_SWAP_CHAIN_DESC sd;
            pSwap->GetDesc(&sd);
            hwnd = sd.OutputWindow;

            currentWidth = sd.BufferDesc.Width;
            currentHeight = sd.BufferDesc.Height;

            ID3D11Texture2D* backBuffer = nullptr;
            pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
            device->CreateRenderTargetView(backBuffer, nullptr, &mainRenderTargetView);
            backBuffer->Release();

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

            ImGui_ImplWin32_Init(hwnd);
            ImGui_ImplDX11_Init(device, context);

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
        // Сохраняем значения регистров
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

        if (!mainRenderTargetView) {
            Log("Recreating render target view");
            ID3D11Texture2D* backBuffer = nullptr;
            HRESULT hr = pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
            if (SUCCEEDED(hr) && backBuffer) {
                device->CreateRenderTargetView(backBuffer, nullptr, &mainRenderTargetView);
                backBuffer->Release();

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
        else {
            currentPlugin = (currentPlugin + 1) % max(1, (int)plugins.size());
            Log("Plugin switched to: " + plugins[currentPlugin].name);
        }
    }

    if (GetAsyncKeyState(reloadKey) & 1 && overlayVisible && !plugins.empty()) {
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
        context->OMSetRenderTargets(1, &mainRenderTargetView, nullptr);
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

    IDXGISwapChain* tempSwap = nullptr;
    ID3D11Device* tempDevice = nullptr;
    ID3D11DeviceContext* tempContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scDesc,
        &tempSwap, &tempDevice, &featureLevel, &tempContext
    );

    if (SUCCEEDED(hr)) {
        void** vTable = *reinterpret_cast<void***>(tempSwap);

        MH_Initialize();
        MH_CreateHook(vTable[8], &hkPresent, reinterpret_cast<void**>(&oPresent));
        MH_EnableHook(vTable[8]);

        tempSwap->Release();
        tempDevice->Release();
        tempContext->Release();

        Log("Present hook installed successfully");
    }
    else {
        Log("Failed to create dummy swap chain");
    }
}

// --- Main Thread ---
DWORD WINAPI MainThread(LPVOID) {
    Log("Main thread started");

    // Initialize Lua
    L = luaL_newstate();
    if (!L) {
        Log("CRITICAL: Failed to initialize Lua!");
        return 1;
    }
    luaL_openlibs(L);
    SetupLuaKeyboardAPI();
    Log("Lua engine initialized successfully");

    // Load plugins without execution
    LoadPluginsWithoutExecution();

    // Initialize DirectX hook
    InitHook();

    // Execute all plugins after DirectX initialization
    ExecuteAllPlugins();
    Log("All plugins executed automatically on startup");

    // Start monitoring the plugins directory
    std::thread monitorThread(MonitorDirectoryChanges, config.pluginFolder);
    monitorThread.detach(); // Детачим поток, чтобы он работал независимо

    return 0;
}

// --- DllMain ---
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        LoadConfig();
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        stopMonitoring = true;

        if (L) {
            lua_close(L);
            L = nullptr;
        }

        if (mainRenderTargetView) {
            mainRenderTargetView->Release();
            mainRenderTargetView = nullptr;
        }

        if (context) {
            context->Release();
            context = nullptr;
        }

        if (device) {
            device->Release();
            device = nullptr;
        }

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();

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