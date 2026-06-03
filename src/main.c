#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Ws2_32.lib")

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

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

#include <codecapi.h>

#include <wmcodecdsp.h>

static ID3D11Device* g_Device = NULL;
static ID3D11DeviceContext* g_Context = NULL;
static IDXGISwapChain* g_SwapChain = NULL;
static ID3D11RenderTargetView* g_RTV = NULL;

#define PORT 7777

static SOCKET s_Socket = INVALID_SOCKET;

static struct sockaddr_in s_RemoteAddr;
static int s_RemoteAddrLen = sizeof(s_RemoteAddr);

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

        char packet[65536];

        int received = recvfrom(
            s_Socket,
            packet,
            sizeof(packet),
            0,
            NULL,
            NULL);

        if (received > 0)
        {
            printf("Received %d bytes\n", received);
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
