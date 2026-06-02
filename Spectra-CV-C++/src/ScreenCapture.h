#pragma once
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <opencv2/opencv.hpp>
#include <wrl/client.h>

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();
    
    // Initializes DirectX capture API (0 = Primary monitor, 1 = Secondary, etc.)
    bool Initialize(int monitorIndex = 0);
    
    // Retrieves the latest screen frame in cv::Mat (BGRA) format
    cv::Mat GetLatestFrame();
    
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_deskDupl;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
    
    int m_width;
    int m_height;
    int m_monitorIndex;
};
