#include "app.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "filetype.h"
#include "format.h"
#include "scanner.h"
#include "treemap.h"

namespace {

constexpr wchar_t kMainClass[] = L"FolderVizMain";
constexpr wchar_t kTreemapClass[] = L"FolderVizTreemap";

constexpr int kIdTree = 2001;
constexpr int kIdList = 2002;
constexpr int kIdTreemap = 2003;
constexpr int kIdStatus = 2004;

constexpr int kCmdScan = 1001;
constexpr int kCmdRefresh = 1002;
constexpr int kCmdExit = 1003;

struct App {
    HINSTANCE instance = nullptr;
    HWND main = nullptr;
    HWND tree = nullptr;
    HWND list = nullptr;
    HWND treemap = nullptr;
    HWND status = nullptr;
    HFONT font = nullptr;

    Scanner scanner;
    std::unique_ptr<Node> root;
    Node* current = nullptr;   // directory currently shown in list + treemap
    Node* selected = nullptr;  // highlighted node (file or directory)
    Node* hover = nullptr;     // node under the cursor in the treemap
    HTREEITEM rootItem = nullptr;
    std::unordered_set<Node*> populated;

    std::vector<Node*> listItems;
    int sortColumn = 1;
    bool sortAscending = false;

    std::vector<TmRect> hits;
    int legendHeight = 0;
    bool scanning = false;
    int dpi = 96;
    bool trackingMouse = false;
};

App g_app;
App* g = &g_app;

int Scale(const App* a, int value) {
    return MulDiv(value, a->dpi, 96);
}

int DpiForWindow(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn) {
        return static_cast<int>(fn(hwnd));
    }
    return 96;
}

bool IsLight(COLORREF color) {
    const int luminance = (GetRValue(color) * 299 + GetGValue(color) * 587 +
                           GetBValue(color) * 114) / 1000;
    return luminance > 150;
}

std::wstring TypeLabel(const Node* node) {
    if (node->isDir) {
        return L"Folder";
    }
    if (node->ext.empty()) {
        return L"File";
    }
    std::wstring label = node->ext;
    for (wchar_t& ch : label) {
        ch = static_cast<wchar_t>(towupper(ch));
    }
    return label;
}

std::wstring PercentText(const Node* node, uint64_t base) {
    if (base == 0) {
        return L"0.0%";
    }
    const double pct = 100.0 * static_cast<double>(node->size) /
                       static_cast<double>(base);
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f%%", pct);
    return buf;
}

// ---------------------------------------------------------------------------
// Tree view helpers
// ---------------------------------------------------------------------------

HTREEITEM InsertNode(HWND tree, HTREEITEM parent, Node* node) {
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    const std::wstring label = node->name + L"  [" + FormatSize(node->size) + L"]";
    insert.item.pszText = const_cast<LPWSTR>(label.c_str());
    insert.item.lParam = reinterpret_cast<LPARAM>(node);
    insert.item.cChildren = node->children.empty() ? 0 : 1;
    return TreeView_InsertItem(tree, &insert);
}

void PopulateChildren(HWND tree, HTREEITEM item, Node* node,
                      std::unordered_set<Node*>& populated) {
    if (populated.count(node)) {
        return;
    }
    populated.insert(node);
    for (auto& child : node->children) {
        InsertNode(tree, item, child.get());
    }
}

HTREEITEM FindChildItem(HWND tree, HTREEITEM parent, Node* node) {
    for (HTREEITEM item = TreeView_GetChild(tree, parent); item;
         item = TreeView_GetNextSibling(tree, item)) {
        TVITEMW info{};
        info.mask = TVIF_PARAM;
        info.hItem = item;
        if (TreeView_GetItem(tree, &info) &&
            reinterpret_cast<Node*>(info.lParam) == node) {
            return item;
        }
    }
    return nullptr;
}

void EnsureTreePath(App* a, Node* target) {
    if (!a->rootItem || !target) {
        return;
    }
    std::vector<Node*> chain;
    for (Node* p = target; p; p = p->parent) {
        chain.push_back(p);
    }
    std::reverse(chain.begin(), chain.end());

    HTREEITEM item = a->rootItem;
    for (size_t i = 1; i < chain.size(); ++i) {
        PopulateChildren(a->tree, item, chain[i - 1], a->populated);
        HTREEITEM next = FindChildItem(a->tree, item, chain[i]);
        if (!next) {
            return;
        }
        TreeView_Expand(a->tree, item, TVE_EXPAND);
        item = next;
    }
    TreeView_SelectItem(a->tree, item);
    TreeView_EnsureVisible(a->tree, item);
}

// ---------------------------------------------------------------------------
// List view (virtual) helpers
// ---------------------------------------------------------------------------

void SortList(App* a) {
    const int column = a->sortColumn;
    const bool ascending = a->sortAscending;
    std::stable_sort(a->listItems.begin(), a->listItems.end(),
                     [column, ascending](const Node* x, const Node* y) {
                         if (x->isDir != y->isDir) {
                             return x->isDir;  // folders first
                         }
                         int cmp = 0;
                         switch (column) {
                             case 0: {
                                 const int r = _wcsicmp(x->name.c_str(), y->name.c_str());
                                 cmp = r;
                                 break;
                             }
                             case 2:
                                 cmp = (x->size < y->size) ? -1 : (x->size > y->size ? 1 : 0);
                                 break;
                             case 3:
                                 cmp = _wcsicmp(TypeLabel(x).c_str(), TypeLabel(y).c_str());
                                 break;
                             default:
                                 cmp = (x->size < y->size) ? -1 : (x->size > y->size ? 1 : 0);
                                 break;
                         }
                         if (cmp == 0) {
                             cmp = _wcsicmp(x->name.c_str(), y->name.c_str());
                         }
                         return ascending ? (cmp < 0) : (cmp > 0);
                     });
}

void RefreshList(App* a) {
    a->listItems.clear();
    if (a->current) {
        a->listItems.reserve(a->current->children.size());
        for (auto& child : a->current->children) {
            a->listItems.push_back(child.get());
        }
    }
    SortList(a);
    ListView_SetItemCountEx(a->list, static_cast<int>(a->listItems.size()),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    ListView_SetItemState(a->list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    InvalidateRect(a->list, nullptr, TRUE);
}

std::wstring ListText(App* a, const Node* node, int column) {
    switch (column) {
        case 0:
            return node->name;
        case 1:
            return FormatSize(node->size);
        case 2:
            return PercentText(node, a->current ? a->current->size : 0);
        default:
            return TypeLabel(node);
    }
}

// ---------------------------------------------------------------------------
// Treemap
// ---------------------------------------------------------------------------

void BuildTreemap(App* a) {
    a->hits.clear();
    if (!a->current) {
        return;
    }
    RECT rc{};
    GetClientRect(a->treemap, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int legend = a->legendHeight;
    if (width <= 0 || height - legend <= 0) {
        return;
    }

    std::vector<Node*> items;
    items.reserve(a->current->children.size());
    for (auto& child : a->current->children) {
        items.push_back(child.get());
    }
    TreemapLayout(items, 0.0f, 0.0f, static_cast<float>(width),
                  static_cast<float>(height - legend), a->hits);
}

struct LegendEntry {
    std::wstring label;
    COLORREF color;
    uint64_t size;
};

std::vector<LegendEntry> BuildLegend(App* a) {
    std::vector<LegendEntry> entries;
    if (!a->current) {
        return entries;
    }

    struct Accum {
        COLORREF color;
        uint64_t size;
    };
    std::unordered_map<std::wstring, Accum> totals;
    for (auto& child : a->current->children) {
        const std::wstring key = TypeLabel(child.get());
        Accum& acc = totals[key];
        if (child->isDir) {
            acc.color = ColorForFolder();
        } else if (acc.size == 0) {
            acc.color = ColorForExt(child->ext);
        }
        acc.size += child->size;
    }

    entries.reserve(totals.size());
    for (auto& pair : totals) {
        entries.push_back({pair.first, pair.second.color, pair.second.size});
    }
    std::sort(entries.begin(), entries.end(),
              [](const LegendEntry& x, const LegendEntry& y) {
                  return x.size > y.size;
              });
    return entries;
}

void PaintTreemap(App* a, HDC target, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(buffer, bitmap));

    HBRUSH background = CreateSolidBrush(RGB(250, 250, 250));
    FillRect(buffer, &client, background);
    DeleteObject(background);

    SetBkMode(buffer, TRANSPARENT);

    HFONT oldFont = static_cast<HFONT>(SelectObject(buffer, a->font));

    for (const TmRect& rect : a->hits) {
        const int x = static_cast<int>(rect.x);
        const int y = static_cast<int>(rect.y);
        const int w = static_cast<int>(rect.w);
        const int h = static_cast<int>(rect.h);
        if (w <= 0 || h <= 0) {
            continue;
        }

        const COLORREF color =
            rect.node->isDir ? ColorForFolder() : ColorForExt(rect.node->ext);
        RECT fill{x, y, x + w, y + h};
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(buffer, &fill, brush);
        DeleteObject(brush);

        const bool hovered = (rect.node == a->hover);
        const bool selected = (rect.node == a->selected);
        if (hovered || selected) {
            HPEN pen = CreatePen(PS_SOLID, selected ? Scale(a, 2) : 1,
                                 selected ? RGB(0, 0, 0) : RGB(255, 255, 255));
            HPEN oldPen = static_cast<HPEN>(SelectObject(buffer, pen));
            HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
            HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(buffer, hollow));
            Rectangle(buffer, x, y, x + w, y + h);
            SelectObject(buffer, oldBrush);
            SelectObject(buffer, oldPen);
            DeleteObject(pen);
        } else {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(190, 190, 190));
            HPEN oldPen = static_cast<HPEN>(SelectObject(buffer, pen));
            HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
            HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(buffer, hollow));
            Rectangle(buffer, x, y, x + w, y + h);
            SelectObject(buffer, oldBrush);
            SelectObject(buffer, oldPen);
            DeleteObject(pen);
        }

        if (w > Scale(a, 48) && h > Scale(a, 18)) {
            SetTextColor(buffer, IsLight(color) ? RGB(20, 20, 20) : RGB(255, 255, 255));
            RECT text{x + Scale(a, 3), y, x + w - Scale(a, 2), y + h};
            DrawTextW(buffer, rect.node->name.c_str(), -1, &text,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    // Legend strip.
    if (a->legendHeight > 0) {
        const int legendTop = height - a->legendHeight;
        RECT legendRect{0, legendTop, width, height};
        HBRUSH legendBrush = CreateSolidBrush(RGB(236, 238, 240));
        FillRect(buffer, &legendRect, legendBrush);
        DeleteObject(legendBrush);

        const std::vector<LegendEntry> entries = BuildLegend(a);
        int x = Scale(a, 6);
        const int swatch = Scale(a, 13);
        const int gap = Scale(a, 4);
        const int itemGap = Scale(a, 14);
        for (const LegendEntry& entry : entries) {
            const std::wstring text = entry.label + L"  " + FormatSize(entry.size);
            RECT measure{0, 0, 0, 0};
            DrawTextW(buffer, text.c_str(), -1, &measure,
                      DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
            const int itemWidth = swatch + gap + (measure.right - measure.left);
            if (x + itemWidth > width - Scale(a, 6)) {
                break;
            }

            RECT box{x, legendTop + Scale(a, 8), x + swatch, legendTop + Scale(a, 8) + swatch};
            HBRUSH brush = CreateSolidBrush(entry.color);
            FillRect(buffer, &box, brush);
            DeleteObject(brush);

            SetTextColor(buffer, RGB(40, 40, 40));
            RECT textRect{box.right + gap, legendTop, box.right + gap + itemWidth, height};
            DrawTextW(buffer, text.c_str(), -1, &textRect,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            x += itemWidth + itemGap;
        }
    }

    SelectObject(buffer, oldFont);
    BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

TmRect* HitTest(App* a, int x, int y) {
    for (TmRect& rect : a->hits) {
        if (static_cast<float>(x) >= rect.x && static_cast<float>(x) < rect.x + rect.w &&
            static_cast<float>(y) >= rect.y && static_cast<float>(y) < rect.y + rect.h) {
            return &rect;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Status bar / layout
// ---------------------------------------------------------------------------

void UpdateStatus(App* a) {
    if (!a->status) {
        return;
    }

    std::wstring left;
    if (a->scanning) {
        left = L"Scanning " + a->scanner.RootPath() + L"...";
    } else if (a->hover) {
        left = a->hover->name + L"  -  " + FormatSize(a->hover->size);
    } else if (a->selected) {
        left = a->selected->name + L"  -  " + FormatSize(a->selected->size);
    } else if (a->current) {
        left = a->current->path;
    } else {
        left = L"File > Scan Folder to begin";
    }
    SendMessageW(a->status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(left.c_str()));

    std::wstring middle;
    std::wstring right;
    if (a->scanning) {
        middle = std::to_wstring(a->scanner.Files()) + L" files, " +
                 std::to_wstring(a->scanner.Dirs()) + L" folders";
        right = FormatSize(a->scanner.Bytes());
    } else if (a->current) {
        middle = std::to_wstring(a->current->fileCount) + L" files, " +
                 std::to_wstring(a->current->dirCount) + L" folders";
        right = FormatSize(a->current->size);
    }
    SendMessageW(a->status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(middle.c_str()));
    SendMessageW(a->status, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(right.c_str()));
}

void Layout(App* a) {
    RECT client{};
    GetClientRect(a->main, &client);
    SendMessageW(a->status, WM_SIZE, 0, 0);

    RECT statusRect{};
    GetWindowRect(a->status, &statusRect);
    const int statusHeight = statusRect.bottom - statusRect.top;

    const int width = client.right - client.left;
    const int contentHeight = (client.bottom - client.top) - statusHeight;
    const int topHeight = contentHeight * 45 / 100;
    const int treeWidth = width * 35 / 100;

    MoveWindow(a->tree, 0, 0, treeWidth, topHeight, TRUE);
    MoveWindow(a->list, treeWidth, 0, width - treeWidth, topHeight, TRUE);
    MoveWindow(a->treemap, 0, topHeight, width, contentHeight - topHeight, TRUE);

    int parts[3];
    parts[0] = width - Scale(a, 320);
    parts[1] = width - Scale(a, 160);
    parts[2] = -1;
    SendMessageW(a->status, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void StartScan(App* a, const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    a->scanning = true;
    a->root.reset();
    a->current = nullptr;
    a->selected = nullptr;
    a->hover = nullptr;
    a->hits.clear();
    a->listItems.clear();
    a->populated.clear();
    a->rootItem = nullptr;

    TreeView_DeleteAllItems(a->tree);
    ListView_SetItemCountEx(a->list, 0, 0);
    InvalidateRect(a->treemap, nullptr, TRUE);

    UpdateStatus(a);
    a->scanner.Start(path, a->main);
}

void OnScanDone(App* a) {
    a->scanning = false;
    a->root = a->scanner.TakeResult();
    if (!a->root) {
        UpdateStatus(a);
        return;
    }

    a->populated.clear();
    a->rootItem = nullptr;
    TreeView_DeleteAllItems(a->tree);
    a->rootItem = InsertNode(a->tree, TVI_ROOT, a->root.get());
    if (a->rootItem) {
        PopulateChildren(a->tree, a->rootItem, a->root.get(), a->populated);
        TreeView_Expand(a->tree, a->rootItem, TVE_EXPAND);
        TreeView_SelectItem(a->tree, a->rootItem);
    }

    a->current = a->root.get();
    a->selected = nullptr;
    a->hover = nullptr;
    RefreshList(a);
    BuildTreemap(a);
    UpdateStatus(a);
    InvalidateRect(a->treemap, nullptr, TRUE);
}

std::wstring PickFolder(App* a, const std::wstring& initial) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return result;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose a folder to scan");

    if (!initial.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    if (SUCCEEDED(dialog->Show(a->main))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }

    dialog->Release();
    return result;
}

// ---------------------------------------------------------------------------
// Window procedures
// ---------------------------------------------------------------------------

void OnTreeNotify(App* a, NMTREEVIEWW* info) {
    switch (info->hdr.code) {
        case TVN_ITEMEXPANDINGW: {
            if (info->action == TVE_EXPAND) {
                Node* node = reinterpret_cast<Node*>(info->itemNew.lParam);
                if (node) {
                    PopulateChildren(a->tree, info->itemNew.hItem, node, a->populated);
                }
            }
            break;
        }
        case TVN_SELCHANGEDW: {
            Node* node = reinterpret_cast<Node*>(info->itemNew.lParam);
            if (!node) {
                break;
            }
            if (node->isDir) {
                a->current = node;
                a->selected = nullptr;
            } else {
                a->current = node->parent;
                a->selected = node;
            }
            a->hover = nullptr;
            RefreshList(a);
            BuildTreemap(a);
            UpdateStatus(a);
            InvalidateRect(a->treemap, nullptr, TRUE);
            break;
        }
        default:
            break;
    }
}

void OnListNotify(App* a, NMHDR* header) {
    switch (header->code) {
        case LVN_GETDISPINFOW: {
            auto* info = reinterpret_cast<NMLVDISPINFOW*>(header);
            if (info->item.mask & LVIF_TEXT) {
                const int index = info->item.iItem;
                if (index >= 0 && index < static_cast<int>(a->listItems.size())) {
                    const std::wstring text = ListText(a, a->listItems[index], info->item.iSubItem);
                    wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(),
                              _TRUNCATE);
                }
            }
            break;
        }
        case LVN_COLUMNCLICK: {
            auto* info = reinterpret_cast<NMLISTVIEW*>(header);
            if (a->sortColumn == info->iSubItem) {
                a->sortAscending = !a->sortAscending;
            } else {
                a->sortColumn = info->iSubItem;
                a->sortAscending = (info->iSubItem == 0);
            }
            SortList(a);
            InvalidateRect(a->list, nullptr, TRUE);
            break;
        }
        case LVN_ITEMCHANGED: {
            auto* info = reinterpret_cast<NMLISTVIEW*>(header);
            if ((info->uNewState & LVIS_SELECTED) &&
                !(info->uOldState & LVIS_SELECTED)) {
                const int index = info->iItem;
                if (index >= 0 && index < static_cast<int>(a->listItems.size())) {
                    a->selected = a->listItems[index];
                    BuildTreemap(a);
                    UpdateStatus(a);
                    InvalidateRect(a->treemap, nullptr, TRUE);
                }
            }
            break;
        }
        case NM_DBLCLK: {
            auto* info = reinterpret_cast<NMITEMACTIVATE*>(header);
            const int index = info->iItem;
            if (index >= 0 && index < static_cast<int>(a->listItems.size())) {
                Node* node = a->listItems[index];
                if (node->isDir) {
                    EnsureTreePath(a, node);
                }
            }
            break;
        }
        default:
            break;
    }
}

LRESULT CALLBACK TreemapWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* a = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE: {
            if (a) {
                BuildTreemap(a);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_PAINT: {
            if (!a) {
                break;
            }
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            PaintTreemap(a, dc, client);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!a) {
                break;
            }
            if (!a->trackingMouse) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                a->trackingMouse = true;
            }
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            TmRect* hit = HitTest(a, x, y);
            Node* node = hit ? hit->node : nullptr;
            if (node != a->hover) {
                a->hover = node;
                UpdateStatus(a);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            if (a) {
                a->trackingMouse = false;
                if (a->hover) {
                    a->hover = nullptr;
                    UpdateStatus(a);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!a) {
                break;
            }
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (TmRect* hit = HitTest(a, x, y)) {
                a->selected = hit->node;
                UpdateStatus(a);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            if (!a) {
                break;
            }
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (TmRect* hit = HitTest(a, x, y)) {
                if (hit->node->isDir) {
                    EnsureTreePath(a, hit->node);
                }
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* a = g;

    switch (message) {
        case WM_CREATE: {
            a->main = hwnd;
            a->dpi = DpiForWindow(hwnd);

            NONCLIENTMETRICSW metrics{};
            metrics.cbSize = sizeof(metrics);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
            a->font = CreateFontIndirectW(&metrics.lfMessageFont);

            a->tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                      WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASBUTTONS |
                                          TVS_HASLINES | TVS_LINESATROOT |
                                          TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
                                      0, 0, 0, 0, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTree)),
                                      a->instance, nullptr);
            a->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                      WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT |
                                          LVS_OWNERDATA | LVS_SHOWSELALWAYS |
                                          LVS_SINGLESEL,
                                      0, 0, 0, 0, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)),
                                      a->instance, nullptr);
            a->treemap = CreateWindowExW(WS_EX_CLIENTEDGE, kTreemapClass, L"",
                                         WS_CHILD | WS_VISIBLE,
                                         0, 0, 0, 0, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTreemap)),
                                         a->instance, a);
            a->status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                        hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)),
                                        a->instance, nullptr);

            TreeView_SetExtendedStyle(a->tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
            ListView_SetExtendedListViewStyle(
                a->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

            SendMessageW(a->tree, WM_SETFONT, reinterpret_cast<WPARAM>(a->font), TRUE);
            SendMessageW(a->list, WM_SETFONT, reinterpret_cast<WPARAM>(a->font), TRUE);
            SendMessageW(a->status, WM_SETFONT, reinterpret_cast<WPARAM>(a->font), TRUE);

            struct Column {
                const wchar_t* text;
                int width;
                int format;
            };
            const Column columns[] = {
                {L"Name", 320, LVCFMT_LEFT},
                {L"Size", 100, LVCFMT_RIGHT},
                {L"%", 70, LVCFMT_RIGHT},
                {L"Type", 120, LVCFMT_LEFT},
            };
            for (int i = 0; i < 4; ++i) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                column.pszText = const_cast<LPWSTR>(columns[i].text);
                column.cx = columns[i].width;
                column.fmt = columns[i].format;
                ListView_InsertColumn(a->list, i, &column);
            }

            if (a->dpi != 96) {
                for (int i = 0; i < 4; ++i) {
                    ListView_SetColumnWidth(a->list, i, 0);
                }
                ListView_SetColumnWidth(a->list, 0, Scale(a, 320));
                ListView_SetColumnWidth(a->list, 1, Scale(a, 100));
                ListView_SetColumnWidth(a->list, 2, Scale(a, 70));
                ListView_SetColumnWidth(a->list, 3, Scale(a, 120));
            }

            a->legendHeight = Scale(a, 30);
            break;
        }
        case WM_SIZE:
            Layout(a);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = Scale(a, 640);
            info->ptMinTrackSize.y = Scale(a, 400);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case kCmdScan: {
                    const std::wstring folder = PickFolder(a, a->scanner.RootPath());
                    if (!folder.empty()) {
                        StartScan(a, folder);
                    }
                    break;
                }
                case kCmdRefresh:
                    if (!a->scanning && a->root) {
                        StartScan(a, a->root->path);
                    }
                    break;
                case kCmdExit:
                    DestroyWindow(hwnd);
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom == a->tree) {
                OnTreeNotify(a, reinterpret_cast<NMTREEVIEWW*>(lParam));
            } else if (header->hwndFrom == a->list) {
                OnListNotify(a, header);
            }
            return 0;
        }
        case WM_APP_SCAN_PROGRESS:
            UpdateStatus(a);
            return 0;
        case WM_APP_SCAN_DONE:
            OnScanDone(a);
            return 0;
        case WM_DESTROY:
            a->scanner.Cancel();
            if (a->font) {
                DeleteObject(a->font);
                a->font = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HMENU CreateAppMenu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kCmdScan, L"&Scan Folder...\tCtrl+O");
    AppendMenuW(file, MF_STRING, kCmdRefresh, L"&Refresh\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kCmdExit, L"E&xit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    return menu;
}

}  // namespace

int RunApp(HINSTANCE instance, const std::wstring& initialPath) {
    g->instance = instance;

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = MainWndProc;
    mainClass.hInstance = instance;
    mainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    mainClass.lpszClassName = kMainClass;
    RegisterClassExW(&mainClass);

    WNDCLASSEXW treemapClass{};
    treemapClass.cbSize = sizeof(treemapClass);
    treemapClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    treemapClass.lpfnWndProc = TreemapWndProc;
    treemapClass.hInstance = instance;
    treemapClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    treemapClass.hbrBackground = nullptr;
    treemapClass.lpszClassName = kTreemapClass;
    RegisterClassExW(&treemapClass);

    HWND hwnd = CreateWindowExW(0, kMainClass,
                                L"FolderViz - Folder Size Visualizer",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1120,
                                740, nullptr, CreateAppMenu(), instance, nullptr);
    if (!hwnd) {
        return 1;
    }

    g->main = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!initialPath.empty()) {
        StartScan(g, initialPath);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
