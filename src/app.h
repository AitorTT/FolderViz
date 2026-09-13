#pragma once

#include <windows.h>

#include <string>

// Creates the main window, runs the message loop, and returns the exit code.
int RunApp(HINSTANCE instance, const std::wstring& initialPath);
