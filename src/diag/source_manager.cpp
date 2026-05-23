// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/diag/source_manager.hpp"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace vestra::diag {

namespace {
constexpr std::size_t SentinelSlot = 0;  // files_[0] is unused so FileId 0 stays invalid
}

std::vector<std::size_t> SourceManager::compute_line_starts(std::string_view text) {
    std::vector<std::size_t> starts;
    starts.push_back(0);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            starts.push_back(i + 1);
        }
    }
    return starts;
}

FileId SourceManager::load_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::ios_base::failure("cannot open source file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return add_in_memory(path.string(), std::move(buffer).str());
}

FileId SourceManager::add_in_memory(std::string name, std::string text) {
    if (files_.empty()) {
        files_.emplace_back();  // reserve index 0
    }
    File f;
    f.line_starts = compute_line_starts(text);
    f.name = std::move(name);
    f.text = std::move(text);
    files_.push_back(std::move(f));
    return static_cast<FileId>(files_.size() - 1);
}

const SourceManager::File& SourceManager::get(FileId id) const {
    auto idx = static_cast<std::size_t>(id);
    if (idx == SentinelSlot || idx >= files_.size()) {
        throw std::out_of_range("SourceManager::get: invalid FileId");
    }
    return files_[idx];
}

std::string_view SourceManager::source(FileId id) const {
    return get(id).text;
}

std::string_view SourceManager::name(FileId id) const {
    return get(id).name;
}

LineCol SourceManager::line_col(SourceLoc loc) const {
    const auto& f = get(loc.file);
    // upper_bound returns the first line_start strictly greater than loc.offset;
    // the line containing the offset is therefore one before that.
    auto it = std::upper_bound(f.line_starts.begin(), f.line_starts.end(), loc.offset);
    assert(it != f.line_starts.begin());
    auto line_idx = static_cast<std::size_t>((it - f.line_starts.begin()) - 1);
    auto col = loc.offset - f.line_starts[line_idx];
    return {static_cast<std::uint32_t>(line_idx + 1), static_cast<std::uint32_t>(col + 1)};
}

std::string_view SourceManager::line_text(SourceLoc loc) const {
    const auto& f = get(loc.file);
    auto it = std::upper_bound(f.line_starts.begin(), f.line_starts.end(), loc.offset);
    auto line_idx = static_cast<std::size_t>((it - f.line_starts.begin()) - 1);
    std::size_t start = f.line_starts[line_idx];
    std::size_t next_line =
        (line_idx + 1 < f.line_starts.size()) ? f.line_starts[line_idx + 1] : f.text.size();
    std::size_t end = next_line;
    if (end > start && f.text[end - 1] == '\n') {
        --end;
    }
    if (end > start && f.text[end - 1] == '\r') {
        --end;
    }
    return std::string_view{f.text}.substr(start, end - start);
}

}  // namespace vestra::diag
