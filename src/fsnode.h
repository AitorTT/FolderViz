#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Node {
    std::wstring name;
    std::wstring path;
    std::wstring ext;       // lowercase, no leading dot (files only)
    uint64_t size = 0;      // aggregate size for directories, file size for files
    uint64_t ownSize = 0;   // bytes of files directly inside a directory
    uint64_t fileCount = 0; // aggregate number of files
    uint64_t dirCount = 0;  // aggregate number of subdirectories
    bool isDir = false;
    bool hasError = false;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
};

// Bottom-up aggregation of directory sizes and counts.
inline void ComputeAggregates(Node* n) {
    if (!n->isDir) {
        n->size = n->ownSize;
        n->fileCount = 1;
        n->dirCount = 0;
        return;
    }

    uint64_t total = 0;
    uint64_t files = 0;
    uint64_t dirs = 0;
    for (auto& child : n->children) {
        ComputeAggregates(child.get());
        total += child->size;
        files += child->fileCount;
        dirs += child->dirCount;
        if (child->isDir) {
            ++dirs;
        }
    }
    n->size = total;
    n->fileCount = files;
    n->dirCount = dirs;
}
