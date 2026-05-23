#include "vestra/diag/diagnostic.hpp"

#include <ostream>
#include <string>
#include <string_view>

namespace vestra::diag {

namespace {

std::string_view severity_label(Severity s) {
    switch (s) {
    case Severity::Error:
        return "error";
    case Severity::Warning:
        return "warning";
    case Severity::Note:
        return "note";
    }
    return "diagnostic";
}

}  // namespace

void DiagnosticReporter::report(Diagnostic d) {
    switch (d.severity) {
    case Severity::Error:
        ++error_count_;
        break;
    case Severity::Warning:
        ++warning_count_;
        break;
    case Severity::Note:
        break;
    }
    diagnostics_.push_back(std::move(d));
}

void DiagnosticReporter::render_to(std::ostream& os) const {
    for (const auto& d : diagnostics_) {
        render_to(os, d);
    }
}

void DiagnosticReporter::render_to(std::ostream& os, const Diagnostic& d) const {
    // Header: `<file>:<line>:<col>: <severity>: <message>`.
    if (!d.labels.empty() && d.labels.front().range.is_valid()) {
        auto range = d.labels.front().range;
        auto lc = sm_->line_col(range.begin);
        os << sm_->name(range.begin.file) << ":" << lc.line << ":" << lc.col << ": ";
    }
    os << severity_label(d.severity) << ": " << d.message << "\n";

    for (const auto& label : d.labels) {
        if (!label.range.is_valid()) {
            continue;
        }
        auto lc = sm_->line_col(label.range.begin);
        auto line_text = sm_->line_text(label.range.begin);

        // Right-pad the line number so the caret column lines up consistently.
        os << "    " << lc.line << " | " << line_text << "\n";

        auto width = std::to_string(lc.line).size();
        std::size_t caret_off = lc.col - 1;
        os << "    " << std::string(width, ' ') << " | " << std::string(caret_off, ' ');
        // Underline the range with at least one '^'.
        std::size_t span = label.range.length;
        if (span <= 1) {
            os << "^";
        } else {
            os << "^" << std::string(span - 1, '~');
        }
        if (!label.message.empty()) {
            os << " " << label.message;
        }
        os << "\n";
    }

    for (const auto& note : d.notes) {
        render_to(os, note);
    }
}

}  // namespace vestra::diag
