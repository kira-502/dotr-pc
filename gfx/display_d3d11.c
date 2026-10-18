/* display_d3d11.c - a plain Win32 window that shows the GS output through a Direct3D 11 swap chain.
 *
 * The PS2 picture is copied into the middle of a back buffer whose shape matches the window, padded with black so
 * the picture keeps the TV's 4:3 shape; DXGI stretches that buffer to the window. Alt+Enter / F11 toggle a
 * borderless full screen (no display mode change). A lost device (driver update, GPU reset) is recreated. Pacing
 * is the runtime's 60 Hz clock: Present does not wait for the monitor. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "display.h"

static HWND wnd;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *rtv;
static ID3D11Device *ext_dev;      /* the GPU renderer's device, shared (display_use_device) */
static int bw, bh;                 /* back buffer size */
static int px, py;                 /* where the picture sits in it */
static int cw = 640, ch = 480;     /* window client size */
static int focused, quit, f12, full;
static RECT saved_rect;
static LONG saved_style;

static void release_chain(void)
{
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (swap) IDXGISwapChain_Release(swap);
    if (dev) ID3D11Device_Release(dev);
    rtv = NULL; ctx = NULL; swap = NULL; dev = NULL;
    bw = bh = 0;
}

void display_use_device(void *d) { ext_dev = (ID3D11Device *)d; }

static int create_chain(void)
{
    DXGI_SWAP_CHAIN_DESC sd;
    IDXGIFactory *fac = NULL;
    HRESULT hr;
    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 448;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (ext_dev) {   /* the GPU renderer's device: a swap chain from its own adapter's factory */
        IDXGIDevice *dd = NULL;
        IDXGIAdapter *ad = NULL;
        hr = E_FAIL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(ext_dev, &IID_IDXGIDevice, (void **)&dd))) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad))) {
                if (SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fac))) {
                    hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)ext_dev, &sd, &swap);
                    if (SUCCEEDED(hr)) IDXGIFactory_MakeWindowAssociation(fac, wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                    IDXGIFactory_Release(fac);
                    fac = NULL;
                }
                IDXGIAdapter_Release(ad);
            }
            IDXGIDevice_Release(dd);
        }
        if (FAILED(hr)) { swap = NULL; return 0; }
        dev = ext_dev; ID3D11Device_AddRef(dev);
        ID3D11Device_GetImmediateContext(dev, &ctx);
        bw = 640; bh = 448;
        return 1;
    }
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &swap,
                                       &dev, NULL, &ctx);
    if (FAILED(hr))   /* no usable GPU driver: Windows' software rasterizer still shows the picture */
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &swap,
                                           &dev, NULL, &ctx);
    if (FAILED(hr)) { release_chain(); return 0; }
    /* our own Alt+Enter (borderless), not DXGI's exclusive full screen at the PS2 resolution */
    if (SUCCEEDED(IDXGISwapChain_GetParent(swap, &IID_IDXGIFactory, (void **)&fac))) {
        IDXGIFactory_MakeWindowAssociation(fac, wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        IDXGIFactory_Release(fac);
    }
    bw = 640; bh = 448;
    return 1;
}

/* Back buffer shaped like a cw x ch window (in picture pixels) with a w x h picture centred in it at the TV's 4:3. */
void display_layout(int w, int h, int cw_, int ch_, int *nw, int *nh, int *x, int *y)
{
    double par = (4.0 / 3.0) / ((double)w / h);   /* how wide one picture pixel is on the TV */
    double win = (double)(cw_ > 0 ? cw_ : 1) / (ch_ > 0 ? ch_ : 1);
    *nw = w; *nh = h;
    if ((double)h * win / par >= w) *nw = (int)((double)h * win / par + 0.5);
    else *nh = (int)((double)w * par / win + 0.5);
    if (*nw > 4096) *nw = 4096;
    if (*nh > 4096) *nh = 4096;
    *x = (*nw - w) / 2; *y = (*nh - h) / 2;
}

/* black back buffer, the picture copied into its place */
static void draw_into(ID3D11DeviceContext *c, ID3D11RenderTargetView *target, ID3D11Resource *tex,
                      const hw_u32 *rgba, int w, int h, int x, int y)
{
    static const float black[4] = { 0, 0, 0, 1 };
    D3D11_BOX box;
    ID3D11DeviceContext_ClearRenderTargetView(c, target, black);
    box.left = (UINT)x; box.top = (UINT)y; box.front = 0;
    box.right = (UINT)(x + w); box.bottom = (UINT)(y + h); box.back = 1;
    ID3D11DeviceContext_UpdateSubresource(c, tex, 0, &box, rgba, (UINT)w * 4, 0);
}

static int layout(int w, int h)
{
    int nw, nh;
    ID3D11Texture2D *back = NULL;
    display_layout(w, h, cw, ch, &nw, &nh, &px, &py);
    if (nw != bw || nh != bh || !rtv) {
        if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(swap, 1, (UINT)nw, (UINT)nh, DXGI_FORMAT_R8G8B8A8_UNORM, 0))) return 0;
        if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back))) return 0;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
        ID3D11Texture2D_Release(back);
        if (!rtv) return 0;
        bw = nw; bh = nh;
    }
    return 1;
}

static void toggle_full(void)
{
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetWindowRect(wnd, &saved_rect);
        saved_style = GetWindowLongW(wnd, GWL_STYLE);
        GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(wnd, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        full = 1;
    } else {
        SetWindowLongW(wnd, GWL_STYLE, saved_style);
        SetWindowPos(wnd, NULL, saved_rect.left, saved_rect.top, saved_rect.right - saved_rect.left,
                     saved_rect.bottom - saved_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        full = 0;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CLOSE: quit = 1; return 0;
    case WM_SETFOCUS: focused = 1; break;
    case WM_KILLFOCUS: focused = 0; break;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp)) { cw = LOWORD(lp); ch = HIWORD(lp); }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F12 && !(lp & 0x40000000)) f12 = 1;
        if (wp == VK_F11 && !(lp & 0x40000000)) toggle_full();
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && !(lp & 0x40000000)) { toggle_full(); return 0; }
        if (wp == VK_F10) return 0;
        break;
    case WM_SYSCHAR: if (wp == VK_RETURN) return 0; break;   /* no beep on Alt+Enter */
    default: break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int display_open(int scale)
{
    WNDCLASSW wc;
    RECT r, work;
    int s = scale > 0 ? scale : 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));    /* the release exe's icon (loader/CMakeLists.txt) */
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"DotRPCWindow";
    RegisterClassW(&wc);
    /* 4:3 like the TV, as large as asked but inside the screen's work area */
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    for (;; s--) {
        r.left = 0; r.top = 0; r.right = 640 * s; r.bottom = 480 * s;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (s <= 1 || (r.right - r.left <= work.right - work.left && r.bottom - r.top <= work.bottom - work.top)) break;
    }
    wnd = CreateWindowExW(0, wc.lpszClassName, L"Yu-Gi-Oh! The Duelists of the Roses - PC", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) return 0;
    /* title bar and taskbar: the icon's own small and large pictures, not one scaled */
    {
        HICON sm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        HICON big = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        if (sm) SendMessageW(wnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);
        if (big) SendMessageW(wnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    }
    GetClientRect(wnd, &r);
    if (r.right > 0 && r.bottom > 0) { cw = r.right; ch = r.bottom; }
    if (!create_chain()) { DestroyWindow(wnd); wnd = NULL; return 0; }
    focused = GetForegroundWindow() == wnd;
    return 1;
}

void display_close(void)
{
    release_chain();
    if (wnd) DestroyWindow(wnd);
    wnd = NULL;
}

void display_present(const hw_u32 *rgba, int w, int h)
{
    ID3D11Texture2D *back = NULL;
    HRESULT hr;
    if (!wnd || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    if (!swap && !create_chain()) return;   /* after a device loss: try again every frame */
    if (!layout(w, h)) return;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back))) return;
    draw_into(ctx, rtv, (ID3D11Resource *)back, rgba, w, h, px, py);
    ID3D11Texture2D_Release(back);
    hr = IDXGISwapChain_Present(swap, 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) release_chain();
}

void display_present_texture(void *texture, int w, int h)
{
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11Texture2D *back = NULL;
    D3D11_BOX box;
    HRESULT hr;
    if (!wnd || !texture || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    if (!swap && !create_chain()) return;
    if (!layout(w, h)) return;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back))) return;
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    box.left = 0; box.top = 0; box.front = 0; box.right = (UINT)w; box.bottom = (UINT)h; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)back, 0, (UINT)px, (UINT)py, 0,
                                              (ID3D11Resource *)texture, 0, &box);
    ID3D11Texture2D_Release(back);
    hr = IDXGISwapChain_Present(swap, 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) release_chain();
}

void display_pump(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

int display_focused(void) { return focused; }
int display_quit(void) { return quit; }
int display_take_f12(void) { int v = f12; f12 = 0; return v; }

double display_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* Headless check of the present path without a window: a device (GPU, else WARP), an off-screen render target
 * shaped as the back buffer for a cw x ch window, the same draw_into as the window, read back through a staging
 * texture. Returns 0 when every pixel is right, else a code saying what failed. */
int display_selftest(const hw_u32 *rgba, int w, int h, int cw_, int ch_, int *mismatches)
{
    ID3D11Device *d = NULL;
    ID3D11DeviceContext *c = NULL;
    ID3D11Texture2D *tex = NULL, *stage = NULL;
    ID3D11RenderTargetView *target = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    int nw, nh, x, y, i, j, bad = 0, rc = 0;
    *mismatches = 0;
    display_layout(w, h, cw_, ch_, &nw, &nh, &x, &y);
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d, NULL, &c)) &&
        FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d, NULL, &c)))
        return 1;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)nw; td.Height = (UINT)nh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(d, &td, NULL, &tex))) { rc = 2; goto out; }
    if (FAILED(ID3D11Device_CreateRenderTargetView(d, (ID3D11Resource *)tex, NULL, &target))) { rc = 3; goto out; }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(d, &td, NULL, &stage))) { rc = 4; goto out; }
    draw_into(c, target, (ID3D11Resource *)tex, rgba, w, h, x, y);
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)stage, (ID3D11Resource *)tex);
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m))) { rc = 5; goto out; }
    for (j = 0; j < nh; j++) {
        const hw_u32 *row = (const hw_u32 *)((const BYTE *)m.pData + (size_t)j * m.RowPitch);
        for (i = 0; i < nw; i++) {
            int in = i >= x && i < x + w && j >= y && j < y + h;
            hw_u32 want = in ? rgba[(j - y) * w + (i - x)] : 0xFF000000u;
            if (row[i] != want) bad++;
        }
    }
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)stage, 0);
    *mismatches = bad;
    if (bad) rc = 6;
out:
    if (stage) ID3D11Texture2D_Release(stage);
    if (target) ID3D11RenderTargetView_Release(target);
    if (tex) ID3D11Texture2D_Release(tex);
    if (c) ID3D11DeviceContext_Release(c);
    if (d) ID3D11Device_Release(d);
    return rc;
}
