#include "city.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

// GDI+ headers reference unqualified min/max; NOMINMAX leaves them in std.
using std::max;
using std::min;

#include <objidl.h>
#include <gdiplus.h>

#include "filetype.h"
#include "format.h"

namespace {

constexpr wchar_t kCityClass[] = L"FolderVizCity";
constexpr float kPi = 3.14159265358979323846f;
constexpr float kNear = 0.05f;
constexpr int kSuperSample = 2;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 Normalize(Vec3 v) {
    const float len = std::sqrt(Dot(v, v));
    if (len < 1e-6f) {
        return {0.0f, 0.0f, 0.0f};
    }
    return {v.x / len, v.y / len, v.z / len};
}

int DpiForWindow(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return fn ? static_cast<int>(fn(hwnd)) : 96;
}

COLORREF Shade(COLORREF color, float factor) {
    int r = static_cast<int>(GetRValue(color) * factor);
    int g = static_cast<int>(GetGValue(color) * factor);
    int b = static_cast<int>(GetBValue(color) * factor);
    r = std::min(255, std::max(0, r));
    g = std::min(255, std::max(0, g));
    b = std::min(255, std::max(0, b));
    return RGB(r, g, b);
}

COLORREF Blend(COLORREF a, COLORREF b, float t) {
    auto mix = [t](int x, int y) {
        return std::min(255, std::max(0, static_cast<int>(x * (1.0f - t) + y * t)));
    };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}

bool PointInPoly(POINT p, const POINT* pts, int count) {
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const bool crosses = (pts[i].y > p.y) != (pts[j].y > p.y);
        if (crosses) {
            const double x = static_cast<double>(pts[j].x - pts[i].x) *
                                 static_cast<double>(p.y - pts[i].y) /
                                 static_cast<double>(pts[j].y - pts[i].y) +
                             static_cast<double>(pts[i].x);
            if (static_cast<double>(p.x) < x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

struct Building {
    Node* node = nullptr;
    float size = 0.0f;
    float cx = 0.0f;
    float cz = 0.0f;
    float hw = 0.0f;
    float hd = 0.0f;
    float height = 0.0f;
};

struct ScreenFace {
    POINT pts[4]{};
    Node* node = nullptr;
    float depth = 0.0f;
};

struct Camera {
    float azimuth = 0.7f;
    float elevation = 0.5f;
    float zoom = 1.0f;
};

struct CityState {
    Node* dir = nullptr;
    Node* selected = nullptr;
    Node* hover = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    Camera camera;
    bool dragging = false;
    bool moved = false;
    bool tracking = false;
    POINT last{};
    POINT down{};
    std::vector<ScreenFace> faces;
    std::vector<std::array<POINT, 4>> glowSelected;
    std::vector<std::array<POINT, 4>> glowHover;
    std::unordered_map<COLORREF, HBRUSH> brushes;
    std::unordered_map<COLORREF, HPEN> pens;
};

HBRUSH GetBrush(CityState* state, COLORREF color) {
    auto it = state->brushes.find(color);
    if (it != state->brushes.end()) {
        return it->second;
    }
    HBRUSH brush = CreateSolidBrush(color);
    state->brushes.emplace(color, brush);
    return brush;
}

HPEN GetPen(CityState* state, COLORREF color) {
    auto it = state->pens.find(color);
    if (it != state->pens.end()) {
        return it->second;
    }
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    state->pens.emplace(color, pen);
    return pen;
}

Node* Pick(CityState* state, POINT point) {
    // Face polygons are stored in the supersampled buffer's coordinate space.
    const POINT scaled{point.x * kSuperSample, point.y * kSuperSample};
    std::vector<const ScreenFace*> sorted;
    sorted.reserve(state->faces.size());
    for (const ScreenFace& face : state->faces) {
        sorted.push_back(&face);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const ScreenFace* a, const ScreenFace* b) { return a->depth < b->depth; });
    for (const ScreenFace* face : sorted) {
        if (PointInPoly(scaled, face->pts, 4)) {
            return face->node;
        }
    }
    return nullptr;
}

void DrawLabel(HDC dc, Node* node, POINT anchor) {
    std::vector<std::wstring> lines;
    lines.push_back(node->name);
    lines.push_back(FormatSize(node->size));
    if (node->isDir) {
        lines.push_back(std::to_wstring(node->fileCount) + L" files, " +
                        std::to_wstring(node->dirCount) + L" folders");
    }

    std::vector<SIZE> sizes(lines.size());
    int maxWidth = 0;
    int totalHeight = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        GetTextExtentPoint32W(dc, lines[i].c_str(), static_cast<int>(lines[i].size()),
                              &sizes[i]);
        maxWidth = std::max<int>(maxWidth, sizes[i].cx);
        totalHeight += sizes[i].cy;
    }

    const int gap = 2;
    const int padX = 6;
    const int padY = 4;
    totalHeight += static_cast<int>(lines.size() - 1) * gap;
    const int boxWidth = maxWidth + padX * 2;
    const int boxHeight = totalHeight + padY * 2;
    RECT box{anchor.x - boxWidth / 2, anchor.y - boxHeight / 2,
             anchor.x + boxWidth / 2, anchor.y + boxHeight / 2};

    const int oldMode = SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(255, 255, 240));
    SetTextColor(dc, RGB(30, 30, 30));

    int y = box.top + padY;
    for (size_t i = 0; i < lines.size(); ++i) {
        RECT lineRect{box.left, y, box.right, y + sizes[i].cy + gap};
        DrawTextW(dc, lines[i].c_str(), -1, &lineRect,
                  DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        y += sizes[i].cy + gap;
    }

    SetBkMode(dc, oldMode);
}

// Soft glow drawn with GDI+ (alpha-blended strokes). GDI pens are opaque and
// can only produce a hard outline, hence the switch.
void DrawGlowFaces(Gdiplus::Graphics& graphics,
                   const std::vector<std::array<POINT, 4>>& faces,
                   const Gdiplus::Color& color, int scale, bool strong) {
    const int maxWidth = (strong ? 14 : 8) * scale;
    for (const auto& face : faces) {
        Gdiplus::Point pts[4] = {Gdiplus::Point(face[0].x, face[0].y),
                                 Gdiplus::Point(face[1].x, face[1].y),
                                 Gdiplus::Point(face[2].x, face[2].y),
                                 Gdiplus::Point(face[3].x, face[3].y)};
        for (int width = maxWidth; width >= 1; --width) {
            const double t = 1.0 - static_cast<double>(width) / static_cast<double>(maxWidth);
            const BYTE alpha =
                static_cast<BYTE>(std::max(4.0, (strong ? 90.0 : 55.0) * t * t));
            Gdiplus::Pen pen(Gdiplus::Color(alpha, color.GetR(), color.GetG(),
                                            color.GetB()),
                             static_cast<Gdiplus::REAL>(width));
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            graphics.DrawPolygon(&pen, pts, 4);
        }
        Gdiplus::Pen core(Gdiplus::Color(255, 255, 250, 220),
                          static_cast<Gdiplus::REAL>(1.4f * scale));
        graphics.DrawPolygon(&core, pts, 4);
    }
}

void DrawGlowGdiPlus(HDC dc, const CityState* state) {
    if (state->glowSelected.empty() && state->glowHover.empty()) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (!state->glowHover.empty()) {
        DrawGlowFaces(graphics, state->glowHover, Gdiplus::Color(255, 210, 225, 255),
                      kSuperSample, false);
    }
    if (!state->glowSelected.empty()) {
        DrawGlowFaces(graphics, state->glowSelected, Gdiplus::Color(255, 255, 220, 130),
                      kSuperSample, true);
    }
    graphics.Flush();
}

void Render(CityState* state, HDC target, int width, int height) {
    const int sw = width * kSuperSample;
    const int sh = height * kSuperSample;

    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, sw, sh);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(dc, bitmap));
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, state->font));

    RECT client{0, 0, sw, sh};
    HBRUSH background = CreateSolidBrush(RGB(22, 24, 30));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(214, 218, 226));

    state->faces.clear();
    state->glowSelected.clear();
    state->glowHover.clear();
    Node* labelNode = nullptr;
    POINT labelAnchor{};

    std::vector<Building> buildings;
    float maxSize = 0.0f;
    if (state->dir) {
        for (auto& child : state->dir->children) {
            Building building;
            building.node = child.get();
            building.size = static_cast<float>(child->size);
            buildings.push_back(building);
            maxSize = std::max(maxSize, building.size);
        }
    }

    const int count = static_cast<int>(buildings.size());
    if (count == 0) {
        DrawTextW(dc, L"No items to display", -1, &client,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
        if (cols < 1) {
            cols = 1;
        }
        const int rows = (count + cols - 1) / cols;
        const float spacing = 1.0f;
        const float halfFootprint = 0.36f;
        const float maxWorldHeight = 2.6f;
        if (maxSize <= 0.0f) {
            maxSize = 1.0f;
        }

        for (int i = 0; i < count; ++i) {
            const int col = i % cols;
            const int row = i / cols;
            Building& b = buildings[static_cast<size_t>(i)];
            b.cx = (static_cast<float>(col) - static_cast<float>(cols - 1) * 0.5f) * spacing;
            b.cz = (static_cast<float>(row) - static_cast<float>(rows - 1) * 0.5f) * spacing;
            b.hw = halfFootprint;
            b.hd = halfFootprint;
            b.height = 0.06f + maxWorldHeight * (b.size / maxSize);
        }

        const float sceneRadius =
            0.5f * std::sqrt(static_cast<float>(cols * cols + rows * rows)) * spacing + 1.0f;

        const float dist = sceneRadius * 2.4f * state->camera.zoom;
        const Vec3 focus{0.0f, maxWorldHeight * 0.25f, 0.0f};
        const float ce = std::cos(state->camera.elevation);
        const float se = std::sin(state->camera.elevation);
        const Vec3 dirToEye{ce * std::sin(state->camera.azimuth), se,
                            ce * std::cos(state->camera.azimuth)};
        const Vec3 eye{focus.x + dirToEye.x * dist, focus.y + dirToEye.y * dist,
                       focus.z + dirToEye.z * dist};

        const Vec3 worldUp{0.0f, 1.0f, 0.0f};
        const Vec3 fwd = Normalize(Sub(focus, eye));
        const Vec3 right = Normalize(Cross(fwd, worldUp));
        const Vec3 up = Cross(right, fwd);
        const float focal = 1.0f / std::tan(0.5f * 50.0f * kPi / 180.0f);
        const float aspect = static_cast<float>(sw) / static_cast<float>(sh);

        auto project = [&](Vec3 p, POINT& out, float& depth) -> bool {
            const Vec3 d = Sub(p, eye);
            const float z = Dot(d, fwd);
            if (z < kNear) {
                return false;
            }
            const float invz = 1.0f / z;
            const float ndcX = focal * Dot(d, right) * invz / aspect;
            const float ndcY = focal * Dot(d, up) * invz;
            out.x = static_cast<LONG>((ndcX * 0.5f + 0.5f) * static_cast<float>(sw));
            out.y = static_cast<LONG>(
                (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(sh));
            depth = z;
            return true;
        };

        // Ground grid.
        const int gridExtent = std::max(cols, rows) / 2 + 2;
        HPEN gridPen = CreatePen(PS_SOLID, kSuperSample, RGB(52, 56, 66));
        HGDIOBJ oldPen = SelectObject(dc, gridPen);
        for (int g = -gridExtent; g <= gridExtent; ++g) {
            POINT a{};
            POINT b{};
            float da = 0.0f;
            float db = 0.0f;
            if (project({static_cast<float>(g), 0.0f, -static_cast<float>(gridExtent)}, a, da) &&
                project({static_cast<float>(g), 0.0f, static_cast<float>(gridExtent)}, b, db)) {
                MoveToEx(dc, a.x, a.y, nullptr);
                LineTo(dc, b.x, b.y);
            }
            if (project({-static_cast<float>(gridExtent), 0.0f, static_cast<float>(g)}, a, da) &&
                project({static_cast<float>(gridExtent), 0.0f, static_cast<float>(g)}, b, db)) {
                MoveToEx(dc, a.x, a.y, nullptr);
                LineTo(dc, b.x, b.y);
            }
        }
        SelectObject(dc, oldPen);
        DeleteObject(gridPen);

        // Painter's algorithm: draw far buildings first.
        std::vector<int> order(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            order[static_cast<size_t>(i)] = i;
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const Building& ba = buildings[static_cast<size_t>(a)];
            const Building& bb = buildings[static_cast<size_t>(b)];
            const float da = (ba.cx - eye.x) * (ba.cx - eye.x) +
                             (ba.height * 0.5f - eye.y) * (ba.height * 0.5f - eye.y) +
                             (ba.cz - eye.z) * (ba.cz - eye.z);
            const float db = (bb.cx - eye.x) * (bb.cx - eye.x) +
                             (bb.height * 0.5f - eye.y) * (bb.height * 0.5f - eye.y) +
                             (bb.cz - eye.z) * (bb.cz - eye.z);
            return da > db;
        });

        const Vec3 light = Normalize({-0.5f, 0.85f, 0.35f});
        const int faceIndex[5][4] = {{4, 5, 6, 7},
                                     {0, 1, 5, 4},
                                     {1, 2, 6, 5},
                                     {2, 3, 7, 6},
                                     {3, 0, 4, 7}};

        for (int index : order) {
            const Building& b = buildings[static_cast<size_t>(index)];
            const Vec3 v[8] = {
                {b.cx - b.hw, 0.0f, b.cz - b.hd}, {b.cx + b.hw, 0.0f, b.cz - b.hd},
                {b.cx + b.hw, 0.0f, b.cz + b.hd}, {b.cx - b.hw, 0.0f, b.cz + b.hd},
                {b.cx - b.hw, b.height, b.cz - b.hd},
                {b.cx + b.hw, b.height, b.cz - b.hd},
                {b.cx + b.hw, b.height, b.cz + b.hd},
                {b.cx - b.hw, b.height, b.cz + b.hd}};

            const COLORREF base = b.node->isDir ? ColorForFolder()
                                                : ColorForExt(b.node->ext);
            POINT facePolys[5][4]{};
            int visibleCount = 0;

            for (const auto& indices : faceIndex) {
                const Vec3 p0 = v[indices[0]];
                const Vec3 p1 = v[indices[1]];
                const Vec3 p2 = v[indices[2]];
                const Vec3 normal = Normalize(Cross(Sub(p2, p0), Sub(p1, p0)));
                const Vec3 center{0.25f * (v[indices[0]].x + v[indices[1]].x + v[indices[2]].x +
                                          v[indices[3]].x),
                                  0.25f * (v[indices[0]].y + v[indices[1]].y + v[indices[2]].y +
                                          v[indices[3]].y),
                                  0.25f * (v[indices[0]].z + v[indices[1]].z + v[indices[2]].z +
                                          v[indices[3]].z)};
                if (Dot(normal, Sub(eye, center)) <= 0.0f) {
                    continue;
                }

                POINT pts[4]{};
                float depth = 0.0f;
                float depthSum = 0.0f;
                bool ok = true;
                for (int k = 0; k < 4; ++k) {
                    if (!project(v[indices[k]], pts[k], depth)) {
                        ok = false;
                        break;
                    }
                    depthSum += depth;
                }
                if (!ok) {
                    continue;
                }

                const float brightness =
                    0.72f + 0.55f * std::max(0.0f, Dot(normal, light));
                COLORREF color = Shade(base, brightness);
                if (b.node == state->selected) {
                    color = Blend(color, RGB(255, 240, 160), 0.5f);
                } else if (b.node == state->hover) {
                    color = Blend(color, RGB(255, 255, 255), 0.22f);
                }

                HBRUSH brush = GetBrush(state, color);
                HPEN pen = GetPen(state, color);
                HGDIOBJ oldBrush = SelectObject(dc, brush);
                HGDIOBJ oldPenObj = SelectObject(dc, pen);
                Polygon(dc, pts, 4);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPenObj);

                ScreenFace face;
                for (int k = 0; k < 4; ++k) {
                    face.pts[k] = pts[k];
                }
                face.node = b.node;
                face.depth = depthSum * 0.25f;
                state->faces.push_back(face);

                if (visibleCount < 5) {
                    for (int k = 0; k < 4; ++k) {
                        facePolys[visibleCount][k] = pts[k];
                    }
                    ++visibleCount;
                }
            }

            if (b.node == state->selected || b.node == state->hover) {
                std::vector<std::array<POINT, 4>>& glow =
                    (b.node == state->selected) ? state->glowSelected : state->glowHover;
                for (int i = 0; i < visibleCount; ++i) {
                    std::array<POINT, 4> poly{};
                    for (int k = 0; k < 4; ++k) {
                        poly[static_cast<size_t>(k)] = facePolys[i][k];
                    }
                    glow.push_back(poly);
                }
            }

            if (b.node == state->selected) {
                POINT anchor{};
                float depth = 0.0f;
                if (project({b.cx, b.height + 0.02f, b.cz}, anchor, depth)) {
                    labelNode = b.node;
                    labelAnchor = anchor;
                }
            }
        }
    }

    DrawGlowGdiPlus(dc, state);
    if (labelNode) {
        DrawLabel(dc, labelNode, labelAnchor);
    }

    SelectObject(dc, oldFont);

    SetStretchBltMode(target, HALFTONE);
    SetBrushOrgEx(target, 0, 0, nullptr);
    StretchBlt(target, 0, 0, width, height, dc, 0, 0, sw, sh, SRCCOPY);

    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT CALLBACK CityWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    CityState* state = reinterpret_cast<CityState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* holder = new CityState();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(holder));
            return TRUE;
        }
        case WM_CREATE: {
            if (state) {
                state->dpi = DpiForWindow(hwnd);
                LOGFONTW lf{};
                lf.lfHeight = -MulDiv(9, state->dpi, 72) * kSuperSample;
                lf.lfWeight = FW_SEMIBOLD;
                wcscpy_s(lf.lfFaceName, L"Segoe UI");
                state->font = CreateFontIndirectW(&lf);
            }
            return 0;
        }
        case WM_SIZE: {
            if (state) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            if (!state) {
                break;
            }
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            if (width > 0 && height > 0) {
                Render(state, dc, width, height);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (state) {
                state->dragging = true;
                state->moved = false;
                state->last.x = GET_X_LPARAM(lParam);
                state->last.y = GET_Y_LPARAM(lParam);
                state->down = state->last;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (state) {
                ReleaseCapture();
                if (state->dragging && !state->moved) {
                    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    Node* node = Pick(state, point);
                    state->selected = node;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    PostMessageW(GetParent(hwnd), WM_APP_CITY_SELECT, 0,
                                 reinterpret_cast<LPARAM>(node));
                }
                state->dragging = false;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            if (state) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (Node* node = Pick(state, point)) {
                    PostMessageW(GetParent(hwnd), WM_APP_CITY_ACTIVATE, 0,
                                 reinterpret_cast<LPARAM>(node));
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!state) {
                return 0;
            }
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (state->dragging) {
                const int dx = x - state->last.x;
                const int dy = y - state->last.y;
                state->last.x = x;
                state->last.y = y;
                const int totalX = x - state->down.x;
                const int totalY = y - state->down.y;
                const int slop = std::max(3, MulDiv(4, state->dpi, 96));
                if (!state->moved && totalX * totalX + totalY * totalY > slop * slop) {
                    state->moved = true;
                }
                if (state->moved) {
                    state->camera.azimuth -= static_cast<float>(dx) * 0.01f;
                    state->camera.elevation += static_cast<float>(dy) * 0.01f;
                    state->camera.elevation =
                        std::min(1.5f, std::max(0.05f, state->camera.elevation));
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else {
                if (!state->tracking) {
                    TRACKMOUSEEVENT track{};
                    track.cbSize = sizeof(track);
                    track.dwFlags = TME_LEAVE;
                    track.hwndTrack = hwnd;
                    TrackMouseEvent(&track);
                    state->tracking = true;
                }
                Node* node = Pick(state, POINT{x, y});
                if (node != state->hover) {
                    state->hover = node;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            if (state) {
                state->tracking = false;
                if (state->hover) {
                    state->hover = nullptr;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (state) {
                state->camera.azimuth = 0.7f;
                state->camera.elevation = 0.5f;
                state->camera.zoom = 1.0f;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (state) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                state->camera.zoom *= (delta > 0) ? 0.9f : 1.1f;
                state->camera.zoom = std::min(6.0f, std::max(0.25f, state->camera.zoom));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DESTROY: {
            if (state) {
                for (auto& entry : state->brushes) {
                    DeleteObject(entry.second);
                }
                for (auto& entry : state->pens) {
                    DeleteObject(entry.second);
                }
                if (state->font) {
                    DeleteObject(state->font);
                }
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                delete state;
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

void RegisterCityClass(HINSTANCE instance) {
    static ULONG_PTR gdiplusToken = 0;
    if (gdiplusToken == 0) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr);
    }

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    cls.lpfnWndProc = CityWndProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    cls.lpszClassName = kCityClass;
    RegisterClassExW(&cls);
}

HWND CreateCityWindow(HWND parent, HINSTANCE instance, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, kCityClass, L"", WS_CHILD, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                           nullptr);
}

void CitySetDirectory(HWND city, Node* dir) {
    auto* state = reinterpret_cast<CityState*>(GetWindowLongPtrW(city, GWLP_USERDATA));
    if (!state) {
        return;
    }
    state->dir = dir;
    state->selected = nullptr;
    state->hover = nullptr;
    InvalidateRect(city, nullptr, FALSE);
}

void CitySetSelected(HWND city, Node* node) {
    auto* state = reinterpret_cast<CityState*>(GetWindowLongPtrW(city, GWLP_USERDATA));
    if (!state) {
        return;
    }
    state->selected = node;
    InvalidateRect(city, nullptr, FALSE);
}
