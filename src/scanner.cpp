#include "scanner.h"

#include <cwctype>

namespace {

std::wstring PrefixLongPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

std::wstring NormalizeRoot(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

std::wstring LowerExtension(const wchar_t* name) {
    const wchar_t* dot = nullptr;
    for (const wchar_t* p = name; *p; ++p) {
        if (*p == L'.') {
            dot = p;
        } else if (*p == L'\\' || *p == L'/') {
            dot = nullptr;
        }
    }
    if (!dot || dot == name || *(dot + 1) == L'\0') {
        return std::wstring();
    }
    std::wstring ext(dot + 1);
    for (wchar_t& ch : ext) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return ext;
}

constexpr uint64_t kNotificationIntervalMs = 60;

}  // namespace

Scanner::~Scanner() {
    Cancel();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Scanner::Start(const std::wstring& rootPath, HWND notify) {
    Cancel();
    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_rootPath = NormalizeRoot(rootPath);
    m_notify = notify;
    m_cancel = false;
    m_stop = false;
    m_pending = 0;
    m_files = 0;
    m_dirs = 0;
    m_bytes = 0;
    m_skipped = 0;
    m_lastNotify = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::queue<Work> empty;
        std::swap(m_queue, empty);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result.reset();
    }

    m_running = true;
    m_thread = std::thread([this] { Run(); });
}

void Scanner::Cancel() {
    m_cancel = true;
    m_cv.notify_all();
}

std::unique_ptr<Node> Scanner::TakeResult() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::move(m_result);
}

void Scanner::Run() {
    auto root = std::make_unique<Node>();
    root->name = m_rootPath;
    root->path = m_rootPath;
    root->isDir = true;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Work work;
        work.path = PrefixLongPath(m_rootPath);
        work.node = root.get();
        m_queue.push(std::move(work));
        m_pending = 1;
    }

    unsigned threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;
    if (threads > 16) threads = 16;

    m_pool.clear();
    m_pool.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        m_pool.emplace_back([this] { WorkerLoop(); });
    }

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_pending == 0; });
        m_stop = true;
    }
    m_cv.notify_all();

    for (auto& worker : m_pool) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_pool.clear();

    ComputeAggregates(root.get());

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = std::move(root);
    }

    m_running = false;
    if (m_notify) {
        PostMessageW(m_notify, WM_APP_SCAN_DONE, 0, 0);
    }
}

void Scanner::WorkerLoop() {
    for (;;) {
        Work work;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) {
                if (m_stop) {
                    return;
                }
                continue;
            }
            work = std::move(m_queue.front());
            m_queue.pop();
        }

        if (!m_cancel.load()) {
            ProcessDir(work.path, work.node);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pending > 0) {
                --m_pending;
            }
            if (m_pending == 0) {
                m_cv.notify_all();
            }
        }
    }
}

void Scanner::ProcessDir(const std::wstring& scanDir, Node* dir) {
    const std::wstring pattern = scanDir + L"\\*";

    WIN32_FIND_DATAW fd{};
    HANDLE handle = FindFirstFileExW(pattern.c_str(),
                                     FindExInfoBasic,
                                     &fd,
                                     FindExSearchNameMatch,
                                     nullptr,
                                     FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE) {
        dir->hasError = true;
        return;
    }

    do {
        const wchar_t* name = fd.cFileName;
        if (name[0] == L'.' &&
            (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) {
            continue;
        }

        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isLink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (isLink) {
            // Do not follow junctions/symlinks: avoids cycles and double counting.
            ++m_skipped;
            continue;
        }

        const std::wstring childScan = scanDir + L"\\" + name;
        const std::wstring childDisplay = dir->path + L"\\" + name;

        if (isDir) {
            auto child = std::make_unique<Node>();
            child->name = name;
            child->path = childDisplay;
            child->isDir = true;
            child->parent = dir;
            Node* childPtr = child.get();
            dir->children.push_back(std::move(child));
            ++m_dirs;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                Work work;
                work.path = childScan;
                work.node = childPtr;
                m_queue.push(std::move(work));
                ++m_pending;
            }
            m_cv.notify_one();
            continue;
        }

        const uint64_t size =
            (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;

        auto child = std::make_unique<Node>();
        child->name = name;
        child->path = childDisplay;
        child->isDir = false;
        child->parent = dir;
        child->ext = LowerExtension(name);
        child->size = size;
        child->ownSize = size;
        dir->children.push_back(std::move(child));

        dir->ownSize += size;
        ++m_files;
        m_bytes += size;
    } while (FindNextFileW(handle, &fd));

    FindClose(handle);
    MaybeNotify();
}

void Scanner::MaybeNotify() {
    if (!m_notify) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - m_lastNotify < kNotificationIntervalMs) {
        return;
    }
    m_lastNotify = now;
    PostMessageW(m_notify, WM_APP_SCAN_PROGRESS, 0, 0);
}
