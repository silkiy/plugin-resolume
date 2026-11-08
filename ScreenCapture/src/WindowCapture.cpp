#include "WindowCapture.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <new>
#include <utility>

namespace
{
using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
using winrt::Windows::Graphics::Capture::IGraphicsCaptureSession2;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

constexpr auto capturePixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr std::int64_t maxCapturePixels = 32 * 1024 * 1024;
constexpr int frameCount = 2;
constexpr auto retryInterval = std::chrono::seconds(1);

class ScopedRuntime
{
public:
    ScopedRuntime() : status_(RoInitialize(RO_INIT_MULTITHREADED)) {}

    ~ScopedRuntime()
    {
        if (SUCCEEDED(status_))
            RoUninitialize();
    }

    // A thread already initialized as STA can use WinRT in its existing apartment.
    bool Ready() const { return SUCCEEDED(status_) || status_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT status_;
};

class MtaLease
{
public:
    MtaLease() { winrt::check_hresult(CoIncrementMTAUsage(&cookie_)); }
    ~MtaLease() { CoDecrementMTAUsage(cookie_); }

private:
    CO_MTA_USAGE_COOKIE cookie_ = nullptr;
};

bool IsCloaked(HWND window)
{
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

BOOL CALLBACK CollectWindows(HWND window, LPARAM data)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0 || processId == GetCurrentProcessId() || !IsWindowVisible(window) ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0 || IsCloaked(window))
        return TRUE;

    wchar_t title[256]{};
    const int length = GetWindowTextW(window, title, static_cast<int>(sizeof(title) / sizeof(*title)));
    if (length <= 0)
        return TRUE;

    try
    {
        auto& windows = *reinterpret_cast<std::vector<CaptureWindow>*>(data);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, title, length, nullptr, 0, nullptr, nullptr);
        if (bytes > 0)
        {
            std::string utf8Title(bytes, '\0');
            WideCharToMultiByte(CP_UTF8, 0, title, length, utf8Title.data(), bytes, nullptr, nullptr);
            windows.push_back({reinterpret_cast<std::uintptr_t>(window), processId, std::move(utf8Title)});
        }
    }
    catch (const std::bad_alloc&)
    {
        return FALSE;
    }
    return TRUE;
}

bool IsCaptureSizeValid(SizeInt32 size)
{
    return size.Width > 0 && size.Height > 0 &&
        size.Width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        size.Height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        static_cast<std::int64_t>(size.Width) * size.Height <= maxCapturePixels;
}

bool SupportsWindowCapture()
{
    // On restricted desktops even the activation factory can fail. Treat this as
    // an unavailable source without translating the failure into a WinRT exception.
    using SessionStatics = ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics;
    constexpr wchar_t className[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";
    HSTRING_HEADER storage{};
    HSTRING name{};
    if (FAILED(WindowsCreateStringReference(className, ARRAYSIZE(className) - 1, &storage, &name)))
        return false;

    winrt::com_ptr<SessionStatics> factory;
    if (FAILED(RoGetActivationFactory(name, __uuidof(SessionStatics), factory.put_void())))
        return false;

    boolean supported = false;
    return SUCCEEDED(factory->IsSupported(&supported)) && supported;
}

template<typename T>
void Close(T& object) noexcept
{
    if (object)
    {
        try { object.Close(); }
        catch (...) {}
        object = nullptr;
    }
}
}

struct WindowCapture::State
{
    State(std::uintptr_t handle, std::uint32_t processId)
        : window(reinterpret_cast<HWND>(handle)), processId(processId)
    {
    }

    ~State() { Stop(); }

    bool Start()
    {
        if (!SupportsWindowCapture())
            return false;

        // These factories must not outlive the MTA when the host's render thread has no COM lease of its own.
        const auto itemFactory = winrt::try_get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        const auto poolFactory = winrt::try_get_activation_factory<Direct3D11CaptureFramePool, IDirect3D11CaptureFramePoolStatics2>();
        if (!itemFactory || !poolFactory)
            return false;
        winrt::check_hresult(itemFactory->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
        size = item.Size();
        if (!IsCaptureSizeValid(size))
        {
            Stop();
            return false;
        }

        auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        if (result == DXGI_ERROR_UNSUPPORTED)
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        winrt::check_hresult(result);
        context.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);

        winrt::com_ptr<IInspectable> deviceBridge;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(), deviceBridge.put()));
        captureDevice = deviceBridge.as<IDirect3DDevice>();
        pool = poolFactory.CreateFreeThreaded(captureDevice, capturePixelFormat, frameCount, size);

        closed = std::make_shared<std::atomic_bool>(false);
        closedSubscription = item.Closed(winrt::auto_revoke, [flag = closed](auto&&, auto&&) { flag->store(true); });
        StartSession();
        return true;
    }

    void StartSession()
    {
        session = pool.CreateCaptureSession(item);
        if (const auto cursorSession = session.try_as<IGraphicsCaptureSession2>())
            cursorSession.IsCursorCaptureEnabled(false);
        session.StartCapture();
    }

    void Stop() noexcept
    {
        closedSubscription.revoke();
        Close(session);
        Close(pool);
        item = nullptr;
        captureDevice = nullptr;
        staging = nullptr;
        context = nullptr;
        device = nullptr;
        pending = false;
        publishedSize = {};
        std::vector<std::uint8_t>().swap(pixels);
    }

    Result Poll()
    {
        Result result = Result::Waiting;
        if (pending)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto status = context->Map(staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (status == DXGI_ERROR_WAS_STILL_DRAWING)
                return Result::Waiting;
            winrt::check_hresult(status);

            const auto bytesPerRow = static_cast<std::size_t>(size.Width) * 4;
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
            for (int row = 0; row < size.Height; ++row)
                std::memcpy(pixels.data() + (size.Height - 1 - row) * bytesPerRow, source + row * mapped.RowPitch, bytesPerRow);
            context->Unmap(staging.get(), 0);
            pending = false;
            publishedSize = size;
            result = Result::NewFrame;
        }

        auto frame = pool.TryGetNextFrame();
        if (!frame)
            return result;
        const auto contentSize = frame.ContentSize();
        if (!IsCaptureSizeValid(contentSize))
            return Result::Unavailable;
        if (contentSize.Width != size.Width || contentSize.Height != size.Height)
        {
            frame.Close();
            // Ask for a fresh first frame after resizing, including for static windows.
            Close(session);
            pool.Recreate(captureDevice, capturePixelFormat, frameCount, contentSize);
            size = contentSize;
            staging = nullptr;
            StartSession();
            return result;
        }

        if (!staging)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(size.Width);
            desc.Height = static_cast<UINT>(size.Height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
            pixels.resize(static_cast<std::size_t>(size.Width) * size.Height * 4);
        }

        const auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
        const D3D11_BOX contentBox{0, 0, 0, static_cast<UINT>(size.Width), static_cast<UINT>(size.Height), 1};
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture.get(), 0, &contentBox);
        context->Flush();
        pending = true;
        return result;
    }

    // Keep the MTA alive between render calls without changing the host's apartment.
    MtaLease mta;
    HWND window;
    std::uint32_t processId;
    bool retired = false;
    bool pending = false;
    std::shared_ptr<std::atomic_bool> closed;
    std::chrono::steady_clock::time_point nextStart{};
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    IDirect3DDevice captureDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    GraphicsCaptureItem::Closed_revoker closedSubscription;
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    SizeInt32 size{};
    SizeInt32 publishedSize{};
    std::vector<std::uint8_t> pixels;
};

std::vector<CaptureWindow> ListCaptureWindows()
{
    std::vector<CaptureWindow> windows;
    if (!EnumWindows(CollectWindows, reinterpret_cast<LPARAM>(&windows)))
        return {};
    return windows;
}

WindowCapture::WindowCapture() = default;
WindowCapture::~WindowCapture() { Reset(); }

WindowCapture::Result WindowCapture::Capture(const CaptureWindow& target)
{
    const ScopedRuntime runtime;
    if (!runtime.Ready())
        return Result::Unavailable;

    const auto window = reinterpret_cast<HWND>(target.handle);
    try
    {
        if (!state_ || window != state_->window || target.processId != state_->processId)
            state_ = std::make_unique<State>(target.handle, target.processId);

        DWORD actualProcessId = 0;
        GetWindowThreadProcessId(window, &actualProcessId);
        if (state_->retired || actualProcessId == 0 || actualProcessId != target.processId ||
            (state_->closed && state_->closed->load()))
        {
            state_->retired = true;
            state_->Stop();
            return Result::Unavailable;
        }
        if (!IsWindowVisible(window) || IsIconic(window) || IsCloaked(window))
        {
            state_->Stop();
            return Result::Unavailable;
        }

        if (!state_->session)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < state_->nextStart)
                return Result::Unavailable;
            state_->nextStart = now + retryInterval;
            if (!state_->Start())
                return Result::Unavailable;
        }

        return state_->Poll();
    }
    catch (const winrt::hresult_error&)
    {
        if (state_)
            state_->Stop();
    }
    catch (const std::bad_alloc&)
    {
        if (state_)
            state_->Stop();
    }
    return Result::Unavailable;
}

const void* WindowCapture::Pixels() const { return state_ ? state_->pixels.data() : nullptr; }
int WindowCapture::Width() const { return state_ ? state_->publishedSize.Width : 0; }
int WindowCapture::Height() const { return state_ ? state_->publishedSize.Height : 0; }

void WindowCapture::Reset()
{
    if (!state_)
        return;
    const ScopedRuntime runtime;
    state_.reset();
}
