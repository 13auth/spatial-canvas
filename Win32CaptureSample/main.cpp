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
            L"Bu Windows sürümünde ekran yakalama (Windows.Graphics.Capture) desteklenmiyor.\n"
            L"Spatial Canvas Windows 10 1903+ / Windows 11 gerektirir.",
            L"Spatial Canvas",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    // Spatial Canvas: tüm uygulama Canvas.cpp/RunCanvasApp() içinde. robmikh örnek
    // UI'si (App/SampleWindow/Composition) artık kullanılmıyor - giriş doğrudan tuval.
    return RunCanvasApp();
}