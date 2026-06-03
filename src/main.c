#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Ws2_32.lib")

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleAut32.lib")

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <initguid.h>
#include <d3d11.h>

#include <dxgi.h>
#include <dxgi1_2.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <strmif.h>
#include <codecapi.h>

#include <wmcodecdsp.h>

static ID3D11Device* g_Device = NULL;
static ID3D11DeviceContext* g_Context = NULL;
static ID3D11RenderTargetView* g_RTV = NULL;
static ID3D11Texture2D* g_StagingTexture = NULL;

static IDXGISwapChain* g_SwapChain = NULL;
static IDXGIOutputDuplication* g_Duplication = NULL;

IMFActivate** activates = NULL;
UINT32 count = 0;

static MFT_REGISTER_TYPE_INFO outputType = {0};

#define PORT 7777

static SOCKET s_Socket = INVALID_SOCKET;

static struct sockaddr_in s_RemoteAddr;
static int s_RemoteAddrLen = sizeof(s_RemoteAddr);

static int InitDesktopDuplication(void)
{
    IDXGIDevice* dxgiDevice = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIOutput* output = NULL;
    IDXGIOutput1* output1 = NULL;

    HRESULT hr;

    hr = g_Device->lpVtbl->QueryInterface(
        g_Device,
        &IID_IDXGIDevice,
        (void**)&dxgiDevice);

    if (FAILED(hr))
        return 0;

    hr = dxgiDevice->lpVtbl->GetAdapter(
        dxgiDevice,
        &adapter);

    dxgiDevice->lpVtbl->Release(dxgiDevice);

    if (FAILED(hr))
        return 0;

    hr = adapter->lpVtbl->EnumOutputs(
        adapter,
        0,
        &output);

    adapter->lpVtbl->Release(adapter);

    if (FAILED(hr))
        return 0;

    hr = output->lpVtbl->QueryInterface(
        output,
        &IID_IDXGIOutput1,
        (void**)&output1);

    output->lpVtbl->Release(output);

    if (FAILED(hr))
        return 0;

    hr = output1->lpVtbl->DuplicateOutput(
        output1,
        (IUnknown*)g_Device,
        &g_Duplication);

    output1->lpVtbl->Release(output1);

    if (FAILED(hr))
        return 0;

    return 1;
}

static ID3D11Texture2D* CaptureDesktopFrame(void)
{
    IDXGIResource* resource = NULL;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    HRESULT hr;

    hr = g_Duplication->lpVtbl->AcquireNextFrame(
        g_Duplication,
        0,
        &frameInfo,
        &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return NULL;

    if (FAILED(hr))
        return NULL;

    ID3D11Texture2D* texture = NULL;

    hr = resource->lpVtbl->QueryInterface(
        resource,
        &IID_ID3D11Texture2D,
        (void**)&texture);

    resource->lpVtbl->Release(resource);

    g_Duplication->lpVtbl->ReleaseFrame(
        g_Duplication);

    if (FAILED(hr))
        return NULL;

    return texture;
}

static LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
        case WM_DESTROY:
        {            
            if (s_Socket != INVALID_SOCKET)
            {
                closesocket(s_Socket);
                s_Socket = INVALID_SOCKET;
            }

            WSACleanup();
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcA(
        hwnd,
        msg,
        wParam,
        lParam);
}

static int InitD3D11(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC scd;

    ZeroMemory(&scd, sizeof(scd));

    scd.BufferCount = 1;
    scd.BufferDesc.Width = 1280;
    scd.BufferDesc.Height = 720;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        0,
        NULL,
        0,
        D3D11_SDK_VERSION,
        &scd,
        &g_SwapChain,
        &g_Device,
        NULL,
        &g_Context);

    if (FAILED(hr))
    {
        printf("D3D11CreateDeviceAndSwapChain failed\n");
        return 0;
    }

    ID3D11Texture2D* backBuffer = NULL;

    hr = g_SwapChain->lpVtbl->GetBuffer(
        g_SwapChain,
        0,
        &IID_ID3D11Texture2D,
        (void**)&backBuffer);

    if (FAILED(hr))
    {
        printf("GetBuffer failed\n");
        return 0;
    }

    hr = g_Device->lpVtbl->CreateRenderTargetView(
        g_Device,
        (ID3D11Resource*)backBuffer,
        NULL,
        &g_RTV);

    backBuffer->lpVtbl->Release(backBuffer);

    if (FAILED(hr))
    {
        printf("CreateRenderTargetView failed\n");
        return 0;
    }

    return 1;
}

static void Render(void)
{
    float clearColor[4] =
    {
        0.1f,
        0.2f,
        0.4f,
        1.0f
    };

    g_Context->lpVtbl->OMSetRenderTargets(
        g_Context,
        1,
        &g_RTV,
        NULL);

    g_Context->lpVtbl->ClearRenderTargetView(
        g_Context,
        g_RTV,
        clearColor);

    g_SwapChain->lpVtbl->Present(
        g_SwapChain,
        1,
        0);
}

static int HostConnection(void)
{
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2,2), &wsa))
        return 0;

    s_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s_Socket == INVALID_SOCKET)
        return 0;

    struct sockaddr_in addr;

    ZeroMemory(&addr, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(
        s_Socket,
        (SOCKADDR*)&addr,
        sizeof(addr)) == SOCKET_ERROR)
    {
        return 0;
    }

    printf("Waiting for client...\n");

    char buffer[64];

    int received = recvfrom(
        s_Socket,
        buffer,
        sizeof(buffer),
        0,
        (SOCKADDR*)&s_RemoteAddr,
        &s_RemoteAddrLen);

    if (received <= 0) return 0;

    if (received >= sizeof(buffer))
        received = sizeof(buffer) - 1;

    buffer[received] = 0;

    printf("Client connected: %s\n",inet_ntoa(s_RemoteAddr.sin_addr));

    sendto(
        s_Socket,
        "OK",
        2,
        0,
        (SOCKADDR*)&s_RemoteAddr,
        s_RemoteAddrLen);

    return 1;
}

static int JoinConnection(void)
{
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2,2), &wsa))
        return 0;

    s_Socket = socket(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP);

    if (s_Socket == INVALID_SOCKET)
        return 0;

    char ip[64];

    printf("IP: ");
    scanf("%63s", ip);

    ZeroMemory(&s_RemoteAddr, sizeof(s_RemoteAddr));

    s_RemoteAddr.sin_family = AF_INET;
    s_RemoteAddr.sin_port = htons(7777);

    inet_pton(
        AF_INET,
        ip,
        &s_RemoteAddr.sin_addr);

    sendto(
        s_Socket,
        "HELLO",
        5,
        0,
        (SOCKADDR*)&s_RemoteAddr,
        sizeof(s_RemoteAddr));

    char buffer[64];

    int received = recvfrom(
        s_Socket,
        buffer,
        sizeof(buffer),
        0,
        NULL,
        NULL);

    if (received <= 0)
        return 0;

    return 1;
}

void SendFrame(void* data, int size)
{
    sendto(
        s_Socket,
        (const char*)data,
        size,
        0,
        (SOCKADDR*)&s_RemoteAddr,
        s_RemoteAddrLen);
}

int main(void)
{

    outputType.guidMajorType = MFMediaType_Video;
    outputType.guidSubtype = MFVideoFormat_H264;

    HINSTANCE hInstance = GetModuleHandleA(NULL);

    AllocConsole();

    FILE* dummy;

    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONIN$",  "r", stdin);

    printf("Console attached\n");

    printf("1. Host\n");
    printf("2. Join\n");

    int mode = 0;

    scanf_s("%d", &mode);

    int connected = 0;

    if (mode == 1) connected = HostConnection();
    else if (mode == 2) connected = JoinConnection();

    if (!connected)
    {
        printf("Connection failed\n");
        return 0;
    }

    printf("Connection established\n");

    u_long nonBlocking = 1;

    ioctlsocket(
        s_Socket,
        FIONBIO,
        &nonBlocking);

    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));

    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "D3D11Window";

    if (!RegisterClassA(&wc))
    {
        printf("RegisterClassA failed\n");
        return 0;
    }

    HWND hwnd = CreateWindowExA(
        0,
        "D3D11Window",
        "D3D11 Example",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        720,
        NULL,
        NULL,
        hInstance,
        NULL);

    if (!hwnd)
    {
        printf("CreateWindowExA failed\n");
        return 0;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!InitD3D11(hwnd))
    {
        printf("InitD3D11 failed\n");
        return 0;
    }

    if (!InitDesktopDuplication())
    {
        printf("InitDesktopDuplication failed\n");
        return 0;
    }

    MFStartup(MF_VERSION,MFSTARTUP_NOSOCKET);

    printf("=== VIDEO ENCODERS ===\n");

    IMFActivate** encoders = NULL;
    UINT32 encoderCount = 0;

    MFT_REGISTER_TYPE_INFO encoderOutput =
    {
        MFMediaType_Video,
        MFVideoFormat_H264
    };

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_ALL,
        NULL,
        &encoderOutput,
        &encoders,
        &encoderCount);

    printf("Encoder count: %u\n\n", encoderCount);

    for (UINT32 i = 0; i < encoderCount; i++)
    {
        WCHAR* name = NULL;
        UINT32 length = 0;

        hr = encoders[i]->lpVtbl->GetAllocatedString(
            encoders[i],
            &MFT_FRIENDLY_NAME_Attribute,
            &name,
            &length);

        if (SUCCEEDED(hr))
        {
            wprintf(
                L"Encoder %u: %ls\n",
                i,
                name);

            CoTaskMemFree(name);
        }
    }

    printf("\n");

    printf("=== VIDEO DECODERS ===\n");

    IMFActivate** decoders = NULL;
    UINT32 decoderCount = 0;

    MFT_REGISTER_TYPE_INFO decoderInput =
    {
        MFMediaType_Video,
        MFVideoFormat_H264
    };

    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_ALL,
        &decoderInput,
        NULL,
        &decoders,
        &decoderCount);

    printf("Decoder count: %u\n\n", decoderCount);

    for (UINT32 i = 0; i < decoderCount; i++)
    {
        WCHAR* name = NULL;
        UINT32 length = 0;

        hr = decoders[i]->lpVtbl->GetAllocatedString(
            decoders[i],
            &MFT_FRIENDLY_NAME_Attribute,
            &name,
            &length);

        if (SUCCEEDED(hr))
        {
            wprintf(
                L"Decoder %u: %ls\n",
                i,
                name);

            CoTaskMemFree(name);
        }
    }

    MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE,
      NULL,
      &outputType,
      &activates,
      &count);

    IMFTransform* encoder = NULL;

    activates[0]->lpVtbl->ActivateObject(
        activates[0],
        &IID_IMFTransform,
        (void**)&encoder);

    DWORD index = 0;

    while (1)
    {
        IMFMediaType* type = NULL;

        HRESULT hr =
            encoder->lpVtbl->GetInputAvailableType(
                encoder,
                0,
                index,
                &type);

        if (FAILED(hr))
            break;

        GUID subtype;

        hr = type->lpVtbl->GetGUID(
            type,
            &MF_MT_SUBTYPE,
            &subtype);

        if (SUCCEEDED(hr))
        {
            if (IsEqualGUID(&subtype, &MFVideoFormat_NV12))
                printf("Input %lu = NV12\n", index);

            else if (IsEqualGUID(&subtype, &MFVideoFormat_YUY2))
                printf("Input %lu = YUY2\n", index);

            else if (IsEqualGUID(&subtype, &MFVideoFormat_RGB32))
                printf("Input %lu = RGB32\n", index);

            else if (IsEqualGUID(&subtype, &MFVideoFormat_ARGB32))
                printf("Input %lu = ARGB32\n", index);

            else
            {
                LPOLESTR str = NULL;

                StringFromCLSID(
                    &subtype,
                    &str);

                wprintf(
                    L"Input %lu = %ls\n",
                    index,
                    str);

                CoTaskMemFree(str);
            }
        }

        type->lpVtbl->Release(type);

        index++;
    }

    DWORD i = 0;

    while (1)
    {
        IMFMediaType* type = NULL;

        HRESULT hr =
            encoder->lpVtbl->GetInputAvailableType(
                encoder,
                0,
                i,
                &type);

        if (FAILED(hr))
            break;

        GUID subtype;

        type->lpVtbl->GetGUID(
            type,
            &MF_MT_SUBTYPE,
            &subtype);

        LPOLESTR guidString = NULL;

        StringFromCLSID(
            &subtype,
            &guidString);

        wprintf(L"Input %lu: %ls\n",
            i,
            guidString);

        CoTaskMemFree(guidString);

        type->lpVtbl->Release(type);

        i++;
    }



    IMFMediaType* inputType = NULL;

    MFCreateMediaType(&inputType);

    inputType->lpVtbl->SetGUID(
        inputType,
        &MF_MT_MAJOR_TYPE,
        &MFMediaType_Video);

    inputType->lpVtbl->SetGUID(
        inputType,
        &MF_MT_SUBTYPE,
        &MFVideoFormat_ARGB32);

    UINT64 frameSize =
        ((UINT64)1920 << 32) | 1080;

    inputType->lpVtbl->SetUINT64(
        inputType,
        &MF_MT_FRAME_SIZE,
        frameSize);

    UINT64 frameRate =
        ((UINT64)60 << 32) | 1;

    inputType->lpVtbl->SetUINT64(
        inputType,
        &MF_MT_FRAME_RATE,
        frameRate);

    hr = encoder->lpVtbl->SetInputType(
        encoder,
        0,
        inputType,
        0);
    printf("SetInputType = 0x%08X\n", (unsigned)hr);

    IMFMediaType* outputType = NULL;

    MFCreateMediaType(&outputType);

    outputType->lpVtbl->SetGUID(
        outputType,
        &MF_MT_MAJOR_TYPE,
        &MFMediaType_Video);

    outputType->lpVtbl->SetGUID(
        outputType,
        &MF_MT_SUBTYPE,
        &MFVideoFormat_H264);

    outputType->lpVtbl->SetUINT64(
        outputType,
        &MF_MT_FRAME_SIZE,
        frameSize);

    outputType->lpVtbl->SetUINT64(
        outputType,
        &MF_MT_FRAME_RATE,
        frameRate);

    outputType->lpVtbl->SetUINT32(
        outputType,
        &MF_MT_AVG_BITRATE,
        8000000);

    hr = encoder->lpVtbl->SetOutputType(
        encoder,
        0,
        outputType,
        0);
    printf("SetOutputType = 0x%08X\n", (unsigned)hr);

    encoder->lpVtbl->ProcessMessage(
        encoder,
        MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
        0);

    encoder->lpVtbl->ProcessMessage(
        encoder,
        MFT_MESSAGE_NOTIFY_START_OF_STREAM,
        0);

    MSG msg;

    ZeroMemory(&msg, sizeof(msg));

    while (msg.message != WM_QUIT)
    {
        while (PeekMessageA(
            &msg,
            NULL,
            0,
            0,
            PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        ID3D11Texture2D* desktopTexture =
            CaptureDesktopFrame();

        if (desktopTexture)
        {
            if (!g_StagingTexture)
            {
                D3D11_TEXTURE2D_DESC desc;

                desktopTexture->lpVtbl->GetDesc(
                    desktopTexture,
                    &desc);

                desc.BindFlags = 0;
                desc.MiscFlags = 0;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                g_Device->lpVtbl->CreateTexture2D(
                    g_Device,
                    &desc,
                    NULL,
                    &g_StagingTexture);
            }

            g_Context->lpVtbl->CopyResource(
                g_Context,
                (ID3D11Resource*)g_StagingTexture,
                (ID3D11Resource*)desktopTexture);

            D3D11_MAPPED_SUBRESOURCE mapped;

            HRESULT hr =
                g_Context->lpVtbl->Map(
                    g_Context,
                    (ID3D11Resource*)g_StagingTexture,
                    0,
                    D3D11_MAP_READ,
                    0,
                    &mapped);

            if (SUCCEEDED(hr))
            {
                unsigned char* pixels =
                    (unsigned char*)mapped.pData;

                UINT pitch =
                    mapped.RowPitch;

                //
                // tutaj:
                //
                // EncodeH264(
                //     pixels,
                //     width,
                //     height,
                //     pitch);
                //
                // SendFrame(...)
                //

                g_Context->lpVtbl->Unmap(
                    g_Context,
                    (ID3D11Resource*)g_StagingTexture,
                    0);
            }

            desktopTexture->lpVtbl->Release(
                desktopTexture);
        }

        Render();
    }

    if (g_RTV) g_RTV->lpVtbl->Release(g_RTV);

    if (g_SwapChain) g_SwapChain->lpVtbl->Release(g_SwapChain);

    if (g_Context) g_Context->lpVtbl->Release(g_Context);

    if (g_Device) g_Device->lpVtbl->Release(g_Device);

    if (s_Socket != INVALID_SOCKET)
    {
        closesocket(s_Socket);
        s_Socket = INVALID_SOCKET;
    }

    WSACleanup();

    return 0;
}
