#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <winrt/Windows.Foundation.h>
#include <vector>
#include <string>
#include <mutex>
#include <algorithm>
#include <deque>

// Dear ImGui
#include "../external/imgui/imgui.h"
#include "../external/imgui/backends/imgui_impl_win32.h"
#include "../external/imgui/backends/imgui_impl_dx11.h"

// Project headers
#include "warp.h"
#include "capture.h"
#include "config.h"

// Link libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")

// ImGui handler forward declaration
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Monitor enumeration structures
struct MonitorInfo {
    int index;
    RECT rect;
    std::wstring name;
    bool isPrimary;
};

// Global State
AppConfig g_Config;
std::string g_ConfigPath = "config.txt";

std::vector<MonitorInfo> g_Monitors;
std::vector<WindowInfo> g_Windows;

HWND g_ControlPanelHwnd = nullptr;
HWND g_ProjectorHwnd = nullptr;

winrt::com_ptr<ID3D11Device> g_D3DDevice = nullptr;
winrt::com_ptr<ID3D11DeviceContext> g_D3DContext = nullptr;
winrt::com_ptr<IDXGISwapChain> g_ControlPanelSwapChain = nullptr;
winrt::com_ptr<IDXGISwapChain> g_ProjectorSwapChain = nullptr;
winrt::com_ptr<ID3D11RenderTargetView> g_ControlPanelRTV = nullptr;
winrt::com_ptr<ID3D11RenderTargetView> g_ProjectorRTV = nullptr;
HANDLE g_ProjectorWaitableHandle = nullptr; // Waitable swap chain handle for low-latency present

winrt::com_ptr<ID3D11VertexShader> g_VertexShader = nullptr;
winrt::com_ptr<ID3D11PixelShader> g_PixelShader = nullptr;
winrt::com_ptr<ID3D11Buffer> g_ConstantBuffer = nullptr;
winrt::com_ptr<ID3D11SamplerState> g_SamplerState = nullptr;

std::unique_ptr<WindowCapture> g_Capture = nullptr;
winrt::com_ptr<ID3D11ShaderResourceView> g_CapturedSRV = nullptr;
unsigned int g_CaptureWidth = 0;
unsigned int g_CaptureHeight = 0;

// Warping state variables
HomographyMatrix g_Homography;
bool g_WarpValid = false;
int g_DraggedCorner = -1;
int g_SelectedCorner = -1; // For fine-tuning with arrow keys
bool g_ShowGuides = true;

// Minimized/hidden-window handling state using transparency & click-through (WS_EX_LAYERED)
struct SavedWindowPlacement {
    HWND hwnd = nullptr;
    WINDOWPLACEMENT placement = { sizeof(WINDOWPLACEMENT) };
    bool wasOffscreen = false; // Keep the same field name to minimize references changes
    LONG originalExStyle = 0;
    COLORREF originalColorKey = 0;
    BYTE originalAlpha = 255;
    DWORD originalFlags = 0;
    bool hadLayeredStyle = false;
} g_SavedPlacement;

// Make the window fully transparent (alpha=1) and click-through (WS_EX_TRANSPARENT),
// keeping it in its screen location (or restoring it if minimized) so it continues to render at full speed.
void MoveWindowOffscreen(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (g_SavedPlacement.wasOffscreen && g_SavedPlacement.hwnd == hwnd) return; // already done

    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    GetWindowPlacement(hwnd, &wp);
    g_SavedPlacement.placement = wp; // save original placement
    g_SavedPlacement.hwnd      = hwnd;
    g_SavedPlacement.wasOffscreen = true;

    // Save original extended styles
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    g_SavedPlacement.originalExStyle = exStyle;
    g_SavedPlacement.hadLayeredStyle = (exStyle & WS_EX_LAYERED) != 0;
    
    if (g_SavedPlacement.hadLayeredStyle) {
        GetLayeredWindowAttributes(hwnd, &g_SavedPlacement.originalColorKey, 
                                   &g_SavedPlacement.originalAlpha, &g_SavedPlacement.originalFlags);
    }

    // Set as Layered and Transparent (click-through)
    SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    
    // Set opacity to 0 (completely invisible, 0/255)
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);

    // If it was minimized, restore it quietly without stealing focus
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT wpRestore = wp;
        wpRestore.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(hwnd, &wpRestore);
    }

    // Make the window Topmost so DWM keeps rendering it (never occluded by other windows),
    // but without activating it.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);

    // Ensure our Control Panel remains the active foreground window
    if (g_ControlPanelHwnd) {
        SetForegroundWindow(g_ControlPanelHwnd);
    }
}

// Restore the window's original opacity, style and positioning
void RestoreSavedWindow() {
    if (!g_SavedPlacement.wasOffscreen) return;
    if (g_SavedPlacement.hwnd && IsWindow(g_SavedPlacement.hwnd)) {
        // Restore window styles
        SetWindowLongW(g_SavedPlacement.hwnd, GWL_EXSTYLE, g_SavedPlacement.originalExStyle);
        
        if (g_SavedPlacement.hadLayeredStyle) {
            SetLayeredWindowAttributes(g_SavedPlacement.hwnd, g_SavedPlacement.originalColorKey, 
                                       g_SavedPlacement.originalAlpha, g_SavedPlacement.originalFlags);
        } else {
            // Ask window to redraw to clear the layered frame cache
            RedrawWindow(g_SavedPlacement.hwnd, nullptr, nullptr, 
                          RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }

        // Restore Z-order (remove topmost)
        SetWindowPos(g_SavedPlacement.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);

        // Restore placement (if it was minimized, minimize it again, etc.)
        SetWindowPlacement(g_SavedPlacement.hwnd, &g_SavedPlacement.placement);
    }
    g_SavedPlacement.hwnd = nullptr;
    g_SavedPlacement.wasOffscreen = false;
}

// Performance / Debug Stats
struct PerfStats {
    LARGE_INTEGER freq = {};
    LARGE_INTEGER frameStart = {};
    LARGE_INTEGER frameEnd = {};
    double frameTimeMs = 0.0;      // last frame total time (ms)
    double projRenderMs = 0.0;     // projector render + present time (ms)
    double captureLatencyMs = 0.0; // time between new frame arriving and SRV update
    float  fps = 0.0f;
    int    droppedFrames = 0;
    bool   hadNewFrameThisTick = false;
    std::deque<float> fpsHistory;  // rolling 120-sample history for sparkline
    LARGE_INTEGER captureArrived = {};
    bool showDebug = false;
} g_Perf;

// Embedded Shader Source Code (Vertex + Pixel Shader)
const char* g_ShaderSource = R"(
cbuffer ConstantBuffer : register(b0)
{
    float4 HRow0;       // Homography row 0 (xyz)
    float4 HRow1;       // Homography row 1 (xyz)
    float4 HRow2;       // Homography row 2 (xyz)
    float4 GridColor;   // (r, g, b, a)
    float4 GridSettings;// x: ShowGrid (1 or 0), y: GridCols, z: GridRows, w: BorderThickness (pixels)
    
    // Calibration guides (rendered in pixels)
    float4 Corner0;     // xy: corner0 pixel pos
    float4 Corner1;     // xy: corner1 pixel pos
    float4 Corner2;     // xy: corner2 pixel pos
    float4 Corner3;     // xy: corner3 pixel pos
    float4 GuideSettings; // x: ShowGuides (1 or 0), y: SelectedCorner (0-3, or -1), z: HandleRadius (pixels), w: LineThickness (pixels)
    float4 GuideColor;  // Color for lines/unselected handles (r, g, b, a)
    float4 ActiveGuideColor; // Color for selected handle (r, g, b, a)
    float4 CropSettings; // x: CropLeft, y: CropTop, z: CropRight, w: CropBottom
};

Texture2D shaderTexture : register(t0);
SamplerState sampleState : register(s0);

struct VS_OUTPUT
{
    float4 Pos : SV_POSITION;
};

// Generate full-screen quad from Vertex ID
VS_OUTPUT VS(uint VertexID : SV_VertexID)
{
    VS_OUTPUT output;
    float2 grid[4] = {
        float2(-1.0f,  1.0f), // TL
        float2( 1.0f,  1.0f), // TR
        float2(-1.0f, -1.0f), // BL
        float2( 1.0f, -1.0f)  // BR
    };
    output.Pos = float4(grid[VertexID], 0.0f, 1.0f);
    return output;
}

// Distance from point p to line segment ab
float DistToSegment(float2 p, float2 a, float2 b)
{
    float2 ap = p - a;
    float2 ab = b - a;
    float t = clamp(dot(ap, ab) / dot(ab, ab), 0.0f, 1.0f);
    return distance(p, a + t * ab);
}

float4 PS(VS_OUTPUT input) : SV_TARGET
{
    float2 pixelPos = input.Pos.xy;
    float3 screenPos = float3(pixelPos, 1.0f);
    
    // Apply 3x3 homography matrix to map projector pixels -> texture UVs
    float u = dot(HRow0.xyz, screenPos);
    float v = dot(HRow1.xyz, screenPos);
    float w = dot(HRow2.xyz, screenPos);
    float2 uv = float2(u, v) / w;
    
    float4 finalColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
    
    // Evaluate insideWarp using original UVs (bounds the rendering strictly to the warp quad)
    bool insideWarp = (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
    
    // Sample texture if inside warp bounds
    if (insideWarp)
    {
        // Apply texture cropping inside the warp (remaps 0-1 range to the cropped subsection)
        float2 croppedUV;
        croppedUV.x = CropSettings.x + uv.x * (1.0f - CropSettings.x - CropSettings.z);
        croppedUV.y = CropSettings.y + uv.y * (1.0f - CropSettings.y - CropSettings.w);
        
        finalColor = shaderTexture.Sample(sampleState, croppedUV);
        
        // Render calibration grid if enabled
        if (GridSettings.x > 0.5f)
        {
            float cols = GridSettings.y;
            float rows = GridSettings.z;
            float thickness = GridSettings.w;
            
            float2 cellUV = uv * float2(cols, rows);
            float2 distToGrid = abs(frac(cellUV + 0.5f) - 0.5f) / float2(cols, rows);
            
            float minDistX = min(uv.x, 1.0f - uv.x);
            float minDistY = min(uv.y, 1.0f - uv.y);
            float borderDistX = min(distToGrid.x, minDistX);
            float borderDistY = min(distToGrid.y, minDistY);
            
            float2 uvDeriv = fwidth(uv);
            float2 threshold = uvDeriv * thickness;
            
            if (borderDistX < threshold.x || borderDistY < threshold.y)
            {
                finalColor = lerp(finalColor, float4(GridColor.rgb, 1.0f), GridColor.a);
            }
        }
    }
    
    // Render Calibration Guides Overlay
    if (GuideSettings.x > 0.5f)
    {
        float radius = GuideSettings.z;
        float lineThickness = GuideSettings.w;
        int selectedIndex = (int)(GuideSettings.y + 0.5f);
        
        float d0 = distance(pixelPos, Corner0.xy);
        float d1 = distance(pixelPos, Corner1.xy);
        float d2 = distance(pixelPos, Corner2.xy);
        float d3 = distance(pixelPos, Corner3.xy);
        
        float dLine = min(
            min(DistToSegment(pixelPos, Corner0.xy, Corner1.xy), DistToSegment(pixelPos, Corner1.xy, Corner2.xy)),
            min(DistToSegment(pixelPos, Corner2.xy, Corner3.xy), DistToSegment(pixelPos, Corner3.xy, Corner0.xy))
        );
        
        // Render lines
        if (dLine < lineThickness)
        {
            float alpha = clamp((lineThickness - dLine), 0.0f, 1.0f) * GuideColor.a;
            finalColor = lerp(finalColor, float4(GuideColor.rgb, 1.0f), alpha);
        }
        
        // Render circles
        float dCircle = min(min(d0, d1), min(d2, d3));
        if (dCircle < radius)
        {
            bool isSelected = false;
            if (selectedIndex == 0 && d0 < radius) isSelected = true;
            if (selectedIndex == 1 && d1 < radius) isSelected = true;
            if (selectedIndex == 2 && d2 < radius) isSelected = true;
            if (selectedIndex == 3 && d3 < radius) isSelected = true;
            
            float4 color = isSelected ? ActiveGuideColor : GuideColor;
            
            if (dCircle < radius - 2.0f)
            {
                finalColor = lerp(finalColor, float4(color.rgb, 1.0f), color.a);
            }
            else
            {
                finalColor = lerp(finalColor, float4(0.0f, 0.0f, 0.0f, 1.0f), 0.8f); // outline
            }
        }
    }
    
    return finalColor;
}
)";

// Constant Buffer Layout Matching Shader
struct ShaderConstBuffer {
    float HRow0[4];
    float HRow1[4];
    float HRow2[4];
    float GridColor[4];
    float GridSettings[4]; // Show, Cols, Rows, Thickness
    float Corner0[4];
    float Corner1[4];
    float Corner2[4];
    float Corner3[4];
    float GuideSettings[4]; // Show, SelectedIndex, Radius, LineThickness
    float GuideColor[4];
    float ActiveGuideColor[4];
    float CropSettings[4]; // Left, Top, Right, Bottom
};

// Function declarations
void RefreshDisplays();
void RefreshWindowsList();
bool InitD3D();
void CleanupD3D();
void CreateControlPanelRTV();
void CleanupControlPanelRTV();
void OpenProjectorWindow();
void CloseProjectorWindow();
void UpdateWarp();
void ResetWarpCorners();

// Monitor enumeration callback
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    auto list = reinterpret_cast<std::vector<MonitorInfo>*>(dwData);
    MONITORINFOEXW info = { sizeof(info) };
    if (GetMonitorInfoW(hMonitor, &info)) {
        MonitorInfo m;
        m.index = static_cast<int>(list->size());
        m.rect = info.rcMonitor;
        m.name = info.szDevice;
        m.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        list->push_back(m);
    }
    return TRUE;
}

void RefreshDisplays() {
    g_Monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&g_Monitors));
}

void RefreshWindowsList() {
    g_Windows = WindowCapture::EnumerateWindows(g_ControlPanelHwnd, g_ProjectorHwnd);
}

void UpdateWarp() {
    Point2D textureCorners[4] = {
        { 0.0f, 0.0f }, // TL
        { 1.0f, 0.0f }, // TR
        { 1.0f, 1.0f }, // BR
        { 0.0f, 1.0f }  // BL
    };
    g_WarpValid = ComputeHomography(g_Config.warp.corners, textureCorners, g_Homography);
}

void ResetWarpCorners() {
    int w = 1920;
    int h = 1080;
    
    if (g_ProjectorHwnd) {
        RECT r;
        GetClientRect(g_ProjectorHwnd, &r);
        w = r.right - r.left;
        h = r.bottom - r.top;
    } else if (g_Config.monitorIndex >= 0 && g_Config.monitorIndex < g_Monitors.size()) {
        const auto& m = g_Monitors[g_Config.monitorIndex];
        w = m.rect.right - m.rect.left;
        h = m.rect.bottom - m.rect.top;
    }
    
    // Inset corners by 10%
    float insetX = w * 0.1f;
    float insetY = h * 0.1f;
    
    g_Config.warp.corners[0] = { insetX, insetY };           // TL
    g_Config.warp.corners[1] = { w - insetX, insetY };       // TR
    g_Config.warp.corners[2] = { w - insetX, h - insetY };   // BR
    g_Config.warp.corners[3] = { insetX, h - insetY };       // BL
    
    UpdateWarp();
}

// Control Panel Window Procedure
LRESULT WINAPI WndProcControlPanel(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (g_D3DDevice != nullptr && g_ControlPanelSwapChain != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupControlPanelRTV();
                g_ControlPanelSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateControlPanelRTV();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU) // Disable Alt key focusing menu bar
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Projector Window Procedure
LRESULT WINAPI WndProcProjector(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (!g_ShowGuides) break;
            int x = (int)LOWORD(lParam);
            int y = (int)HIWORD(lParam);
            
            float radius = 15.0f; // Click tolerance
            int closest = -1;
            float minDist = radius;
            
            for (int i = 0; i < 4; ++i) {
                float dist = std::hypot(x - g_Config.warp.corners[i].x, y - g_Config.warp.corners[i].y);
                if (dist < minDist) {
                    minDist = dist;
                    closest = i;
                }
            }
            
            if (closest != -1) {
                g_DraggedCorner = closest;
                g_SelectedCorner = closest;
                SetCapture(hWnd);
            } else {
                g_SelectedCorner = -1;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_DraggedCorner != -1 && (wParam & MK_LBUTTON)) {
                int x = (int)LOWORD(lParam);
                int y = (int)HIWORD(lParam);
                
                g_Config.warp.corners[g_DraggedCorner] = { (float)x, (float)y };
                UpdateWarp();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_DraggedCorner != -1) {
                g_DraggedCorner = -1;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (g_SelectedCorner != -1) {
                float dx = 0.0f;
                float dy = 0.0f;
                float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10.0f : 1.0f;
                
                if (wParam == VK_LEFT) dx = -step;
                else if (wParam == VK_RIGHT) dx = step;
                else if (wParam == VK_UP) dy = -step;
                else if (wParam == VK_DOWN) dy = step;
                
                if (dx != 0.0f || dy != 0.0f) {
                    g_Config.warp.corners[g_SelectedCorner].x += dx;
                    g_Config.warp.corners[g_SelectedCorner].y += dy;
                    UpdateWarp();
                    return 0;
                }
            }
            break;
        }
        case WM_SIZE: {
            if (g_D3DDevice != nullptr && g_ProjectorSwapChain != nullptr && wParam != SIZE_MINIMIZED) {
                // Resize back buffer
                if (g_ProjectorRTV) { g_ProjectorRTV = nullptr; }
                g_ProjectorSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                
                winrt::com_ptr<ID3D11Texture2D> backBuffer;
                g_ProjectorSwapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
                g_D3DDevice->CreateRenderTargetView(backBuffer.get(), nullptr, g_ProjectorRTV.put());
            }
            return 0;
        }
        case WM_CLOSE:
            CloseProjectorWindow();
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool InitD3D() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_ControlPanelHwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, 
        D3D_DRIVER_TYPE_HARDWARE, 
        nullptr, 
        createDeviceFlags, 
        featureLevelArray, 2,
        D3D11_SDK_VERSION, 
        &sd, 
        g_ControlPanelSwapChain.put(), 
        g_D3DDevice.put(), 
        &featureLevel, 
        g_D3DContext.put()
    );
    
    if (FAILED(hr)) return false;

    CreateControlPanelRTV();

    // Compile Shaders
    winrt::com_ptr<ID3DBlob> vsBlob;
    winrt::com_ptr<ID3DBlob> psBlob;
    winrt::com_ptr<ID3DBlob> errorBlob;

    hr = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, vsBlob.put(), errorBlob.put());
    if (FAILED(hr)) {
        if (errorBlob) MessageBoxA(nullptr, (char*)errorBlob->GetBufferPointer(), "Shader VS Error", MB_OK | MB_ICONERROR);
        return false;
    }

    hr = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, psBlob.put(), errorBlob.put());
    if (FAILED(hr)) {
        if (errorBlob) MessageBoxA(nullptr, (char*)errorBlob->GetBufferPointer(), "Shader PS Error", MB_OK | MB_ICONERROR);
        return false;
    }

    g_D3DDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, g_VertexShader.put());
    g_D3DDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, g_PixelShader.put());

    // Create Constant Buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.ByteWidth = sizeof(ShaderConstBuffer);
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_D3DDevice->CreateBuffer(&cbd, nullptr, g_ConstantBuffer.put());
    if (FAILED(hr)) return false;

    // Create Sampler State
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.BorderColor[0] = 0.0f;
    sampDesc.BorderColor[1] = 0.0f;
    sampDesc.BorderColor[2] = 0.0f;
    sampDesc.BorderColor[3] = 0.0f; // transparent borders!
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_D3DDevice->CreateSamplerState(&sampDesc, g_SamplerState.put());
    if (FAILED(hr)) return false;

    return true;
}

void CreateControlPanelRTV() {
    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    g_ControlPanelSwapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
    g_D3DDevice->CreateRenderTargetView(backBuffer.get(), nullptr, g_ControlPanelRTV.put());
}

void CleanupControlPanelRTV() {
    g_ControlPanelRTV = nullptr;
}

void OpenProjectorWindow() {
    if (g_ProjectorHwnd) return;

    if (g_Config.monitorIndex < 0 || g_Config.monitorIndex >= g_Monitors.size()) return;
    const auto& m = g_Monitors[g_Config.monitorIndex];

    int x = m.rect.left;
    int y = m.rect.top;
    int w = m.rect.right - m.rect.left;
    int h = m.rect.bottom - m.rect.top;

    g_ProjectorHwnd = CreateWindowExW(
        WS_EX_TOPMOST, 
        L"EasyMapProjector", 
        L"EasyMap Projector Output", 
        WS_POPUP | WS_VISIBLE, 
        x, y, w, h, 
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );

    if (!g_ProjectorHwnd) return;

    // Create swap chain for the projector window using IDXGIFactory2 + waitable object
    winrt::com_ptr<IDXGIFactory2> dxgiFactory2;
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    g_D3DDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
    
    winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(dxgiAdapter.put());
    dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory2.put()));

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    desc1.Width       = w;
    desc1.Height      = h;
    desc1.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc1.Stereo      = FALSE;
    desc1.SampleDesc  = { 1, 0 };
    desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc1.BufferCount = 2;
    desc1.Scaling     = DXGI_SCALING_STRETCH;
    desc1.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc1.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    // FRAME_LATENCY_WAITABLE_OBJECT: lets us wait on a GPU event instead of blocking in Present
    desc1.Flags       = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HRESULT hr = dxgiFactory2->CreateSwapChainForHwnd(
        g_D3DDevice.get(), g_ProjectorHwnd, &desc1, nullptr, nullptr,
        reinterpret_cast<IDXGISwapChain1**>(g_ProjectorSwapChain.put()));
    if (FAILED(hr)) {
        DestroyWindow(g_ProjectorHwnd);
        g_ProjectorHwnd = nullptr;
        return;
    }

    // Configure frame latency: 1 frame max (lowest possible input lag)
    winrt::com_ptr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(g_ProjectorSwapChain->QueryInterface(IID_PPV_ARGS(sc2.put())))) {
        sc2->SetMaximumFrameLatency(1);
        g_ProjectorWaitableHandle = sc2->GetFrameLatencyWaitableObject();
    }

    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    g_ProjectorSwapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
    g_D3DDevice->CreateRenderTargetView(backBuffer.get(), nullptr, g_ProjectorRTV.put());

    // Focus projector window
    ShowWindow(g_ProjectorHwnd, SW_SHOW);
    SetForegroundWindow(g_ProjectorHwnd);

    // If configuration corners are defaulted, initialize to the monitor size
    if (g_Config.warp.corners[0].x == 100.0f && g_Config.warp.corners[0].y == 100.0f &&
        g_Config.warp.corners[1].x == 1820.0f && g_Config.warp.corners[2].y == 980.0f) {
        ResetWarpCorners();
    } else {
        UpdateWarp();
    }
}

void CloseProjectorWindow() {
    if (g_ProjectorWaitableHandle) {
        CloseHandle(g_ProjectorWaitableHandle);
        g_ProjectorWaitableHandle = nullptr;
    }
    g_ProjectorRTV = nullptr;
    g_ProjectorSwapChain = nullptr;
    if (g_ProjectorHwnd) {
        DestroyWindow(g_ProjectorHwnd);
        g_ProjectorHwnd = nullptr;
    }
}

void CleanupD3D() {
    CleanupControlPanelRTV();
    CloseProjectorWindow();
    
    g_SamplerState = nullptr;
    g_ConstantBuffer = nullptr;
    g_VertexShader = nullptr;
    g_PixelShader = nullptr;
    g_ControlPanelSwapChain = nullptr;
    g_D3DContext = nullptr;
    g_D3DDevice = nullptr;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Enable DPI awareness (Per-Monitor V2) to get correct physical resolutions
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
            SetProcessDPIAware();
        }
    }

    // Initialize COM / C++/WinRT Apartment
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Load Configuration
    LoadConfig(g_ConfigPath, g_Config);

    // Register classes
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProcControlPanel;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"EasyMapControlPanel";
    RegisterClassExW(&wc);

    WNDCLASSEXW wcp = { sizeof(wcp) };
    wcp.style = CS_HREDRAW | CS_VREDRAW;
    wcp.lpfnWndProc = WndProcProjector;
    wcp.hInstance = hInstance;
    wcp.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcp.lpszClassName = L"EasyMapProjector";
    RegisterClassExW(&wcp);

    // Create Control Panel Window (GUI)
    g_ControlPanelHwnd = CreateWindowW(
        wc.lpszClassName, 
        L"EasyMap Control Panel - Real-Time Window Warping", 
        WS_OVERLAPPEDWINDOW, 
        100, 100, 750, 650, 
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_ControlPanelHwnd) return 1;

    // Initialize Direct3D 11
    if (!InitD3D()) {
        CleanupD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        UnregisterClassW(wcp.lpszClassName, hInstance);
        return 1;
    }

    // Initialize Capture Session Backend
    g_Capture = std::make_unique<WindowCapture>(g_D3DDevice);

    // Show GUI
    ShowWindow(g_ControlPanelHwnd, SW_SHOW);
    UpdateWindow(g_ControlPanelHwnd);

    // Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // Sleek Dark Theme Design
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.30f, 0.50f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.45f, 0.70f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.22f, 0.40f, 1.00f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);

    ImGui_ImplWin32_Init(g_ControlPanelHwnd);
    ImGui_ImplDX11_Init(g_D3DDevice.get(), g_D3DContext.get());

    RefreshDisplays();
    RefreshWindowsList();

    // Auto-start capture if we have saved target in config
    if (!g_Config.captureWindowTitle.empty()) {
        for (const auto& w : g_Windows) {
            if (w.title == g_Config.captureWindowTitle) {
                g_Capture->Start(w.hwnd);
                break;
            }
        }
    }

    // Initial warping setup
    UpdateWarp();

    // Message Loop
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    bool done = false;

    // Initialize high-resolution performance counters
    QueryPerformanceFrequency(&g_Perf.freq);
    QueryPerformanceCounter(&g_Perf.frameStart);

    
    while (!done) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        // 1. Process Windows Graphics Capture Frame
        // Auto-restore minimized window if the option is enabled.
        // We check IsIconic() — if the window is still minimized after our SetWindowPlacement,
        // it means something overrode us (e.g. the app re-minimized itself). Move it again.
        if (g_Capture->IsActive() && g_Config.autoRestoreMinimized) {
            HWND targetHwnd = g_Capture->GetTargetHWND();
            if (targetHwnd && IsWindow(targetHwnd) && IsIconic(targetHwnd)) {
                MoveWindowOffscreen(targetHwnd);
                // Restart the WGC session — WGC may have internally paused when the
                // window was minimized; restarting it forces frame delivery to resume.
                g_Capture->Stop();
                g_CapturedSRV = nullptr;
                g_CaptureWidth = 0;
                g_CaptureHeight = 0;
                Sleep(80); // give DWM time to composite the newly-restored window
                g_Capture->Start(targetHwnd);
            }
        }
        // Track whether a genuinely new frame arrived this tick
        g_Perf.hadNewFrameThisTick = false;
        if (g_Capture->IsActive()) {
            LARGE_INTEGER beforeSRV;
            QueryPerformanceCounter(&beforeSRV);
            unsigned int prevW = g_CaptureWidth, prevH = g_CaptureHeight;
            bool updated = g_Capture->UpdateSRV(g_CapturedSRV, g_CaptureWidth, g_CaptureHeight);
            if (updated) {
                LARGE_INTEGER afterSRV;
                QueryPerformanceCounter(&afterSRV);
                g_Perf.captureLatencyMs = (afterSRV.QuadPart - beforeSRV.QuadPart) * 1000.0 / g_Perf.freq.QuadPart;
                g_Perf.hadNewFrameThisTick = true;
            }
        } else {
            g_CapturedSRV = nullptr;
        }

        // 2. Render Projector Output
        if (g_ProjectorHwnd && g_ProjectorRTV) {
            // Wait for the GPU to be ready for the next frame (waitable swap chain).
            // This replaces the blocking Present(1,0) wait — OS wakes us precisely
            // at VSync with near-zero jitter instead of sleeping in the driver.
            if (g_ProjectorWaitableHandle) {
                WaitForSingleObjectEx(g_ProjectorWaitableHandle, 1000, true);
            }

            float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            g_D3DContext->ClearRenderTargetView(g_ProjectorRTV.get(), clearColor);

            if (g_WarpValid) {
                RECT clientRect;
                GetClientRect(g_ProjectorHwnd, &clientRect);
                
                D3D11_VIEWPORT vp = {};
                vp.Width = (float)(clientRect.right - clientRect.left);
                vp.Height = (float)(clientRect.bottom - clientRect.top);
                vp.MinDepth = 0.0f;
                vp.MaxDepth = 1.0f;
                g_D3DContext->RSSetViewports(1, &vp);

                // Setup shader resources
                g_D3DContext->VSSetShader(g_VertexShader.get(), nullptr, 0);
                g_D3DContext->PSSetShader(g_PixelShader.get(), nullptr, 0);
                
                // Update Constant Buffer
                D3D11_MAPPED_SUBRESOURCE mappedResource;
                if (SUCCEEDED(g_D3DContext->Map(g_ConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
                    ShaderConstBuffer* cb = (ShaderConstBuffer*)mappedResource.pData;
                    
                    // Copy Homography Matrix
                    cb->HRow0[0] = g_Homography.m[0][0];
                    cb->HRow0[1] = g_Homography.m[0][1];
                    cb->HRow0[2] = g_Homography.m[0][2];
                    cb->HRow0[3] = 0.0f;

                    cb->HRow1[0] = g_Homography.m[1][0];
                    cb->HRow1[1] = g_Homography.m[1][1];
                    cb->HRow1[2] = g_Homography.m[1][2];
                    cb->HRow1[3] = 0.0f;

                    cb->HRow2[0] = g_Homography.m[2][0];
                    cb->HRow2[1] = g_Homography.m[2][1];
                    cb->HRow2[2] = g_Homography.m[2][2];
                    cb->HRow2[3] = 0.0f;

                    // Grid Settings
                    cb->GridColor[0] = g_Config.gridColor[0];
                    cb->GridColor[1] = g_Config.gridColor[1];
                    cb->GridColor[2] = g_Config.gridColor[2];
                    cb->GridColor[3] = g_Config.gridColor[3];
                    
                    cb->GridSettings[0] = g_Config.showGrid ? 1.0f : 0.0f;
                    cb->GridSettings[1] = (float)g_Config.gridCols;
                    cb->GridSettings[2] = (float)g_Config.gridRows;
                    cb->GridSettings[3] = g_Config.gridThickness;

                    // Calibration Guides Settings
                    cb->Corner0[0] = g_Config.warp.corners[0].x; cb->Corner0[1] = g_Config.warp.corners[0].y;
                    cb->Corner1[0] = g_Config.warp.corners[1].x; cb->Corner1[1] = g_Config.warp.corners[1].y;
                    cb->Corner2[0] = g_Config.warp.corners[2].x; cb->Corner2[1] = g_Config.warp.corners[2].y;
                    cb->Corner3[0] = g_Config.warp.corners[3].x; cb->Corner3[1] = g_Config.warp.corners[3].y;

                    cb->GuideSettings[0] = g_ShowGuides ? 1.0f : 0.0f;
                    cb->GuideSettings[1] = (float)g_SelectedCorner;
                    cb->GuideSettings[2] = 12.0f; // Handle radius in pixels
                    cb->GuideSettings[3] = 2.0f;  // Line thickness in pixels

                    cb->GuideColor[0] = 0.0f; cb->GuideColor[1] = 0.8f; cb->GuideColor[2] = 1.0f; cb->GuideColor[3] = 0.7f; // Cyan
                    cb->ActiveGuideColor[0] = 1.0f; cb->ActiveGuideColor[1] = 0.2f; cb->ActiveGuideColor[2] = 0.2f; cb->ActiveGuideColor[3] = 0.9f; // Red
                    
                    // Crop Settings (pass crop fractions to GPU)
                    cb->CropSettings[0] = g_Config.cropLeft;
                    cb->CropSettings[1] = g_Config.cropTop;
                    cb->CropSettings[2] = g_Config.cropRight;
                    cb->CropSettings[3] = g_Config.cropBottom;

                    g_D3DContext->Unmap(g_ConstantBuffer.get(), 0);
                }

                ID3D11Buffer* cbuffers[] = { g_ConstantBuffer.get() };
                g_D3DContext->PSSetConstantBuffers(0, 1, cbuffers);

                ID3D11ShaderResourceView* srvs[] = { g_CapturedSRV.get() };
                g_D3DContext->PSSetShaderResources(0, 1, srvs);

                ID3D11SamplerState* samplers[] = { g_SamplerState.get() };
                g_D3DContext->PSSetSamplers(0, 1, samplers);

                ID3D11RenderTargetView* rtvs[] = { g_ProjectorRTV.get() };
                g_D3DContext->OMSetRenderTargets(1, rtvs, nullptr);
                g_D3DContext->IASetInputLayout(nullptr);
                g_D3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                
                // Draw full-screen quad (the pixel shader performs the warp mapping)
                g_D3DContext->Draw(4, 0);
            }

            // Present projector with SyncInterval=0 — timing is handled by the
            // waitable handle above. DXGI_PRESENT_DO_NOT_WAIT avoids driver stalls.
            LARGE_INTEGER projPresStart, projPresEnd;
            QueryPerformanceCounter(&projPresStart);
            g_ProjectorSwapChain->Present(0, g_ProjectorWaitableHandle ? DXGI_PRESENT_DO_NOT_WAIT : 0);
            QueryPerformanceCounter(&projPresEnd);
            g_Perf.projRenderMs = (projPresEnd.QuadPart - projPresStart.QuadPart) * 1000.0 / g_Perf.freq.QuadPart;
        }

        // 3. Render Control Panel (GUI)
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("ControlPanelContainer", nullptr, 
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | 
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "EasyMap - Real-Time Window Warping & Projection");
        ImGui::Separator();
        ImGui::Spacing();

        // Section: Display Output
        if (ImGui::CollapsingHeader("1. Output Projector Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Refresh Displays")) {
                RefreshDisplays();
            }
            ImGui::SameLine();
            ImGui::Text("Connected Monitors: %d", (int)g_Monitors.size());

            std::vector<std::string> monitorNames;
            int activeMonitorIndex = -1;
            for (int i = 0; i < g_Monitors.size(); ++i) {
                const auto& m = g_Monitors[i];
                char buf[128];
                sprintf_s(buf, "Display %d: %S %s (%dx%d)", 
                    i, m.name.c_str(), m.isPrimary ? "[Primary]" : "", 
                    m.rect.right - m.rect.left, m.rect.bottom - m.rect.top);
                monitorNames.push_back(buf);
                if (g_Config.monitorIndex == i) activeMonitorIndex = i;
            }

            if (activeMonitorIndex == -1 && !g_Monitors.empty()) {
                g_Config.monitorIndex = 0;
                activeMonitorIndex = 0;
            }

            std::string currentMonStr = g_Monitors.empty() ? "No displays found" : monitorNames[activeMonitorIndex];
            if (ImGui::BeginCombo("Select Display", currentMonStr.c_str())) {
                for (int i = 0; i < monitorNames.size(); ++i) {
                    bool isSelected = (g_Config.monitorIndex == i);
                    if (ImGui::Selectable(monitorNames[i].c_str(), isSelected)) {
                        g_Config.monitorIndex = i;
                        // If projector is running, move it
                        if (g_ProjectorHwnd) {
                            CloseProjectorWindow();
                            OpenProjectorWindow();
                        }
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            if (g_ProjectorHwnd) {
                if (ImGui::Button("Close Projector Window", ImVec2(220, 35))) {
                    CloseProjectorWindow();
                }
            } else {
                if (ImGui::Button("Open Projector Window", ImVec2(220, 35))) {
                    OpenProjectorWindow();
                }
            }
        }

        ImGui::Spacing();

        // Section: Capture Source
        if (ImGui::CollapsingHeader("2. Source Capture Window", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Refresh Windows")) {
                RefreshWindowsList();
            }
            
            std::vector<std::string> winTitles;
            int activeWinIdx = -1;
            for (int i = 0; i < g_Windows.size(); ++i) {
                std::wstring wt = g_Windows[i].title;
                std::string titleUtf8 = std::string(wt.begin(), wt.end()); // Simple ASCII/UTF-8 conversion for combo box
                winTitles.push_back(titleUtf8);
                if (g_Capture->IsActive() && g_Capture->GetTargetHWND() == g_Windows[i].hwnd) {
                    activeWinIdx = i;
                }
            }

            std::string currentWinStr = activeWinIdx == -1 ? "Select target window..." : winTitles[activeWinIdx];
            if (ImGui::BeginCombo("Select Window", currentWinStr.c_str())) {
                for (int i = 0; i < winTitles.size(); ++i) {
                    bool isSelected = (activeWinIdx == i);
                    if (ImGui::Selectable(winTitles[i].c_str(), isSelected)) {
                        g_Config.captureWindowTitle = g_Windows[i].title;
                        g_Capture->Start(g_Windows[i].hwnd);
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (g_Capture->IsActive()) {
                ImGui::Text("Status: "); ImGui::SameLine();
                HWND th = g_Capture->GetTargetHWND();
                bool isMinimized = th && IsIconic(th);
                bool isOffscreen = g_SavedPlacement.wasOffscreen && g_SavedPlacement.hwnd == th;

                if (g_CaptureWidth == 0 || g_CaptureHeight == 0) {
                    if (isMinimized && !g_Config.autoRestoreMinimized) {
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Minimized — no frames");
                        ImGui::TextWrapped("Enable 'Keep minimized windows active' below, or unminimize the window.");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "Waiting for frames...");
                        ImGui::TextWrapped("Make sure the target window is not running as Administrator.");
                    }
                } else if (isOffscreen) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Capturing Real-Time (%dx%d)", g_CaptureWidth, g_CaptureHeight);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "[transparent]");
                } else {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Capturing Real-Time (%dx%d)", g_CaptureWidth, g_CaptureHeight);
                }

                ImGui::Checkbox("Auto-keep minimized windows active (transparent)", &g_Config.autoRestoreMinimized);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("When enabled, if the captured window is minimized, it will automatically be made transparent & topmost on-screen to keep capturing.");

                if (isOffscreen) {
                    if (ImGui::Button("Restore window visibility")) {
                        RestoreSavedWindow();
                    }
                } else {
                    if (th && ImGui::Button("Make window transparent (keep capturing)")) {
                        MoveWindowOffscreen(th);
                    }
                }
                ImGui::SameLine();

                if (ImGui::Button("Stop Capture")) {
                    RestoreSavedWindow(); // Restore before stopping
                    g_Capture->Stop();
                }
            } else {
                ImGui::Text("Status: "); ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Inactive");
            }
        }

        ImGui::Spacing();

        // Section: Calibration / Warping
        if (ImGui::CollapsingHeader("3. Projection Mapping & Warp Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Show Corner Handles & Guides", &g_ShowGuides);
            ImGui::SameLine();
            if (ImGui::Button("Reset Warp Corners")) {
                ResetWarpCorners();
            }

            ImGui::Spacing();
            ImGui::Text("Drag corners directly on the Projector Output window,");
            ImGui::Text("or select a corner below and use arrow keys (Shift for 10px):");

            const char* cornerNames[] = { "0: Top-Left", "1: Top-Right", "2: Bottom-Right", "3: Bottom-Left" };
            if (ImGui::BeginCombo("Fine-tune Corner", g_SelectedCorner == -1 ? "None selected" : cornerNames[g_SelectedCorner])) {
                if (ImGui::Selectable("None", g_SelectedCorner == -1)) g_SelectedCorner = -1;
                for (int i = 0; i < 4; ++i) {
                    if (ImGui::Selectable(cornerNames[i], g_SelectedCorner == i)) g_SelectedCorner = i;
                }
                ImGui::EndCombo();
            }

            // Float inputs for corner coordinates
            for (int i = 0; i < 4; ++i) {
                float pos[2] = { g_Config.warp.corners[i].x, g_Config.warp.corners[i].y };
                char label[32];
                sprintf_s(label, "Corner %d px", i);
                if (ImGui::InputFloat2(label, pos, "%.1f")) {
                    g_Config.warp.corners[i].x = pos[0];
                    g_Config.warp.corners[i].y = pos[1];
                    UpdateWarp();
                }
            }
        }

        ImGui::Spacing();

        // Section: Grid Overlay
        if (ImGui::CollapsingHeader("4. Alignment Grid Options")) {
            ImGui::Checkbox("Show Alignment Grid", &g_Config.showGrid);
            ImGui::SliderInt("Grid Columns", &g_Config.gridCols, 2, 40);
            ImGui::SliderInt("Grid Rows", &g_Config.gridRows, 2, 40);
            ImGui::SliderFloat("Grid Line Width", &g_Config.gridThickness, 1.0f, 10.0f, "%.1f px");
            ImGui::ColorEdit4("Grid Color", g_Config.gridColor);
        }

        ImGui::Spacing();

        // Section: Image Cropping Options
        if (ImGui::CollapsingHeader("5. Image Cropping Options")) {
            ImGui::Text("Crop margins to remove window borders or title bars:");
            ImGui::SliderFloat("Crop Top", &g_Config.cropTop, 0.0f, 0.25f, "%.3f");
            ImGui::SliderFloat("Crop Bottom", &g_Config.cropBottom, 0.0f, 0.25f, "%.3f");
            ImGui::SliderFloat("Crop Left", &g_Config.cropLeft, 0.0f, 0.25f, "%.3f");
            ImGui::SliderFloat("Crop Right", &g_Config.cropRight, 0.0f, 0.25f, "%.3f");
            
            if (ImGui::Button("Reset Crop")) {
                g_Config.cropLeft = 0.0f;
                g_Config.cropTop = 0.0f;
                g_Config.cropRight = 0.0f;
                g_Config.cropBottom = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Crop Standard Window Borders")) {
                // Approximate standard Windows titlebar and side border sizes
                g_Config.cropLeft = 0.005f;
                g_Config.cropTop = 0.032f;
                g_Config.cropRight = 0.005f;
                g_Config.cropBottom = 0.005f;
            }
        }

        ImGui::Spacing();

        // Section: Performance Debug
        if (ImGui::CollapsingHeader("6. Performance Debug")) {
            ImGui::Checkbox("Show Performance Overlay", &g_Perf.showDebug);
            ImGui::Spacing();

            // Colour-coded FPS
            float fps = g_Perf.fps;
            ImVec4 fpsColor = fps >= 55.0f ? ImVec4(0.0f,1.0f,0.3f,1.0f)
                            : fps >= 40.0f ? ImVec4(1.0f,0.8f,0.0f,1.0f)
                                           : ImVec4(1.0f,0.2f,0.2f,1.0f);
            ImGui::Text("Render FPS:"); ImGui::SameLine();
            ImGui::TextColored(fpsColor, "%.1f fps", fps);

            ImGui::Text("Frame time:  "); ImGui::SameLine();
            ImGui::TextColored(fpsColor, "%.2f ms", g_Perf.frameTimeMs);

            ImGui::Text("Present time:"); ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f,0.7f,1.0f,1.0f), "%.2f ms", g_Perf.projRenderMs);

            ImGui::Text("SRV update:  "); ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f,0.7f,1.0f,1.0f), "%.2f ms", g_Perf.captureLatencyMs);

            ImGui::Text("Dropped (cap):"); ImGui::SameLine();
            ImVec4 dropColor = g_Perf.droppedFrames == 0
                             ? ImVec4(0.0f,1.0f,0.3f,1.0f)
                             : ImVec4(1.0f,0.3f,0.3f,1.0f);
            ImGui::TextColored(dropColor, "%d", g_Perf.droppedFrames);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) g_Perf.droppedFrames = 0;

            // FPS history graph
            if (!g_Perf.fpsHistory.empty()) {
                std::vector<float> hist(g_Perf.fpsHistory.begin(), g_Perf.fpsHistory.end());
                ImGui::PlotLines("##fps", hist.data(), (int)hist.size(), 0,
                                 nullptr, 0.0f, 75.0f, ImVec2(-1, 50));
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Green=OK (>=55fps)  Yellow=marginal  Red=dropping");
            ImGui::TextDisabled("'Dropped' = render ticks with no new WGC frame.");
            ImGui::TextDisabled("'Present time' spikes = VSync wait (normal on vsync).");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Save & Load
        if (ImGui::Button("Save Calibration Settings", ImVec2(240, 40))) {
            if (SaveConfig(g_ConfigPath, g_Config)) {
                MessageBoxA(g_ControlPanelHwnd, "Settings saved successfully to config.txt!", "EasyMap", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxA(g_ControlPanelHwnd, "Failed to save settings!", "EasyMap Error", MB_OK | MB_ICONERROR);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Last Saved Settings", ImVec2(240, 40))) {
            if (LoadConfig(g_ConfigPath, g_Config)) {
                UpdateWarp();
                // Trigger reconnect if caption is valid
                if (g_Capture && !g_Config.captureWindowTitle.empty()) {
                    for (const auto& w : g_Windows) {
                        if (w.title == g_Config.captureWindowTitle) {
                            g_Capture->Start(w.hwnd);
                            break;
                        }
                    }
                }
            } else {
                MessageBoxA(g_ControlPanelHwnd, "Failed to load settings!", "EasyMap Error", MB_OK | MB_ICONERROR);
            }
        }

        ImGui::End();

        // Render Control Panel GUI to swap chain
        ImGui::Render();
        
        float clearColor[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
        g_D3DContext->ClearRenderTargetView(g_ControlPanelRTV.get(), clearColor);
        ID3D11RenderTargetView* rtvs[] = { g_ControlPanelRTV.get() };
        g_D3DContext->OMSetRenderTargets(1, rtvs, nullptr);
        
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Control panel: Present(0) — never blocks, no vsync stall on GUI
        g_ControlPanelSwapChain->Present(0, 0);

        // --- Frame timing bookkeeping ---
        QueryPerformanceCounter(&g_Perf.frameEnd);
        g_Perf.frameTimeMs = (g_Perf.frameEnd.QuadPart - g_Perf.frameStart.QuadPart)
                             * 1000.0 / g_Perf.freq.QuadPart;
        g_Perf.frameStart = g_Perf.frameEnd;
        g_Perf.fps = g_Perf.frameTimeMs > 0.0 ? (float)(1000.0 / g_Perf.frameTimeMs) : 0.0f;
        if (g_Perf.fpsHistory.size() >= 120) g_Perf.fpsHistory.pop_front();
        g_Perf.fpsHistory.push_back(g_Perf.fps);
        // Count frames where no new capture arrived (≈ dropped from capture pipeline)
        if (g_Capture->IsActive() && g_CaptureWidth > 0 && !g_Perf.hadNewFrameThisTick)
            ++g_Perf.droppedFrames;
    }

    // Restore window transparency/placement if the app is closed while capturing in transparent mode
    RestoreSavedWindow();

    // Shut down capture before cleanup
    if (g_Capture) {
        g_Capture->Stop();
        g_Capture = nullptr;
    }

    // Cleanup ImGui
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    // Cleanup Direct3D
    CleanupD3D();

    // Destroy Control Panel Window
    if (g_ControlPanelHwnd) {
        DestroyWindow(g_ControlPanelHwnd);
        g_ControlPanelHwnd = nullptr;
    }

    UnregisterClassW(wc.lpszClassName, hInstance);
    UnregisterClassW(wcp.lpszClassName, hInstance);

    return (int)msg.wParam;
}
