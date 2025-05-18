#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <thread>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <vector>
#include <map>
#include <string>
#include <sstream>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "libMinHook.x86.lib")

bool showOverlay = false;
bool initialized = false;
bool hookInitialized = false;

HWND gameHWND = nullptr;
WNDPROC oWndProc = nullptr;
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;

void Log(const std::string& msg) {
    static bool firstLog = true;
    FILE* file = nullptr;

    if (firstLog) {
        fopen_s(&file, "dinput8_log.txt", "w");
        firstLog = false;
    }
    else {
        fopen_s(&file, "dinput8_log.txt", "a");
    }

    if (file) {
        fprintf(file, "%s\n", msg.c_str());
        fclose(file);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (showOverlay && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

void InitImGui(HWND hwnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
}

void RenderOverlay() {
    if (!showOverlay) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Plugin Overlay");
    ImGui::Text("Overlay Active");
    ImGui::End();
    ImGui::Render();
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void CreateRenderTarget(IDXGISwapChain* pSwapChain) {
    ID3D11Texture2D* pBackBuffer;
    pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!initialized) {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice))) {
            g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
            DXGI_SWAP_CHAIN_DESC sd;
            pSwapChain->GetDesc(&sd);
            gameHWND = sd.OutputWindow;
            CreateRenderTarget(pSwapChain);
            InitImGui(gameHWND);
            oWndProc = (WNDPROC)SetWindowLongPtr(gameHWND, GWLP_WNDPROC, (LONG_PTR)WndProc);
            initialized = true;
        }
    }
    RenderOverlay();
    return oPresent(pSwapChain, SyncInterval, Flags);
}

void InitHookOnce() {
    if (hookInitialized) return;
    hookInitialized = true;
    Log("Initializing delayed Present hook...");

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    scDesc.BufferCount = 1;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow = GetForegroundWindow();
    scDesc.SampleDesc.Count = 1;
    scDesc.Windowed = TRUE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scDesc, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext))) {

        void** pVTable = *reinterpret_cast<void***>(g_pSwapChain);
        if (MH_Initialize() == MH_OK && MH_CreateHook(pVTable[8], &hkPresent, reinterpret_cast<void**>(&oPresent)) == MH_OK && MH_EnableHook(pVTable[8]) == MH_OK) {
            Log("Present hook installed.");
        }
        else {
            Log("Failed to hook Present (delayed)");
        }
    }
}

DWORD WINAPI MainThread(LPVOID) {
    Log("--- New Session ---");
    InitHookOnce();

    while (true) {
        if (GetAsyncKeyState(VK_F9) & 1) {
            showOverlay = !showOverlay;
            Log(showOverlay ? "Overlay shown" : "Overlay hidden");
        }
        Sleep(100);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riid, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    Log("DirectInput8Create called");
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\dinput8.dll");
    HMODULE realDInput = LoadLibraryA(sysPath);
    if (!realDInput) return E_FAIL;

    using DInputCreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    DInputCreateFn original = (DInputCreateFn)GetProcAddress(realDInput, "DirectInput8Create");
    if (!original) return E_FAIL;
    return original(hinst, dwVersion, riid, ppvOut, punkOuter);
}