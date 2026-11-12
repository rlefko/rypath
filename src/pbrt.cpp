// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt.hpp"

#include <algorithm>
#include <exception>
#include <sstream>
#include <string_view>

namespace rypath::pbrt {
namespace {

std::string printable(std::string_view text) {
    std::string result;
    constexpr std::string_view hex = "0123456789abcdef";
    for (unsigned char character : text) {
        if (character < 32 || character == 127) {
            result += "\\x";
            result += hex[character >> 4];
            result += hex[character & 15];
        } else {
            result += static_cast<char>(character);
        }
    }
    return result;
}

std::string_view severity_name(Severity severity) {
    switch (severity) {
    case Severity::error:
        return "error";
    case Severity::unsupported:
        return "unsupported";
    case Severity::warning:
        return "warning";
    }
    return "error";
}

std::string_view dependency_name(DependencyKind kind) {
    switch (kind) {
    case DependencyKind::include:
        return "include";
    case DependencyKind::import:
        return "import";
    case DependencyKind::mesh:
        return "mesh";
    case DependencyKind::image:
        return "image";
    case DependencyKind::spectrum:
        return "spectrum";
    case DependencyKind::scene:
        return "scene";
    }
    return "input";
}

} // namespace

int Result::exit_code() const {
    const auto has = [&](Severity severity) {
        return std::any_of(document.diagnostics.begin(), document.diagnostics.end(),
                           [&](const auto &diagnostic) { return diagnostic.severity == severity; });
    };
    if (has(Severity::error) || document.diagnostics_truncated) {
        return 1;
    }
    if (has(Severity::unsupported)) {
        return 2;
    }
    return document.syntax_complete && document.dependencies_complete && structure_complete ? 0 : 1;
}

Result check(const std::filesystem::path &entry, const Limits &limits) {
    try {
        return interpret(parse_file(entry, limits), limits);
    } catch (const std::exception &error) {
        Result result;
        result.document.syntax_complete = false;
        result.document.dependencies_complete = false;
        diagnose(result.document, limits, Severity::error, "analysis-failure", {entry.string()},
                 error.what());
        return result;
    }
}

std::string format_diagnostic(const Diagnostic &diagnostic) {
    std::ostringstream output;
    output << printable(*diagnostic.location.file) << ':' << diagnostic.location.line << ':'
           << diagnostic.location.column << ": " << severity_name(diagnostic.severity) << " ["
           << diagnostic.code << "]: " << printable(diagnostic.message) << '\n';
    return output.str();
}

std::string report(const Result &result) {
    std::ostringstream output;
    for (const auto &diagnostic : result.document.diagnostics) {
        output << format_diagnostic(diagnostic);
    }
    output << "PBRT v4 preflight (structural profile 1)\n"
           << "Syntax: " << (result.document.syntax_complete ? "complete" : "incomplete") << '\n'
           << "Checked scene structure: " << (result.structure_complete ? "complete" : "incomplete")
           << '\n'
           << "Dependency inventory: "
           << (result.document.dependencies_complete ? "complete"
                                                     : "incomplete / unchecked content")
           << '\n'
           << "Input dependencies: " << result.document.dependencies.size() << '\n';
    for (const auto &dependency : result.document.dependencies) {
        output << "  " << dependency_name(dependency.kind) << ": "
               << printable(dependency.authored_path) << " -> "
               << printable(dependency.resolved_path.string()) << " ("
               << (dependency.exists ? (dependency.visited ? "visited" : "unvisited / unchecked")
                                     : "missing")
               << ")\n";
    }
    const auto unsupported = std::count_if(
        result.document.diagnostics.begin(), result.document.diagnostics.end(),
        [](const auto &diagnostic) { return diagnostic.severity == Severity::unsupported; });
    output << "Unsupported features: " << unsupported << '\n'
           << "Inventory: " << result.inventory.named_materials << " named materials, "
           << result.inventory.meshes << " meshes, " << result.inventory.triangles << " triangles, "
           << result.inventory.instances << " object placements\n"
           << "Rendering was not attempted.\n";
    return output.str();
}

} // namespace rypath::pbrt
