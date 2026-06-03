#include "ScreenCapture.h"
#include <iostream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

ScreenCapture::ScreenCapture() : m_width(0), m_height(0), m_monitorIndex(0) {}

ScreenCapture::~ScreenCapture() {}

bool ScreenCapture::Initialize(int monitorIndex) {
    m_monitorIndex = monitorIndex;
    
    // Release existing resources before reinitializing
    m_deskDupl.Reset();
    m_stagingTexture.Reset();
    m_context.Reset();
    m_device.Reset();

    HRESULT hr = S_OK;

    // Create DXGI factory to enumerate ALL GPU adapters
    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory);
    if (FAILED(hr)) return false;

    int totalOutputIndex = 0; // Global screen index across all cards

    // Loop through all GPU adapters (NVIDIA, Intel, virtual Parsec, etc.)
    for (UINT adapterIdx = 0; ; adapterIdx++) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (dxgiFactory->EnumAdapters1(adapterIdx, &adapter) == DXGI_ERROR_NOT_FOUND)
            break; // No more adapters

        // Loop through all connected outputs/monitors on this adapter
        for (UINT outputIdx = 0; ; outputIdx++) {
            Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
            if (adapter->EnumOutputs(outputIdx, &dxgiOutput) == DXGI_ERROR_NOT_FOUND)
                break; // No more outputs on this adapter

            // Is this the monitor we're looking for?
            if (totalOutputIndex != monitorIndex) {
                totalOutputIndex++;
                continue;
            }

            // Verify this output is attached to the desktop (not a virtual empty one)
            DXGI_OUTPUT_DESC desc;
            dxgiOutput->GetDesc(&desc);
            if (!desc.AttachedToDesktop) {
                totalOutputIndex++;
                continue;
            }

            // Create D3D11 device specifically on THIS adapter
            D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
            D3D_FEATURE_LEVEL featureLevel;
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                featureLevels, 2, D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context);
            if (FAILED(hr)) { totalOutputIndex++; continue; }

            // Get IDXGIOutput1 interface for duplication
            Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;
            hr = dxgiOutput.As(&dxgiOutput1);
            if (FAILED(hr)) { totalOutputIndex++; continue; }

            hr = dxgiOutput1->DuplicateOutput(m_device.Get(), &m_deskDupl);
            if (FAILED(hr)) { totalOutputIndex++; continue; }

            // Retrieve screen dimensions
            DXGI_OUTDUPL_DESC duplDesc;
            m_deskDupl->GetDesc(&duplDesc);
            m_width  = duplDesc.ModeDesc.Width;
            m_height = duplDesc.ModeDesc.Height;

            // Create staging texture (for CPU copy)
            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width            = m_width;
            texDesc.Height           = m_height;
            texDesc.MipLevels        = 1;
            texDesc.ArraySize        = 1;
            texDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage            = D3D11_USAGE_STAGING;
            texDesc.BindFlags        = 0;
            texDesc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

            hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_stagingTexture);
            if (FAILED(hr)) return false;

            std::cout << "[SCREENC-APTURE-INITIALIZE] Capture initialized: Adapter " << adapterIdx
                      << ", Monitor " << outputIdx
                      << " (" << m_width << "x" << m_height << ")" << std::endl;
            return true;
        }
    }

    std::cerr << "[ERROR-SCREEN-CAPTURE-INITIALIZE] Monitor index " << monitorIndex << " not found on any adapter." << std::endl;
    return false;
}


cv::Mat ScreenCapture::GetLatestFrame() {
    if (!m_deskDupl) {
        // Attempt to reinitialize if access was lost (e.g. exclusive fullscreen transition)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!Initialize(m_monitorIndex)) {
            return cv::Mat();
        }
    }

    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    
    // Release in case a previous lock was left stuck
    m_deskDupl->ReleaseFrame();
    
    // Short timeout (100ms) to avoid blocking if no frame is available
    HRESULT hr = m_deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::cout << "[ERROR-SCREEN-CAPTURE-ACQUIRE-FRAME] DXGI_ERROR_ACCESS_LOST detected. Reinitializing..." << std::endl;
            m_deskDupl.Reset(); // Force re-initialization on next call
        }
        return cv::Mat();
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    if (FAILED(hr)) return cv::Mat();

    m_context->CopyResource(m_stagingTexture.Get(), desktopTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return cv::Mat();

    // Create an OpenCV image from mapped memory
    cv::Mat frame(m_height, m_width, CV_8UC4, mapped.pData, mapped.RowPitch);
    cv::Mat resultFrame;
    
    // Copy is necessary to be able to unmap GPU memory afterwards
    frame.copyTo(resultFrame);

    m_context->Unmap(m_stagingTexture.Get(), 0);
    m_deskDupl->ReleaseFrame();
    
    // Return native BGRA directly without full conversion to save CPU
    return resultFrame;
}
