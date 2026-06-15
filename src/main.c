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

#include <mferror.h>

#define FAILURE 0
#define SUCCESS 1

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

static IMFTransform* g_Encoder = NULL;

static UINT g_Width = 0;
static UINT g_Height = 0;

static UINT64 g_FrameIndex = 0;

ID3D11VideoDevice* videoDevice = NULL;
ID3D11VideoContext* videoContext = NULL;

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

static int InitEncoder(UINT width, UINT height)
{
    HRESULT hr;

    g_Width  = width;
    g_Height = height;

    MFT_REGISTER_TYPE_INFO outputType =
    {
        MFMediaType_Video,
        MFVideoFormat_H264
    };

    IMFActivate** activates = NULL;
    UINT32 count = 0;

    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE,
        NULL,
        &outputType,
        &activates,
        &count);
    if (FAILED(hr) || count == 0) return FAILURE;

    hr = activates[0]->lpVtbl->ActivateObject(activates[0], &IID_IMFTransform, (void**)&g_Encoder);
    if (FAILED(hr)) return FAILURE;


    printf("Supported input formats:\n");

    IMFMediaType* type = NULL;

    for (DWORD i = 0;; i++)
    {
        hr = g_Encoder->lpVtbl->GetInputAvailableType(
            g_Encoder,
            0,
            i,
            &type);

        if (FAILED(hr))
            break;

        GUID subtype;

        if (SUCCEEDED(type->lpVtbl->GetGUID(
            type,
            &MF_MT_SUBTYPE,
            &subtype)))
        {
            if (IsEqualGUID(&subtype, &MFVideoFormat_NV12))
                printf("NV12\n");

            else if (IsEqualGUID(&subtype, &MFVideoFormat_ARGB32))
                printf("ARGB32\n");

            else if (IsEqualGUID(&subtype, &MFVideoFormat_RGB32))
                printf("RGB32\n");

            else
                printf("Other format\n");
        }

        type->lpVtbl->Release(type);
        type = NULL;
    }

    //IMFMediaType* type = NULL;

    DWORD index = 0;

    while (SUCCEEDED(
        g_Encoder->lpVtbl->GetInputAvailableType(
            g_Encoder,
            0,
            index,
            &type)))
    {
        GUID subtype;

        HRESULT hr = type->lpVtbl->GetGUID(
            type,
            &MF_MT_SUBTYPE,
            &subtype);

        if (SUCCEEDED(hr))
        {
            if (IsEqualGUID(&subtype, &MFVideoFormat_NV12))
                printf("Input %lu: NV12\n", index);

            else if (IsEqualGUID(&subtype, &MFVideoFormat_RGB32))
                printf("Input %lu: RGB32\n", index);

            else if (IsEqualGUID(&subtype, &MFVideoFormat_ARGB32))
                printf("Input %lu: ARGB32\n", index);

            else if (IsEqualGUID(&subtype, &MFVideoFormat_YUY2))
                printf("Input %lu: YUY2\n", index);

            else
            {
                LPOLESTR guidString = NULL;

                StringFromCLSID(&subtype, &guidString);

                wprintf(
                    L"Input %lu: %ls\n",
                    index,
                    guidString);

                CoTaskMemFree(guidString);
            }
        }

        type->lpVtbl->Release(type);
        type = NULL;

        index++;
    }

    IMFMediaType* inputType = NULL;

    MFCreateMediaType(&inputType);

    UINT64 frameSize = ((UINT64)width << 32) | height;
    UINT64 frameRate = ((UINT64)60 << 32) | 1;

    inputType->lpVtbl->SetGUID(inputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    inputType->lpVtbl->SetGUID(inputType, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    inputType->lpVtbl->SetUINT64(inputType, &MF_MT_FRAME_SIZE, frameSize);
    inputType->lpVtbl->SetUINT64(inputType, &MF_MT_FRAME_RATE, frameRate);

    hr = g_Encoder->lpVtbl->SetInputType(g_Encoder, 0, inputType, 0);

    if (FAILED(hr)) return FAILURE;

    IMFMediaType* outputMediaType = NULL;

    MFCreateMediaType(&outputMediaType);

    outputMediaType->lpVtbl->SetGUID(outputMediaType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    outputMediaType->lpVtbl->SetGUID(outputMediaType, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    outputMediaType->lpVtbl->SetUINT64(outputMediaType,&MF_MT_FRAME_SIZE, frameSize);
    outputMediaType->lpVtbl->SetUINT64(outputMediaType, &MF_MT_FRAME_RATE, frameRate);
    outputMediaType->lpVtbl->SetUINT32(outputMediaType, &MF_MT_AVG_BITRATE, 8000000);

    hr = g_Encoder->lpVtbl->SetOutputType(g_Encoder, 0, outputMediaType, 0);

    if (FAILED(hr)) return FAILURE;

    g_Encoder->lpVtbl->ProcessMessage(g_Encoder, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    g_Encoder->lpVtbl->ProcessMessage(g_Encoder, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return SUCCESS;
}


static int EncodeNV12Frame(BYTE* nv12Data, DWORD size)
{
    HRESULT hr;

    IMFMediaBuffer* buffer = NULL;
    IMFSample* sample = NULL;

    hr = MFCreateMemoryBuffer(size, &buffer);

    if (FAILED(hr)) return FAILURE;

    BYTE* dst = NULL;

    buffer->lpVtbl->Lock(buffer, &dst, NULL, NULL);

    memcpy(dst, nv12Data, size);

    buffer->lpVtbl->Unlock(buffer);
    buffer->lpVtbl->SetCurrentLength(buffer,size);

    MFCreateSample(&sample);

    sample->lpVtbl->AddBuffer(sample,buffer);
    sample->lpVtbl->SetSampleTime(sample, g_FrameIndex * 166666);
    sample->lpVtbl->SetSampleDuration(sample, 166666);

    hr = g_Encoder->lpVtbl->ProcessInput(g_Encoder, 0, sample, 0);

    sample->lpVtbl->Release(sample);
    buffer->lpVtbl->Release(buffer);

    g_FrameIndex++;

    if (FAILED(hr)) return FAILURE;

    return SUCCESS;
}

static int ReceiveEncodedPacket(void)
{
    HRESULT hr;

    MFT_OUTPUT_STREAM_INFO streamInfo;

    hr = g_Encoder->lpVtbl->GetOutputStreamInfo(g_Encoder, 0, &streamInfo);

    if (FAILED(hr)) return FAILURE;

    IMFMediaBuffer* outputBuffer = NULL;

    hr = MFCreateMemoryBuffer(streamInfo.cbSize, &outputBuffer);

    if (FAILED(hr)) return FAILURE;

    IMFSample* sample = NULL;

    MFCreateSample(&sample);

    sample->lpVtbl->AddBuffer(sample, outputBuffer);

    MFT_OUTPUT_DATA_BUFFER output;

    ZeroMemory(&output, sizeof(output));

    output.dwStreamID = 0;
    output.pSample = sample;

    DWORD status = 0;

    hr = g_Encoder->lpVtbl->ProcessOutput(
        g_Encoder,
        0,
        1,
        &output,
        &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        sample->lpVtbl->Release(sample);
        outputBuffer->lpVtbl->Release(outputBuffer);
        return FAILURE;
    }

    BYTE* data = NULL;

    DWORD maxLen;
    DWORD curLen;

    outputBuffer->lpVtbl->Lock(outputBuffer, &data, &maxLen, &curLen);

    if (curLen) SendFrame(data, curLen);

    outputBuffer->lpVtbl->Unlock(outputBuffer);

    sample->lpVtbl->Release(sample);
    outputBuffer->lpVtbl->Release(outputBuffer);

    return SUCCESS;
}

static int InitDesktopDuplication(void)
{
    IDXGIDevice* dxgiDevice = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIOutput* output = NULL;
    IDXGIOutput1* output1 = NULL;

    HRESULT hr;

    hr = g_Device->lpVtbl->QueryInterface(g_Device, &IID_ID3D11VideoDevice, (void**)&videoDevice);
    if (FAILED(hr)) return FAILURE;

    hr = g_Context->lpVtbl->QueryInterface(g_Context, &IID_ID3D11VideoContext, (void**)&videoContext);
    if (FAILED(hr)) return FAILURE;

    hr = g_Device->lpVtbl->QueryInterface(g_Device, &IID_IDXGIDevice, (void**)&dxgiDevice);
    if (FAILED(hr)) return FAILURE;

    hr = dxgiDevice->lpVtbl->GetAdapter(dxgiDevice,&adapter);
    dxgiDevice->lpVtbl->Release(dxgiDevice);
    if (FAILED(hr)) return FAILURE;

    hr = adapter->lpVtbl->EnumOutputs(adapter, 0, &output);
    adapter->lpVtbl->Release(adapter);
    if (FAILED(hr)) return FAILURE;

    hr = output->lpVtbl->QueryInterface(output,&IID_IDXGIOutput1, (void**)&output1);
    output->lpVtbl->Release(output);
    if (FAILED(hr)) return FAILURE;

    hr = output1->lpVtbl->DuplicateOutput(output1, (IUnknown*)g_Device, &g_Duplication);
    output1->lpVtbl->Release(output1);
    if (FAILED(hr)) return FAILURE;

    return SUCCESS;
}

static ID3D11Texture2D* CaptureDesktopFrame(void)
{
    IDXGIResource* resource = NULL;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    HRESULT hr;

    hr = g_Duplication->lpVtbl->AcquireNextFrame(
        g_Duplication,
        16, // 60 FPS
        &frameInfo,
        &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) { printf("timeout\n"); return NULL; }

    if (FAILED(hr)) { printf("AcquireNextFrame: 0x%08X\n", (unsigned)hr); return NULL; }

    ID3D11Texture2D* texture = NULL;

    hr = resource->lpVtbl->QueryInterface(resource, &IID_ID3D11Texture2D, (void**)&texture);

    resource->lpVtbl->Release(resource);

    g_Duplication->lpVtbl->ReleaseFrame(g_Duplication);
    if (FAILED(hr)) return NULL;

    return texture;
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
            return FAILURE;
        }
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
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
        return FAILURE;
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
        return FAILURE;
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
        return FAILURE;
    }

    return SUCCESS;
}

static void Render(void)
{
    float clearColor[4] = { 0.1f, 0.2f, 0.4f, 1.0f };

    g_Context->lpVtbl->OMSetRenderTargets(g_Context, 1, &g_RTV, NULL);
    g_Context->lpVtbl->ClearRenderTargetView(g_Context, g_RTV, clearColor);
    g_SwapChain->lpVtbl->Present(g_SwapChain, 1, 0);
}

static int HostConnection(void)
{
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2,2), &wsa)) return FAILURE;

    s_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s_Socket == INVALID_SOCKET) return FAILURE;

    struct sockaddr_in addr;

    ZeroMemory(&addr, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s_Socket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) return FAILURE;

    printf("Waiting for client...\n");

    char buffer[64];

    int received = recvfrom(s_Socket,buffer,sizeof(buffer),0, (SOCKADDR*)&s_RemoteAddr, &s_RemoteAddrLen);

    if (received <= 0) return FAILURE;
    if (received >= sizeof(buffer)) received = sizeof(buffer) - 1;

    buffer[received] = 0;

    printf("Client connected: %s\n",inet_ntoa(s_RemoteAddr.sin_addr));

    sendto(s_Socket, "OK", 2, 0, (SOCKADDR*)&s_RemoteAddr, s_RemoteAddrLen);

    return SUCCESS;
}

static int JoinConnection(void)
{
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2,2), &wsa)) return FAILURE;

    s_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s_Socket == INVALID_SOCKET) return FAILURE;

    char ip[64];

    printf("IP: ");
    scanf("%63s", ip);

    ZeroMemory(&s_RemoteAddr, sizeof(s_RemoteAddr));

    s_RemoteAddr.sin_family = AF_INET;
    s_RemoteAddr.sin_port = htons(7777);

    inet_pton(AF_INET, ip, &s_RemoteAddr.sin_addr);

    sendto(s_Socket, "HELLO", 5, 0, (SOCKADDR*)&s_RemoteAddr, sizeof(s_RemoteAddr));

    char buffer[64];

    DWORD timeout = 1000; // 1 sekund

    setsockopt(
        s_Socket,
        SOL_SOCKET,
        SO_RCVTIMEO,
        (const char*)&timeout,
        sizeof(timeout));

    int received = recvfrom(
        s_Socket,
        buffer,
        sizeof(buffer),
        0,
        NULL,
        NULL);

    if (received <= 0) return FAILURE;
    if (received == SOCKET_ERROR)
    {
        int err = WSAGetLastError();

        if (err == WSAETIMEDOUT)
        {
            printf("Connection timeout\n");
        }

        return FAILURE;
    }

    return SUCCESS;
}

int main(void)
{
    outputType.guidMajorType = MFMediaType_Video;
    outputType.guidSubtype = MFVideoFormat_H264;

    HINSTANCE hInstance = GetModuleHandleA(NULL);

    AllocConsole();

    FILE* dummy = NULL;

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

    if (!connected) { printf("Connection failed\n"); return FAILURE; }

    printf("Connection established\n");

    u_long nonBlocking = 1;

    ioctlsocket(s_Socket, FIONBIO, &nonBlocking);

    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));

    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "D3D11Window";

    if (!RegisterClassA(&wc)) { printf("RegisterClassA failed\n"); return FAILURE; }

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

    if (!hwnd) { printf("CreateWindowExA failed\n"); return FAILURE; }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    CoInitializeEx(NULL,COINIT_MULTITHREADED);
    MFStartup(MF_VERSION,MFSTARTUP_NOSOCKET);

    if (!InitD3D11(hwnd)) { printf("InitD3D11 failed\n"); return FAILURE; }
    if (!InitDesktopDuplication()) { printf("InitDesktopDuplication failed\n"); return FAILURE; }
    else { printf("Desktop duplication OK\n"); }

    HRESULT hr = {0};

    MSG msg;

    ZeroMemory(&msg, sizeof(msg));

    while (msg.message != WM_QUIT)
    {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        ID3D11Texture2D* desktopTexture = CaptureDesktopFrame();

        if (desktopTexture)
        {
            printf("Frame captured\n");
            fflush(stdout);

            if (!g_StagingTexture)
            {
                D3D11_TEXTURE2D_DESC desc;

                desktopTexture->lpVtbl->GetDesc(desktopTexture, &desc);

                InitEncoder(desc.Width, desc.Height);

                desc.BindFlags = 0;
                desc.MiscFlags = 0;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                g_Device->lpVtbl->CreateTexture2D(g_Device, &desc, NULL, &g_StagingTexture);
            }

            g_Context->lpVtbl->CopyResource(g_Context, (ID3D11Resource*)g_StagingTexture, (ID3D11Resource*)desktopTexture);

            D3D11_MAPPED_SUBRESOURCE mapped;

            hr = g_Context->lpVtbl->Map(
                    g_Context,
                    (ID3D11Resource*)g_StagingTexture,
                    0,
                    D3D11_MAP_READ,
                    0,
                    &mapped);

            if (SUCCEEDED(hr))
            {
                unsigned char* pixels = (unsigned char*)mapped.pData;

                UINT pitch = mapped.RowPitch;

                //EncodeNV12Frame();
                //ReceiveEncodedPacket();

                g_Context->lpVtbl->Unmap(g_Context, (ID3D11Resource*)g_StagingTexture, 0);
            }

            desktopTexture->lpVtbl->Release(desktopTexture);
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
