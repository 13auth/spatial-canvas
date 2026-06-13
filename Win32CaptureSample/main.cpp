#include "pch.h"
#include "Canvas.h"

int __stdcall WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    // Initialize COM
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Check to see that capture is supported
    auto isCaptureSupported = winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
    if (!isCaptureSupported)
    {
        MessageBoxW(nullptr,
            L"Screen capture (Windows.Graphics.Capture) is not supported on this Windows version.\n"
            L"Spatial Canvas requires Windows 10 1903+ / Windows 11.",
            L"Spatial Canvas",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    // Spatial Canvas: tüm uygulama Canvas.cpp/RunCanvasApp() içinde. robmikh örnek
    // UI'si (App/SampleWindow/Composition) artık kullanılmıyor - giriş doğrudan tuval.
    return RunCanvasApp();
}