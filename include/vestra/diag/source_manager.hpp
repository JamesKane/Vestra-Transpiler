#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vestra::diag {

// Identifier for a source buffer loaded into the SourceManager. uint32_t so a
// build with many generated files (macro expansions, @embed) doesn't overflow.
// NOLINTNEXTLINE(performance-enum-size)
enum class FileId : std::uint32_t { Invalid = 0 };

// A point in a source buffer. We store a byte offset because Vestra source is
// UTF-8 (§3); line/column are computed on demand when a diagnostic is rendered.
struct SourceLoc {
    FileId file = FileId::Invalid;
    std::size_t offset = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return file != FileId::Invalid; }
};

// A half-open byte range [begin, end) within a single file.
struct SourceRange {
    SourceLoc begin;
    std::size_t length = 0;

    [[nodiscard]] SourceLoc end() const noexcept { return {begin.file, begin.offset + length}; }

    [[nodiscard]] constexpr bool is_valid() const noexcept { return begin.is_valid(); }
};

// A line/column pair derived from a SourceLoc. Lines and columns are 1-based,
// the way humans count.
struct LineCol {
    std::uint32_t line = 1;
    std::uint32_t col = 1;
};

// Loaded source buffers, indexed by FileId. Owns the buffer text.
//
// Usage:
//   SourceManager sm;
//   auto id = sm.load_file("hello.vst");
//   auto v  = sm.source(id);             // std::string_view into owned buffer
//
class SourceManager {
public:
    SourceManager() = default;
    ~SourceManager() = default;
    SourceManager(const SourceManager&) = delete;
    SourceManager& operator=(const SourceManager&) = delete;
    SourceManager(SourceManager&&) noexcept = default;
    SourceManager& operator=(SourceManager&&) noexcept = default;

    // Load a file from disk. Throws std::ios_base::failure on read error.
    FileId load_file(const std::filesystem::path& path);

    // Register an in-memory buffer (tests, REPL). `name` is used for
    // diagnostics; `text` is copied.
    FileId add_in_memory(std::string name, std::string text);

    [[nodiscard]] std::string_view source(FileId id) const;

    // Display name for a file (the path it was loaded from, or the synthetic
    // name passed to add_in_memory).
    [[nodiscard]] std::string_view name(FileId id) const;

    // Resolve a byte offset to a (line, column) pair.
    [[nodiscard]] LineCol line_col(SourceLoc loc) const;

    // Return the source bytes for the line containing `loc`, with the
    // trailing newline stripped.
    [[nodiscard]] std::string_view line_text(SourceLoc loc) const;

    [[nodiscard]] std::size_t file_count() const noexcept { return files_.size(); }

private:
    struct File {
        std::string name;
        std::string text;
        // Byte offset of the start of each line. line_starts_[0] is always 0.
        std::vector<std::size_t> line_starts;
    };

    static std::vector<std::size_t> compute_line_starts(std::string_view text);
    [[nodiscard]] const File& get(FileId id) const;

    std::vector<File> files_;  // index 0 unused so FileId::Invalid stays distinct
};

}  // namespace vestra::diag
