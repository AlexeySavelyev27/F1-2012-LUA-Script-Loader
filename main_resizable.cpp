// F1 2012 LUA Loader — main.cpp
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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "libMinHook.x86.lib")
#pragma comment(lib, "lua5.1.lib")

using namespace std;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// --- Logging ---
ofstream logFile;
void InitLog() {
    logFile.open("dinput8_log.txt", ios::out);
    logFile << "[LOG] --- New Session ---" << endl;
}
void Log(const string& msg) {
    if (logFile.is_open()) {
        time_t now = time(nullptr);
        logFile << "[" << now << "] " << msg << endl;
        logFile.flush();
    }
}

// --- Config & Plugin Structures ---
struct HookConfig {
    string name = "F1 2012 LUA Loader";
    string version = "1.0";
    string toggleKey = "F9";
    string closeKey = "F10";
    string pluginFolder = "plugins";
    bool showOnStartup = true;
    int colorR = 0, colorG = 64, colorB = 0;
};

struct Plugin {
    string name, version, author;
    string statusInfo, status;
    string luaPath;
    map<string, string> iniData;
};

// --- Globals ---
UINT lastWidth = 0, lastHeight = 0;
HookConfig config;
vector<Plugin> plugins;
int currentPlugin = 0;
bool overlayVisible = false;
bool initialized = false;

HWND hwnd = nullptr;
WNDPROC oWndProc = nullptr;
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
ID3D11RenderTargetView* mainRenderTargetView = nullptr;
lua_State* L = nullptr;
Clock::time_point lastPluginCheck = Clock::now();

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

// --- Config / Plugin Load ---
void LoadConfig() {
    auto ini = ParseIni("dinput8_config.ini");
    config.name = ini["hook.name"];
    config.version = ini["hook.version"];
    config.toggleKey = ini["hook.toggleKey"];
    config.closeKey = ini["hook.closeKey"];
    config.pluginFolder = ini["hook.pluginFolder"];
    config.showOnStartup = ini["hook.defaultOverlay"] == "true";
    sscanf_s(ini["hook.overlayColor"].c_str(), "%d,%d,%d", &config.colorR, &config.colorG, &config.colorB);
    Log("Loaded config from dinput8_config.ini");
}

void ReloadPlugins() {
    static map<string, Plugin> loaded;
    map<string, Plugin> newPlugins;

    for (auto& entry : fs::directory_iterator(config.pluginFolder)) {
        if (entry.path().extension() != ".ini") continue;
        string base = entry.path().stem().string();
        string iniPath = entry.path().string();
        string luaPath = entry.path().parent_path().string() + "/" + base + ".lua";

        if (!fs::exists(luaPath)) continue;

        auto ini = ParseIni(iniPath);
        Plugin plugin;
        plugin.name = ini["meta.name"];
        plugin.version = ini["meta.version"];
        plugin.author = ini["meta.author"];
        plugin.statusInfo = ini["status.info"];
        plugin.status = ini["status.pluginStatus"];
        plugin.luaPath = luaPath;
        plugin.iniData = ini;

        newPlugins[base] = plugin;

        if (loaded.find(base) == loaded.end()) {
            Log("New plugin detected: " + base);
        }
    }

    // Replace global list
    plugins.clear();
    for (auto& [_, p] : newPlugins)
        plugins.push_back(p);

    loaded = newPlugins;
}

// --- Lua ---
void RunCurrentLua() {
    if (!L || plugins.empty()) return;
    luaL_dofile(L, plugins[currentPlugin].luaPath.c_str());
    Log("Executed Lua: " + plugins[currentPlugin].luaPath);
}

// --- ImGui ---
void RenderOverlay() {
    if (plugins.empty()) return;
    auto& p = plugins[currentPlugin];
    ImGuiIO& io = ImGui::GetIO();
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    int lines = 6 + (int)p.iniData.size();
    float height = lines * lineH;

    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - height));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(config.colorR / 255.f, config.colorG / 255.f, config.colorB / 255.f, 1.0f));
    ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::Text("%s v%s | %s", config.name.c_str(), config.version.c_str(), p.luaPath.c_str());
    ImGui::Separator();
    ImGui::Text("%s | v. %s", p.name.c_str(), p.version.c_str());
    ImGui::Text("by %s", p.author.c_str());
    ImGui::Spacing();
    for (auto& kv : p.iniData) {
        ImGui::Text("%s: %s", kv.first.c_str(), kv.second.c_str());
    }
    ImGui::Text("Plugin Status: %s", p.status.c_str());
    ImGui::End();
    ImGui::PopStyleColor();
}
// --- WndProc ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (overlayVisible && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

// --- Present Hook ---
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
PresentFn oPresent = nullptr;

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwap, UINT SyncInterval, UINT Flags) {
    if (!initialized) {
        if (SUCCEEDED(pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&device))) {
            device->GetImmediateContext(&context);
            DXGI_SWAP_CHAIN_DESC sd;
            pSwap->GetDesc(&sd);
            hwnd = sd.OutputWindow;

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

    // --- Resize check ---
    DXGI_SWAP_CHAIN_DESC currentDesc;
    pSwap->GetDesc(&currentDesc);

    if (currentDesc.BufferDesc.Width != lastWidth || currentDesc.BufferDesc.Height != lastHeight) {
        if (mainRenderTargetView) {
            mainRenderTargetView->Release();
            mainRenderTargetView = nullptr;
        }

        ID3D11Texture2D* pBackBuffer = nullptr;
        pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
        device->CreateRenderTargetView(pBackBuffer, nullptr, &mainRenderTargetView);
        pBackBuffer->Release();

        lastWidth = currentDesc.BufferDesc.Width;
        lastHeight = currentDesc.BufferDesc.Height;

        Log("Window resized, render target recreated");
    }

        }
    }

    // Auto plugin refresh every 1 sec
    if (Clock::now() - lastPluginCheck >= chrono::seconds(1)) {
        ReloadPlugins();
        lastPluginCheck = Clock::now();
    }

    // Input
    if (GetAsyncKeyState(VK_F9) & 1) {
        if (!overlayVisible) {
            overlayVisible = true;
            Log("Overlay shown (F9)");
        } else {
            currentPlugin = (currentPlugin + 1) % max(1, (int)plugins.size());
            RunCurrentLua();
            Log("Plugin switched (F9)");
        }
    }
    if (GetAsyncKeyState(VK_F10) & 1) {
        overlayVisible = false;
        Log("Overlay hidden (F10)");
    }

    // Render
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (overlayVisible) RenderOverlay();
    ImGui::Render();
    context->OMSetRenderTargets(1, &mainRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    return oPresent(pSwap, SyncInterval, Flags);
}

// --- Init Hook ---
void InitHook() {
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

    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scDesc, &tempSwap, &tempDevice, &featureLevel, &tempContext))) {

        void** vTable = *reinterpret_cast<void***>(tempSwap);
        MH_Initialize();
        MH_CreateHook(vTable[8], &hkPresent, reinterpret_cast<void**>(&oPresent));
        MH_EnableHook(vTable[8]);

        tempSwap->Release();
        tempDevice->Release();
        tempContext->Release();

        Log("Present hook installed");
    } else {
        Log("Failed to create dummy swap chain");
    }
}

// --- Main Thread ---
DWORD WINAPI MainThread(LPVOID) {
    InitLog();
    LoadConfig();
    ReloadPlugins();

    L = luaL_newstate();
    luaL_openlibs(L);
    if (!plugins.empty()) RunCurrentLua();

    InitHook();
    return 0;
}

// --- DllMain ---
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}

// --- DirectInput8Create Export ---
extern "C" __declspec(dllexport)
HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riid, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    Log("DirectInput8Create called");
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\dinput8.dll");

    HMODULE realDInput = LoadLibraryA(sysPath);
    if (!realDInput) return E_FAIL;

    using DInputCreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto original = (DInputCreateFn)GetProcAddress(realDInput, "DirectInput8Create");
    if (!original) return E_FAIL;

    return original(hinst, dwVersion, riid, ppvOut, punkOuter);
}
