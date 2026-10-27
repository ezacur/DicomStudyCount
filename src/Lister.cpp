// Total Commander's Lister (F3, Ctrl+Q): a Lister plugin (WLX) with the preview of Explorer's preview pane
#include "Shell.h"

// ---------------------------------------------------------------------------------------------
// This same DLL, copied as explorer4dicoms.wlx64 (tools\New-ListerPlugin.ps1), is the plugin: Total
// Commander loads it and asks it for a window of its own inside the Lister's for each DICOM file. That
// window holds a PreviewHandler as Explorer's preview pane does (its tabs, the image, the series of the
// folder), but with the columns as the values of the file (no check boxes: there is no Explorer window).
// The Lister calls nobody's TranslateAccelerator, as Explorer does for a preview: a hook on the messages
// of its thread does, and the keys the preview does not use go back to the Lister (Esc, N, P...).

#pragma comment(linker, "/EXPORT:ListLoad")
#pragma comment(linker, "/EXPORT:ListLoadW")
#pragma comment(linker, "/EXPORT:ListCloseWindow")
#pragma comment(linker, "/EXPORT:ListGetDetectString")

namespace
{
    // The windows of the plugin open in this thread, and the hook while there is any
    struct Open { HWND host; PreviewHandler* preview; };
    std::vector<Open> g_open;
    HHOOK g_hook = nullptr;

    // Its keys, before the Lister's loop translates and dispatches them
    LRESULT CALLBACK Messages(int code, WPARAM remove, LPARAM lp)
    {
        auto msg = reinterpret_cast<MSG*>(lp);
        if (code == HC_ACTION && remove == PM_REMOVE && msg && (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN))
            for (const Open& o : g_open)
                if (msg->hwnd == o.host || IsChild(o.host, msg->hwnd))
                {
                    if (o.preview->TranslateAccelerator(msg) == S_OK) msg->message = WM_NULL;
                    break;
                }
        return CallNextHookEx(g_hook, code, remove, lp);
    }

    // The window inside the Lister's: the preview over all of it
    LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto preview = reinterpret_cast<PreviewHandler*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_SIZE:
            if (preview)
            {
                RECT rc = { 0, 0, LOWORD(lp), HIWORD(lp) };
                preview->SetRect(&rc);
            }
            return 0;
        case WM_SETFOCUS:
            if (preview) preview->SetFocus();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            if (preview)
            {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                g_open.erase(std::remove_if(g_open.begin(), g_open.end(), [&](const Open& o) { return o.host == hwnd; }), g_open.end());
                if (g_open.empty() && g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
                preview->Unload();
                preview->Release();
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool FileLooksDicom(const wchar_t* file)
    {
        HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        unsigned char head[132] = {};
        DWORD got = 0;
        const bool read = ReadFile(h, head, sizeof(head), &got, nullptr) != FALSE;
        CloseHandle(h);
        return read && LooksDicom(head, got);
    }
}

// A window with the file in it, a child of the Lister's; null if it is not DICOM (the Lister tries the
// next plugin, or shows it as text)
extern "C" HWND __stdcall ListLoadW(HWND parent, wchar_t* file, int)
{
    if (!parent || !file || !FileLooksDicom(file)) return nullptr;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);     // (the Lister's thread has COM already, as a rule)
    static ATOM cls = 0;
    if (!cls)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = HostProc;
        wc.hInstance = g_module;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Explorer4DicomsLister";
        cls = RegisterClassExW(&wc);
        if (!cls) return nullptr;
    }
    RECT rc = {};
    GetClientRect(parent, &rc);
    HWND host = CreateWindowExW(0, L"Explorer4DicomsLister", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                0, 0, rc.right, rc.bottom, parent, nullptr, g_module, nullptr);
    if (!host) return nullptr;
    IShellItem* item = nullptr;
    auto preview = new (std::nothrow) PreviewHandler();
    bool ok = preview && SUCCEEDED(SHCreateItemFromParsingName(file, nullptr, IID_PPV_ARGS(&item)));
    if (ok)
    {
        preview->ForLister(parent);
        RECT inside = { 0, 0, rc.right, rc.bottom };
        ok = SUCCEEDED(preview->Initialize(item, STGM_READ)) && SUCCEEDED(preview->SetWindow(host, &inside)) &&
             SUCCEEDED(preview->DoPreview());
    }
    if (item) item->Release();
    if (!ok)
    {
        if (preview) { preview->Unload(); preview->Release(); }
        DestroyWindow(host);
        return nullptr;
    }
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(preview));
    g_open.push_back({ host, preview });
    if (!g_hook) g_hook = SetWindowsHookExW(WH_GETMESSAGE, Messages, nullptr, GetCurrentThreadId());
    return host;
}

extern "C" HWND __stdcall ListLoad(HWND parent, char* file, int flags)
{
    if (!file) return nullptr;
    const int n = MultiByteToWideChar(CP_ACP, 0, file, -1, nullptr, 0);
    std::wstring wide(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, file, -1, &wide[0], n);
    return ListLoadW(parent, &wide[0], flags);
}

extern "C" void __stdcall ListCloseWindow(HWND window)
{
    DestroyWindow(window);
}

// The files Total Commander offers it: by their extension, or "DICM" after the preamble
extern "C" void __stdcall ListGetDetectString(char* detect, int length)
{
    const char text[] = "EXT=\"DCM\" | EXT=\"DICOM\" | EXT=\"IMA\" | ([128]=\"D\" & [129]=\"I\" & [130]=\"C\" & [131]=\"M\")";
    if (detect && length > 0) strncpy_s(detect, static_cast<size_t>(length), text, _TRUNCATE);
}
