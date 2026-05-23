#pragma once

#include "vestra/diag/source_manager.hpp"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace vestra::diag {

enum class Severity { Note, Warning, Error };

struct Label {
    SourceRange range;
    std::string message;
};

// A single diagnostic message. Owns its strings so the producer is free to
// build them with std::format / std::to_string and then drop intermediates.
struct Diagnostic {
    Severity severity;
    std::string message;
    std::vector<Label> labels;
    // Notes are subordinate diagnostics that elaborate the primary one.
    std::vector<Diagnostic> notes;

    static Diagnostic error(std::string msg) { return {Severity::Error, std::move(msg), {}, {}}; }
    static Diagnostic warning(std::string msg) {
        return {Severity::Warning, std::move(msg), {}, {}};
    }
    static Diagnostic note(std::string msg) { return {Severity::Note, std::move(msg), {}, {}}; }

    Diagnostic& at(SourceRange r, std::string label = "") & {
        labels.push_back({r, std::move(label)});
        return *this;
    }
    Diagnostic&& at(SourceRange r, std::string label = "") && {
        labels.push_back({r, std::move(label)});
        return std::move(*this);
    }

    Diagnostic& with_note(Diagnostic n) & {
        notes.push_back(std::move(n));
        return *this;
    }
    Diagnostic&& with_note(Diagnostic n) && {
        notes.push_back(std::move(n));
        return std::move(*this);
    }
};

// Sink for diagnostics. Accumulates and pretty-prints them.
//
// A reporter has a hard cap on the number of errors it will accept before
// returning `should_stop()` so the compiler can bail out instead of cascading
// hundreds of derived failures.
class DiagnosticReporter {
public:
    explicit DiagnosticReporter(const SourceManager& sm, std::size_t error_cap = 32)
        : sm_(&sm), error_cap_(error_cap) {}

    void report(Diagnostic d);

    void render_to(std::ostream& os) const;
    void render_to(std::ostream& os, const Diagnostic& d) const;

    std::size_t error_count() const noexcept { return error_count_; }
    std::size_t warning_count() const noexcept { return warning_count_; }
    bool has_errors() const noexcept { return error_count_ > 0; }
    bool should_stop() const noexcept { return error_count_ >= error_cap_; }

    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

private:
    const SourceManager* sm_;
    std::size_t error_cap_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t error_count_ = 0;
    std::size_t warning_count_ = 0;
};

}  // namespace vestra::diag
