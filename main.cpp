#include <windows.h>
#include <wrl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <ctype.h>
#include "WebView2.h"

using namespace Microsoft::WRL;

#pragma comment(lib, "WebView2LoaderStatic.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

static HWND g_hwnd = nullptr;
static ICoreWebView2Controller* g_controller = nullptr;
static WCHAR g_rootDir[MAX_PATH] = { 0 };
static WCHAR g_entryPath[MAX_PATH] = { 0 };
static WCHAR g_configDir[MAX_PATH] = { 0 };
static WCHAR g_title[MAX_PATH] = { 0 };
static WCHAR g_iconPath[MAX_PATH] = { 0 };
static WCHAR g_startUrl[MAX_PATH] = { 0 };
static WCHAR g_resourceDir[MAX_PATH] = { 0 };
static WCHAR g_cacheDir[MAX_PATH] = { 0 };
static WCHAR g_currentListingDir[MAX_PATH] = { 0 };
static BOOL g_darkMode = FALSE;
static BOOL g_showListing = FALSE;
static BOOL g_fullscreen = FALSE;
static BOOL g_is404 = FALSE;

// ---------- 纯 C 字符串与路径工具 ----------
static void GetExePath(WCHAR* outPath) {
    GetModuleFileNameW(nullptr, outPath, MAX_PATH);
}

static void GetExeDir(WCHAR* outDir) {
    GetExePath(outDir);
    WCHAR* p = wcsrchr(outDir, L'\\');
    if (p) *p = L'\0';
}

static void GetExeName(WCHAR* outName) {
    GetExePath(outName);
    WCHAR* p = wcsrchr(outName, L'\\');
    if (p) {
        wcscpy_s(outName, MAX_PATH, p + 1);
    }
    p = wcsrchr(outName, L'.');
    if (p) *p = L'\0';
}

static void PathCombineC(WCHAR* out, const WCHAR* dir, const WCHAR* rel) {
    wcscpy_s(out, MAX_PATH, dir);
    size_t len = wcslen(out);
    if (len > 0 && out[len - 1] != L'\\' && out[len - 1] != L'/') {
        wcscat_s(out, MAX_PATH, L"\\");
    }
    wcscat_s(out, MAX_PATH, rel);
}

static void ReplaceVariables(WCHAR* str) {
    WCHAR exeName[MAX_PATH] = { 0 };
    GetExeName(exeName);

    WCHAR* pos = wcsstr(str, L"{exe}");
    if (pos) {
        WCHAR temp[MAX_PATH] = { 0 };
        wcscpy_s(temp, MAX_PATH, pos + 5);
        wcscpy_s(pos, MAX_PATH - (pos - str), exeName);
        wcscat_s(str, MAX_PATH, temp);
    }
}

static unsigned int CalculatePathHash(const WCHAR* str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static void WideToAnsi(const WCHAR* wstr, char* outStr, int maxLen) {
    if (!wstr || !outStr) return;
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, outStr, maxLen, NULL, NULL);
}

// ---------- 全屏/窗口无缝切换 ----------
static void ToggleWindowMode(BOOL fullscreen) {
    if (g_fullscreen == fullscreen) return;
    g_fullscreen = fullscreen;

    if (g_fullscreen) {
        SetWindowLongPtr(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else {
        SetWindowLongPtr(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    }
}

// ---------- 纯 C 风格 URL 编码与解码 ----------
static void UrlEncode(const WCHAR* input, WCHAR* output, size_t maxLen) {
    char utf8Buf[MAX_PATH * 4] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, input, -1, utf8Buf, sizeof(utf8Buf), NULL, NULL);

    WCHAR* p = output;
    for (int i = 0; utf8Buf[i] != '\0'; i++) {
        unsigned char c = (unsigned char)utf8Buf[i];
        if (c == '\\') {
            if ((size_t)(p - output) < maxLen - 1) *p++ = L'/';
        }
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            if ((size_t)(p - output) < maxLen - 1) *p++ = (WCHAR)c;
        }
        else {
            if ((size_t)(p - output) < maxLen - 4) {
                wchar_t hex[4];
                swprintf_s(hex, L"%%%02X", c);
                *p++ = hex[0];
                *p++ = hex[1];
                *p++ = hex[2];
            }
        }
    }
    *p = L'\0';
}

static void UrlDecode(const WCHAR* input, WCHAR* output, size_t maxLen) {
    WCHAR* p = output;
    for (int i = 0; input[i] != L'\0' && (size_t)(p - output) < maxLen - 1; i++) {
        if (input[i] == L'%' && iswxdigit(input[i + 1]) && iswxdigit(input[i + 2])) {
            wchar_t hex[3] = { input[i + 1], input[i + 2], 0 };
            *p++ = (WCHAR)wcstol(hex, NULL, 16);
            i += 2;
        }
        else if (input[i] == L'+') {
            *p++ = L' ';
        }
        else {
            *p++ = input[i];
        }
    }
    *p = L'\0';
}

// ---------- 终极兼容 INI 解析 ----------
static void ReadIniString(const WCHAR* section, const WCHAR* key, const WCHAR* defaultVal, const WCHAR* configPath, WCHAR* outVal) {
    char secA[128] = { 0 };
    char keyA[128] = { 0 };
    char pathA[MAX_PATH * 2] = { 0 };

    WideToAnsi(section, secA, 128);
    WideToAnsi(key, keyA, 128);
    WideToAnsi(configPath, pathA, MAX_PATH * 2);

    char buffer[MAX_PATH * 4] = { 0 };
    GetPrivateProfileStringA(secA, keyA, "", buffer, MAX_PATH * 4, pathA);

    if (strlen(buffer) == 0) {
        wcscpy_s(outVal, MAX_PATH, defaultVal);
    }
    else {
        int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buffer, -1, NULL, 0);
        if (wlen > 0) {
            MultiByteToWideChar(CP_UTF8, 0, buffer, -1, outVal, MAX_PATH);
        }
        else {
            MultiByteToWideChar(CP_ACP, 0, buffer, -1, outVal, MAX_PATH);
        }
    }
    ReplaceVariables(outVal);
}

static BOOL ReadIniInt(const WCHAR* section, const WCHAR* key, BOOL defaultVal, const WCHAR* configPath) {
    char secA[128] = { 0 };
    char keyA[128] = { 0 };
    char pathA[MAX_PATH * 2] = { 0 };
    WideToAnsi(section, secA, 128);
    WideToAnsi(key, keyA, 128);
    WideToAnsi(configPath, pathA, MAX_PATH * 2);
    return GetPrivateProfileIntA(secA, keyA, defaultVal ? 1 : 0, pathA) != 0;
}

static void LoadConfig(const WCHAR* configPath) {
    WCHAR* p = wcsrchr((WCHAR*)configPath, L'\\');
    if (p) {
        wcsncpy_s(g_configDir, MAX_PATH, configPath, p - configPath);
    }
    else {
        wcscpy_s(g_configDir, MAX_PATH, L".");
    }

    ReadIniString(L"App", L"Title", L"", configPath, g_title);
    if (wcslen(g_title) == 0) GetExeName(g_title);

    g_darkMode = ReadIniInt(L"App", L"DarkMode", FALSE, configPath);
    g_fullscreen = ReadIniInt(L"App", L"Fullscreen", FALSE, configPath);

    ReadIniString(L"App", L"Icon", L"", configPath, g_iconPath);
    ReadIniString(L"App", L"StartUrl", L"", configPath, g_startUrl);
    if (wcslen(g_startUrl) == 0) {
        wcscpy_s(g_startUrl, MAX_PATH, L"index.html | {exe}.html | showlist");
    }

    ReadIniString(L"App", L"ResourceDir", L"", configPath, g_resourceDir);
    ReadIniString(L"App", L"CacheDir", L"", configPath, g_cacheDir);
}

static BOOL IsRegularFile(const WCHAR* path) {
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------- 目标解析 ----------
static void ResolveTarget(const WCHAR* arg, BOOL hasArg) {
    WCHAR exeDir[MAX_PATH] = { 0 };
    GetExeDir(exeDir);
    g_showListing = FALSE;
    g_is404 = FALSE;

    if (hasArg && PathIsDirectoryW(arg)) {
        wcscpy_s(g_rootDir, MAX_PATH, arg);
    }
    else if (wcslen(g_resourceDir) > 0) {
        if (PathIsRelativeW(g_resourceDir)) {
            PathCombineC(g_rootDir, g_configDir, g_resourceDir);
        }
        else {
            wcscpy_s(g_rootDir, MAX_PATH, g_resourceDir);
        }
    }
    else {
        wcscpy_s(g_rootDir, MAX_PATH, wcslen(g_configDir) > 0 ? g_configDir : exeDir);
    }

    if (hasArg && IsRegularFile(arg)) {
        WCHAR* p = wcsrchr((WCHAR*)arg, L'\\');
        if (p) {
            wcsncpy_s(g_rootDir, MAX_PATH, arg, p - arg);
            wcscpy_s(g_entryPath, MAX_PATH, p + 1);
        }
        return;
    }

    WCHAR startUrlCopy[MAX_PATH * 2] = { 0 };
    wcscpy_s(startUrlCopy, MAX_PATH * 2, g_startUrl);

    WCHAR* nextToken = nullptr;
    WCHAR* token = wcstok_s(startUrlCopy, L"|", &nextToken);
    WCHAR firstAttemptPath[MAX_PATH] = { 0 };

    while (token != nullptr) {
        while (*token == L' ' || *token == L'\t') token++;
        WCHAR* end = token + wcslen(token) - 1;
        while (end > token && (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n')) {
            *end = L'\0';
            end--;
        }

        if (wcslen(token) > 0) {
            if (_wcsicmp(token, L"showlist") == 0) {
                g_showListing = TRUE;
                wcscpy_s(g_currentListingDir, MAX_PATH, g_rootDir);
                return;
            }

            if (wcsstr(token, L"http") == token) {
                wcscpy_s(g_entryPath, MAX_PATH, token);
                return;
            }

            WCHAR testPath[MAX_PATH] = { 0 };
            PathCombineC(testPath, g_rootDir, token);

            if (wcslen(firstAttemptPath) == 0) {
                wcscpy_s(firstAttemptPath, MAX_PATH, token);
            }

            if (PathFileExistsW(testPath) && IsRegularFile(testPath)) {
                wcscpy_s(g_entryPath, MAX_PATH, token);
                return;
            }
        }
        token = wcstok_s(nullptr, L"|", &nextToken);
    }

    g_is404 = TRUE;
    if (wcslen(firstAttemptPath) > 0) {
        wcscpy_s(g_entryPath, MAX_PATH, firstAttemptPath);
    }
    else {
        wcscpy_s(g_entryPath, MAX_PATH, L"force_404_page.html");
    }
}

// ---------- 纯 C 风格 HTML 拼接 ----------
static void GenerateDirectoryListingHtml(const WCHAR* dir, WCHAR* outHtml, size_t maxLen) {
    WCHAR* p = outHtml;
    p += swprintf_s(p, maxLen - (p - outHtml), L"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>目录浏览</title>");

    WCHAR relDir[MAX_PATH] = { 0 };
    size_t rootLen = wcslen(g_rootDir);
    size_t dirLen = wcslen(dir);
    if (dirLen > rootLen) {
        const WCHAR* rel = dir + rootLen;
        while (*rel == L'\\' || *rel == L'/') rel++;
        if (wcslen(rel) > 0) {
            wcscpy_s(relDir, MAX_PATH, rel);
            for (int i = 0; relDir[i]; i++) if (relDir[i] == L'\\') relDir[i] = L'/';
            wcscat_s(relDir, MAX_PATH, L"/");
        }
    }
    p += swprintf_s(p, maxLen - (p - outHtml), L"<base href=\"https://app.local/%s\">", relDir);

    p += swprintf_s(p, maxLen - (p - outHtml), L"<style>");
    if (g_darkMode) {
        p += swprintf_s(p, maxLen - (p - outHtml), L"body { font-family: 'Segoe UI', sans-serif; background: #1e1e1e; color: #ccc; padding: 30px; } h2 { color: #fff; border-bottom: 2px solid #333; } li { padding: 10px 0; border-bottom: 1px solid #333; } a { text-decoration: none; color: #4daafc; font-size: 18px; display: block; } .dir { color: #e8bf6a; font-weight: bold; }");
    }
    else {
        p += swprintf_s(p, maxLen - (p - outHtml), L"body { font-family: 'Segoe UI', sans-serif; background: #fff; color: #333; padding: 30px; } h2 { color: #000; border-bottom: 2px solid #eee; } li { padding: 10px 0; border-bottom: 1px solid #eee; } a { text-decoration: none; color: #0066cc; font-size: 18px; display: block; } .dir { color: #d97706; font-weight: bold; }");
    }
    p += swprintf_s(p, maxLen - (p - outHtml), L"</style></head><body><h2>📁 %s</h2><ul>", dir);

    WCHAR parentDir[MAX_PATH] = { 0 };
    wcscpy_s(parentDir, MAX_PATH, dir);
    WCHAR* lastSlash = wcsrchr(parentDir, L'\\');
    if (lastSlash && wcslen(dir) > wcslen(g_rootDir)) {
        *lastSlash = L'\0';
        p += swprintf_s(p, maxLen - (p - outHtml), L"<li><a href=\"#\" class=\"dir\" onclick=\"window.location.href='https://app.local/?browse_folder=..'\">↩️ 返回上一级</a></li>");
    }

    WCHAR searchPath[MAX_PATH] = { 0 };
    PathCombineC(searchPath, dir, L"*");

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                WCHAR encodedName[MAX_PATH * 4] = { 0 };
                UrlEncode(fd.cFileName, encodedName, MAX_PATH * 4);
                p += swprintf_s(p, maxLen - (p - outHtml), L"<li><a href=\"#\" class=\"dir\" onclick=\"window.location.href='https://app.local/?browse_folder=%s'\">📁 %s/</a></li>", encodedName, fd.cFileName);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    hFind = FindFirstFileW(searchPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                WCHAR encodedName[MAX_PATH * 4] = { 0 };
                UrlEncode(fd.cFileName, encodedName, MAX_PATH * 4);
                p += swprintf_s(p, maxLen - (p - outHtml), L"<li><a href=\"%s\">📄 %s</a></li>", encodedName, fd.cFileName);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    p += swprintf_s(p, maxLen - (p - outHtml), L"</ul></body></html>");
}

static void NavigateToEntry(ICoreWebView2* webview) {
    ComPtr<ICoreWebView2_3> webview3;
    BOOL mapped = FALSE;

    if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview3)))) {
        HRESULT hr = webview3->SetVirtualHostNameToFolderMapping(L"app.local", g_rootDir, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        if (SUCCEEDED(hr)) mapped = TRUE;
    }

    if (g_showListing) {
        static WCHAR htmlBuffer[1024 * 128];
        GenerateDirectoryListingHtml(g_currentListingDir, htmlBuffer, 131072);
        webview->NavigateToString(htmlBuffer);
        return;
    }

    if (mapped) {
        WCHAR url[MAX_PATH * 2] = { 0 };
        if (wcsstr(g_entryPath, L"http") == g_entryPath) {
            webview->Navigate(g_entryPath);
        }
        else {
            swprintf_s(url, L"https://app.local/%s", g_entryPath);
            for (int i = 0; url[i]; i++) if (url[i] == L'\\') url[i] = L'/';
            webview->Navigate(url);
        }
        return;
    }
}

// ---------- 拦截浏览子目录 ----------
class NavStartingHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2NavigationStartingEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) override {
        LPWSTR rawUri = nullptr;
        args->get_Uri(&rawUri);

        WCHAR prefix[] = L"https://app.local/?browse_folder=";
        if (wcsstr(rawUri, prefix) == rawUri) {
            args->put_Cancel(TRUE);
            WCHAR* folderNameEncoded = rawUri + wcslen(prefix);
            WCHAR folderName[MAX_PATH] = { 0 };
            UrlDecode(folderNameEncoded, folderName, MAX_PATH);

            if (wcscmp(folderName, L"..") == 0) {
                WCHAR* lastSlash = wcsrchr(g_currentListingDir, L'\\');
                if (lastSlash && wcslen(g_currentListingDir) > wcslen(g_rootDir)) *lastSlash = L'\0';
            }
            else {
                PathCombineC(g_currentListingDir, g_currentListingDir, folderName);
            }

            static WCHAR htmlBuffer[1024 * 128];
            GenerateDirectoryListingHtml(g_currentListingDir, htmlBuffer, 131072);
            sender->NavigateToString(htmlBuffer);
        }
        CoTaskMemFree(rawUri);
        return S_OK;
    }
};

// ---------- 快捷键拦截（F1 切换全屏/窗口，长按 ESC 切回窗口） ----------
class AccelKeyHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2AcceleratorKeyPressedEventHandler> {
private:
    ULONGLONG m_escPressTime = 0;
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller* sender, ICoreWebView2AcceleratorKeyPressedEventArgs* args) override {
        COREWEBVIEW2_KEY_EVENT_KIND kind;
        args->get_KeyEventKind(&kind);
        UINT32 key;
        args->get_VirtualKey(&key);

        // 1. F1 键：自由切换全屏/窗口模式
        if (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && key == VK_F1) {
            args->put_Handled(TRUE);
            ToggleWindowMode(!g_fullscreen);
            return S_OK;
        }

        // 2. ESC 键：单按放行，长按切回普通窗口
        if (key == VK_ESCAPE) {
            if (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) {
                if (m_escPressTime == 0) {
                    m_escPressTime = GetTickCount64();
                }
                else {
                    args->put_Handled(TRUE);
                    if (GetTickCount64() - m_escPressTime > 1500) {
                        // 如果长按，不杀进程，而是切回窗口模式
                        if (g_fullscreen) {
                            ToggleWindowMode(FALSE);
                        }
                        else {
                            // 如果已经是窗口模式，再按 ESC 可能想退出了，这里按需选择：是彻底退出，还是不做反应。为了安全，退全屏就足够了。这里我们选择彻底退出（为了兜底）。
                            PostQuitMessage(0);
                        }
                    }
                }
            }
            else if (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP) {
                m_escPressTime = 0;
            }
        }
        return S_OK;
    }
};

// ---------- 页面加载完成事件：注入 JS ----------
class NavCompletedHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2NavigationCompletedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) override {
        BOOL isSuccess = FALSE;
        args->get_IsSuccess(&isSuccess);

        if (isSuccess) {
            sender->ExecuteScript(
                L"window.addEventListener('mousedown', function(e) {"
                L"  if (e.clientX > window.innerWidth * 0.95 && e.clientY < window.innerHeight * 0.05) {"
                L"    if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage('toggle');" // 改成 toggle
                L"  }"
                L"});", nullptr);
        }
        return S_OK;
    }
};

class Handler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        if (FAILED(result) || !env) { PostQuitMessage(1); return result; }
        return env->CreateCoreWebView2Controller(g_hwnd, this);
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        if (FAILED(result) || !controller) { PostQuitMessage(1); return result; }
        g_controller = controller;
        g_controller->AddRef();
        RECT rc; GetClientRect(g_hwnd, &rc);
        g_controller->put_Bounds(rc);

        ICoreWebView2* webview = nullptr;
        g_controller->get_CoreWebView2(&webview);
        if (webview) {
            if (g_darkMode) {
                ComPtr<ICoreWebView2_13> webview13;
                if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview13)))) {
                    ComPtr<ICoreWebView2Profile> profile;
                    if (SUCCEEDED(webview13->get_Profile(&profile))) {
                        profile->put_PreferredColorScheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK);
                    }
                }
            }

            EventRegistrationToken navToken;
            auto navHandler = Make<NavStartingHandler>();
            webview->add_NavigationStarting(navHandler.Get(), &navToken);

            EventRegistrationToken keyToken;
            auto keyHandler = Make<AccelKeyHandler>();
            controller->add_AcceleratorKeyPressed(keyHandler.Get(), &keyToken);

            EventRegistrationToken compToken;
            auto compHandler = Make<NavCompletedHandler>();
            webview->add_NavigationCompleted(compHandler.Get(), &compToken);

            EventRegistrationToken msgToken;
            webview->add_WebMessageReceived(
                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                        LPWSTR message = nullptr;
                        args->TryGetWebMessageAsString(&message);
                        if (message) {
                            if (wcscmp(message, L"exit") == 0) {
                                PostQuitMessage(0);
                            }
                            else if (wcscmp(message, L"toggle") == 0) {
                                // 网页触发右上角点击时，切换窗口模式
                                ToggleWindowMode(!g_fullscreen);
                            }
                            CoTaskMemFree(message);
                        }
                        return S_OK;
                    }).Get(), &msgToken);

            NavigateToEntry(webview);
            webview->Release();
        }
        return S_OK;
    }
};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_controller) { RECT rc; GetClientRect(hwnd, &rc); g_controller->put_Bounds(rc); }
        return 0;
    case WM_DESTROY:
        if (g_controller) { g_controller->Release(); g_controller = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    WCHAR configPath[MAX_PATH] = { 0 };
    GetExeDir(configPath);
    wcscat_s(configPath, MAX_PATH, L"\\config.ini");

    WCHAR target[MAX_PATH] = { 0 };

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--config") == 0 || _wcsicmp(argv[i], L"-c") == 0) {
            if (i + 1 < argc) {
                wcscpy_s(configPath, MAX_PATH, argv[i + 1]);
                i++;
            }
        }
    }

    LoadConfig(configPath);

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--fullscreen") == 0 || _wcsicmp(argv[i], L"-f") == 0) {
            g_fullscreen = TRUE;
        }
        else if (_wcsicmp(argv[i], L"--config") == 0 || _wcsicmp(argv[i], L"-c") == 0) {
            i++;
        }
        else if (wcslen(target) == 0) {
            wcscpy_s(target, MAX_PATH, argv[i]);
        }
    }

    ResolveTarget(target, wcslen(target) > 0);
    LocalFree(argv);

    if (g_is404) {
        g_fullscreen = FALSE;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WebViewHost";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    HICON hIcon = nullptr;
    if (wcslen(g_iconPath) > 0) {
        WCHAR fullIconPath[MAX_PATH] = { 0 };
        if (PathIsRelativeW(g_iconPath)) PathCombineC(fullIconPath, g_configDir, g_iconPath);
        else wcscpy_s(fullIconPath, MAX_PATH, g_iconPath);
        hIcon = (HICON)LoadImageW(nullptr, fullIconPath, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    if (!hIcon) {
        WCHAR exePath[MAX_PATH] = { 0 };
        GetExePath(exePath);
        hIcon = ExtractIconW(hInst, exePath, 0);
    }

    if (hIcon && hIcon != (HICON)1) {
        wc.hIcon = hIcon; wc.hIconSm = hIcon;
    }
    RegisterClassExW(&wc);

    DWORD style = g_fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    g_hwnd = CreateWindowExW(0, L"WebViewHost", g_title, style, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, hInst, nullptr);

    if (g_darkMode) {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));
    }

    if (g_fullscreen) {
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_SHOWWINDOW);
    }
    else {
        ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    }

    WCHAR userDataFolder[MAX_PATH] = { 0 };
    if (wcslen(g_cacheDir) > 0) {
        wcscpy_s(userDataFolder, MAX_PATH, g_cacheDir);
        CreateDirectoryW(userDataFolder, nullptr);
    }
    else {
        WCHAR tempPath[MAX_PATH] = { 0 };
        GetTempPathW(MAX_PATH, tempPath);

        size_t len = wcslen(tempPath);
        if (len > 0 && tempPath[len - 1] != L'\\') {
            wcscat_s(tempPath, MAX_PATH, L"\\");
        }

        WCHAR exeDir[MAX_PATH] = { 0 };
        GetExeDir(exeDir);
        unsigned int hash = CalculatePathHash(exeDir);

        swprintf_s(userDataFolder, MAX_PATH, L"%lsUniversalWebView_Cache_%u", tempPath, hash);
        CreateDirectoryW(userDataFolder, nullptr);
    }

    auto handler = Make<Handler>();
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder, nullptr, handler.Get());

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }

    CoUninitialize();
    return 0;
}