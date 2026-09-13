#include "filetype.h"

#include <array>
#include <unordered_map>

namespace {

const std::array<COLORREF, 24> kPalette = {
    RGB(214, 69, 65),   RGB(241, 138, 46),  RGB(243, 194, 58),
    RGB(142, 196, 65),  RGB(76, 175, 112),  RGB(56, 176, 178),
    RGB(63, 143, 214),  RGB(84, 102, 197),  RGB(139, 94, 199),
    RGB(206, 89, 168),  RGB(150, 111, 74),  RGB(120, 144, 156),
    RGB(232, 102, 128), RGB(255, 165, 90),  RGB(200, 200, 70),
    RGB(120, 200, 140), RGB(110, 190, 220), RGB(120, 150, 240),
    RGB(180, 130, 220), RGB(230, 140, 200), RGB(170, 140, 110),
    RGB(110, 130, 140), RGB(240, 120, 90),  RGB(90, 170, 120),
};

const std::unordered_map<std::wstring, COLORREF>& KnownExts() {
    static const std::unordered_map<std::wstring, COLORREF> map = {
        {L"exe", RGB(214, 69, 65)},    {L"dll", RGB(176, 52, 52)},
        {L"sys", RGB(150, 40, 40)},    {L"obj", RGB(196, 110, 60)},
        {L"png", RGB(241, 150, 46)},   {L"jpg", RGB(240, 190, 50)},
        {L"jpeg", RGB(240, 190, 50)},  {L"gif", RGB(230, 170, 60)},
        {L"bmp", RGB(220, 160, 70)},   {L"svg", RGB(255, 200, 80)},
        {L"mp3", RGB(142, 196, 65)},   {L"wav", RGB(120, 180, 70)},
        {L"flac", RGB(100, 170, 80)},  {L"mp4", RGB(76, 175, 112)},
        {L"mkv", RGB(60, 160, 130)},   {L"avi", RGB(50, 150, 120)},
        {L"mov", RGB(70, 180, 140)},   {L"zip", RGB(56, 176, 178)},
        {L"7z", RGB(50, 160, 165)},    {L"rar", RGB(60, 150, 190)},
        {L"gz", RGB(80, 170, 200)},    {L"tar", RGB(90, 160, 190)},
        {L"iso", RGB(84, 130, 190)},   {L"pdf", RGB(206, 70, 70)},
        {L"doc", RGB(63, 143, 214)},   {L"docx", RGB(63, 143, 214)},
        {L"xls", RGB(70, 160, 120)},   {L"xlsx", RGB(70, 160, 120)},
        {L"ppt", RGB(215, 110, 70)},   {L"pptx", RGB(215, 110, 70)},
        {L"txt", RGB(150, 160, 170)},  {L"md", RGB(120, 144, 156)},
        {L"log", RGB(130, 140, 150)},  {L"ini", RGB(160, 150, 130)},
        {L"json", RGB(180, 160, 90)},  {L"xml", RGB(170, 150, 100)},
        {L"html", RGB(214, 100, 60)},  {L"css", RGB(90, 130, 210)},
        {L"js", RGB(230, 200, 80)},    {L"ts", RGB(70, 120, 200)},
        {L"c", RGB(100, 140, 210)},    {L"cpp", RGB(84, 102, 197)},
        {L"h", RGB(120, 140, 220)},    {L"hpp", RGB(120, 140, 220)},
        {L"cs", RGB(120, 160, 90)},    {L"py", RGB(80, 130, 180)},
        {L"java", RGB(200, 100, 60)},  {L"rs", RGB(190, 120, 80)},
        {L"go", RGB(80, 180, 200)},    {L"db", RGB(140, 94, 199)},
        {L"sql", RGB(160, 110, 200)},  {L"ttf", RGB(139, 94, 199)},
        {L"fon", RGB(150, 110, 200)},  {L"ico", RGB(206, 89, 168)},
    };
    return map;
}

}  // namespace

COLORREF ColorForFolder() {
    return RGB(240, 196, 92);
}

COLORREF ColorForExt(const std::wstring& ext) {
    if (ext.empty()) {
        return RGB(170, 176, 184);
    }

    const auto& known = KnownExts();
    auto it = known.find(ext);
    if (it != known.end()) {
        return it->second;
    }

    // Stable hash of the extension into the fallback palette.
    uint32_t hash = 2166136261u;
    for (wchar_t ch : ext) {
        hash ^= static_cast<uint32_t>(ch);
        hash *= 16777619u;
    }
    return kPalette[hash % kPalette.size()];
}
