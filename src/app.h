#pragma once

#include <windows.h>

#include <string>

// Creates the main window, runs the message loop, and returns the exit code.
// The 3D city view is the default; pass startInCityView = false for the treemap.
int RunApp(HINSTANCE instance, const std::wstring& initialPath, bool startInCityView = true);
