#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <cwchar>
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
    bool startCity = true;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(commandLine, &argc)) {
        // wWinMain's lpCmdLine excludes the program name, so argv[0] is the
        // first real argument.
        for (int i = 0; i < argc; ++i) {
            if (argv[i] && _wcsicmp(argv[i], L"--city") == 0) {
                startCity = true;
            } else if (argv[i] && _wcsicmp(argv[i], L"--treemap") == 0) {
                startCity = false;
            } else if (argv[i] && argv[i][0] != L'\0' && initialPath.empty()) {
                initialPath = argv[i];
            }
        }
        LocalFree(argv);
    }

    const int result = RunApp(instance, initialPath, startCity);

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return result;
}
