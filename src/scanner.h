#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "fsnode.h"

// Posted to the notify window (no payload; read the atomics for progress).
constexpr UINT WM_APP_SCAN_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 2;

class Scanner {
public:
    Scanner() = default;
    ~Scanner();

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    // Starts a scan, cancelling any scan already in flight (blocks until joined).
    void Start(const std::wstring& rootPath, HWND notify);
    void Cancel();

    bool Running() const { return m_running.load(); }

    uint64_t Files() const { return m_files.load(); }
    uint64_t Dirs() const { return m_dirs.load(); }
    uint64_t Bytes() const { return m_bytes.load(); }
    uint64_t Skipped() const { return m_skipped.load(); }

    // Transfers ownership of the completed tree to the caller.
    std::unique_ptr<Node> TakeResult();
    std::wstring RootPath() const { return m_rootPath; }

private:
    struct Work {
        std::wstring path;  // long-path-prefixed path used for enumeration
        Node* node = nullptr;
    };

    void Run();
    void ProcessDir(const std::wstring& scanDir, Node* dir);
    void WorkerLoop();
    void MaybeNotify();

    HWND m_notify = nullptr;
    std::wstring m_rootPath;
    std::unique_ptr<Node> m_result;

    std::thread m_thread;
    std::vector<std::thread> m_pool;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<Work> m_queue;
    uint64_t m_pending = 0;
    bool m_stop = false;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<uint64_t> m_files{0};
    std::atomic<uint64_t> m_dirs{0};
    std::atomic<uint64_t> m_bytes{0};
    std::atomic<uint64_t> m_skipped{0};

    ULONGLONG m_lastNotify = 0;
};
