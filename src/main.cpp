#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES |
                     ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    std::wstring initialPath;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(commandLine, &argc)) {
        if (argc > 1 && argv[1]) {
            initialPath = argv[1];
        }
        LocalFree(argv);
    }

    const int result = RunApp(instance, initialPath);

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return result;
}
