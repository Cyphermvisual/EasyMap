#include "capture.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <dwmapi.h>
#include <stdexcept>

// Helper to convert DXGI Device to WinRT Device
inline winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice CreateDirect3DDevice(IDXGIDevice* dxgiDevice) {
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put()));
    return inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

// Helper to create Capture Item from HWND
inline winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd) {
    auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(factory->CreateForWindow(hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), reinterpret_cast<void**>(winrt::put_abi(item))));
    return item;
}

struct EnumData {
    std::vector<WindowInfo> list;
    HWND exclude1;
    HWND exclude2;
};

// Window filtering callback
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto data = reinterpret_cast<EnumData*>(lParam);

    if (hwnd == data->exclude1 || hwnd == data->exclude2) return TRUE;

    // Filter invisible windows
    if (!IsWindowVisible(hwnd)) return TRUE;

    // Filter windows with empty titles
    int length = GetWindowTextLengthW(hwnd);
    if (length == 0) return TRUE;

    // Filter system shell window
    HWND shellWindow = GetShellWindow();
    if (hwnd == shellWindow) return TRUE;

    // Filter by style (skip tool windows and overlays)
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    // Filter cloaked windows (suspended UWP apps, virtual desktop background windows)
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        if (cloaked != 0) return TRUE;
    }

    std::vector<wchar_t> titleBuffer(length + 1, L'\0');
    GetWindowTextW(hwnd, titleBuffer.data(), length + 1);
    std::wstring title(titleBuffer.data());

    data->list.push_back({ hwnd, title });
    return TRUE;
}

WindowCapture::WindowCapture(winrt::com_ptr<ID3D11Device> d3dDevice) : m_d3dDevice(d3dDevice) {
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(m_d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
    m_winrtDevice = CreateDirect3DDevice(dxgiDevice.get());
    // Get the immediate device context for GPU texture copies
    m_d3dDevice->GetImmediateContext(m_d3dContext.put());
}

WindowCapture::~WindowCapture() {
    Stop();
}

std::vector<WindowInfo> WindowCapture::EnumerateWindows(HWND excludeHwnd1, HWND excludeHwnd2) {
    EnumData data;
    data.exclude1 = excludeHwnd1;
    data.exclude2 = excludeHwnd2;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    return data.list;
}

bool WindowCapture::Start(HWND targetHwnd) {
    if (m_isActive) {
        Stop();
    }

    if (!IsWindow(targetHwnd)) return false;

    m_targetHwnd = targetHwnd;

    try {
        m_item = CreateCaptureItemForWindow(m_targetHwnd);
        m_currentSize = m_item.Size();

        // Create the free-threaded frame pool
        m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3, // 3-buffer pipeline for smoother frame delivery
            m_currentSize
        );

        // Frame arrived event callback (raised on a worker thread)
        m_frameArrivedRevoker = m_framePool.FrameArrived(winrt::auto_revoke, [this](auto const& sender, auto const&) {
            auto frame = sender.TryGetNextFrame();
            if (frame) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pendingFrame = frame;
                m_hasNewFrame = true;
            }
        });

        // Create the capture session
        m_session = m_framePool.CreateCaptureSession(m_item);

        // Try to disable the yellow border (Windows 11 / Windows 10 2004+)
        try {
            m_session.IsBorderRequired(false);
        } catch (...) {
            // Safe fallback if the OS build does not support disabling borders
        }

        m_session.StartCapture();
        m_isActive = true;
        return true;
    } catch (...) {
        Stop();
        return false;
    }
}

void WindowCapture::Stop() {
    m_isActive = false;
    m_frameArrivedRevoker.revoke();
    
    if (m_session) {
        try { m_session.Close(); } catch (...) {}
        m_session = nullptr;
    }
    
    if (m_framePool) {
        try { m_framePool.Close(); } catch (...) {}
        m_framePool = nullptr;
    }
    
    m_item = nullptr;
    m_targetHwnd = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingFrame = nullptr;
        m_hasNewFrame = false;
    }
    m_currentSize = { 0, 0 };
    m_ownedTexture = nullptr; // Release our owned copy on stop
}

bool WindowCapture::UpdateSRV(winrt::com_ptr<ID3D11ShaderResourceView>& outSRV, unsigned int& outWidth, unsigned int& outHeight) {
    if (!m_isActive) return false;

    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{ nullptr };
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hasNewFrame) {
            frame = m_pendingFrame;
            m_pendingFrame = nullptr;
            m_hasNewFrame = false;
        }
    }

    if (frame == nullptr) {
        // If no new frame, continue using the existing Shader Resource View
        if (outSRV != nullptr) {
            outWidth = m_currentSize.Width;
            outHeight = m_currentSize.Height;
            return true;
        }
        return false;
    }

    try {
        auto contentSize = frame.ContentSize();
        
        // Recreate pool and owned texture if the source window size has changed
        bool sizeChanged = (contentSize.Width != m_currentSize.Width ||
                            contentSize.Height != m_currentSize.Height);
        if (sizeChanged) {
            m_currentSize = contentSize;
            m_ownedTexture = nullptr; // Force recreation below
            m_framePool.Recreate(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                3,
                m_currentSize
            );
        }

        // Get the WGC-owned GPU texture from the frame
        auto surface = frame.Surface();
        auto dxgiInterfaceAccess = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> wgcTexture;
        winrt::check_hresult(dxgiInterfaceAccess->GetInterface(winrt::guid_of<ID3D11Texture2D>(), wgcTexture.put_void()));

        // Create or recreate our owned texture (same desc, but ours exclusively)
        if (!m_ownedTexture) {
            D3D11_TEXTURE2D_DESC wgcDesc = {};
            wgcTexture->GetDesc(&wgcDesc);

            D3D11_TEXTURE2D_DESC ownedDesc = {};
            ownedDesc.Width            = wgcDesc.Width;
            ownedDesc.Height           = wgcDesc.Height;
            ownedDesc.MipLevels        = 1;
            ownedDesc.ArraySize        = 1;
            ownedDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            ownedDesc.SampleDesc.Count = 1;
            ownedDesc.Usage            = D3D11_USAGE_DEFAULT;
            ownedDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
            ownedDesc.CPUAccessFlags   = 0;
            ownedDesc.MiscFlags        = 0;

            if (FAILED(m_d3dDevice->CreateTexture2D(&ownedDesc, nullptr, m_ownedTexture.put()))) {
                return false;
            }

            // Rebuild the SRV for the new owned texture
            outSRV = nullptr;
            if (FAILED(m_d3dDevice->CreateShaderResourceView(m_ownedTexture.get(), nullptr, outSRV.put()))) {
                m_ownedTexture = nullptr;
                return false;
            }
        }

        // GPU copy: WGC texture → our owned texture (fully GPU-side, no CPU stall)
        m_d3dContext->CopyResource(m_ownedTexture.get(), wgcTexture.get());
        // Release the WGC frame ASAP so the pool can recycle the buffer
        frame = nullptr;

        outWidth  = m_currentSize.Width;
        outHeight = m_currentSize.Height;
        return true;
    } catch (...) {
        return false;
    }
}
