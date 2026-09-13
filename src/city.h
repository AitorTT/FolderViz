#pragma once

#include <windows.h>

#include "fsnode.h"

// Parent is notified when the user clicks (SELECT) or double-clicks (ACTIVATE)
// a building. lParam is the Node*.
constexpr UINT WM_APP_CITY_SELECT = WM_APP + 3;
constexpr UINT WM_APP_CITY_ACTIVATE = WM_APP + 4;

void RegisterCityClass(HINSTANCE instance);
HWND CreateCityWindow(HWND parent, HINSTANCE instance, int id);

// Sets the directory whose immediate subfolders are drawn as buildings.
void CitySetDirectory(HWND city, Node* dir);
void CitySetSelected(HWND city, Node* node);
