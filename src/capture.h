#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <d3d11.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

struct WindowInfo {
    HWND hwnd;
    std::wstring title;
};

class WindowCapture {
public:
    WindowCapture(winrt::com_ptr<ID3D11Device> d3dDevice);
    ~WindowCapture();

    // Enumerates capturable windows, excluding the specified HWNDs (e.g. EasyMap's own windows)
    static std::vector<WindowInfo> EnumerateWindows(HWND excludeHwnd1 = nullptr, HWND excludeHwnd2 = nullptr);

    // Start capturing a window. Returns true if successful.
    bool Start(HWND targetHwnd);
    
    // Stop the active capture session
    void Stop();

    // Check for a new frame, recreate pool if size changed, and update the SRV.
    // Call this in the main render thread.
    bool UpdateSRV(winrt::com_ptr<ID3D11ShaderResourceView>& outSRV, unsigned int& outWidth, unsigned int& outHeight);
    
    // Check if a capture is currently running
    bool IsActive() const { return m_isActive; }
    
    // Get target HWND
    HWND GetTargetHWND() const { return m_targetHwnd; }

private:
    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
    
    HWND m_targetHwnd{ nullptr };
    bool m_isActive{ false };

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker m_frameArrivedRevoker;

    std::mutex m_mutex;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_pendingFrame{ nullptr };
    bool m_hasNewFrame{ false };
    
    winrt::Windows::Graphics::SizeInt32 m_currentSize{ 0, 0 };

    // Our own D3D11 texture we copy each WGC frame into.
    // This gives us exclusive GPU ownership and avoids WGC read/write races.
    winrt::com_ptr<ID3D11Texture2D> m_ownedTexture;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext; // immediate context for CopyResource
};
