// SPDX-License-Identifier: GPL-3.0-only

#include "spectrum_file.hpp"
#include "pbrt_numeric.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <new>

namespace rypath::pbrt {
namespace {

bool newline(char c) { return c == '\n' || c == '\r'; }

bool whitespace(char c) { return newline(c) || c == ' ' || c == '\t' || c == '\f' || c == '\v'; }

class Decoder {
  public:
    Decoder(std::string_view input, const Limits &limits, PlyBudget &remaining)
        : input_(input), limits_(limits), remaining_(remaining) {}

    std::vector<SpectralKnot> decode();

  private:
    std::string_view input_;
    const Limits &limits_;
    PlyBudget &remaining_;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
    std::size_t values_ = 0;

    void skip_separators();
    float value();
};

void Decoder::skip_separators() {
    while (!input_.empty()) {
        const char c = input_.front();
        if (c == '#') {
            const auto end = input_.find_first_of("\r\n");
            const auto count = std::min(end, input_.size());
            input_.remove_prefix(count);
            column_ += count;
        } else if (newline(c)) {
            input_.remove_prefix(input_.starts_with("\r\n") ? 2 : 1);
            ++line_;
            column_ = 1;
        } else if (whitespace(c)) {
            input_.remove_prefix(1);
            ++column_;
        } else {
            return;
        }
    }
}

float Decoder::value() {
    if (values_ == limits_.list_values || remaining_.values == 0) {
        throw SpectrumFileIssue{"limit-exceeded", "Spectrum input value limit exceeded", line_,
                                column_};
    }
    ++values_;
    --remaining_.values;
    std::size_t length = 0;
    while (length < input_.size() && !whitespace(input_[length]) && input_[length] != '#') {
        if (length == limits_.token_bytes) {
            throw SpectrumFileIssue{"limit-exceeded", "Spectrum input token byte limit exceeded",
                                    line_, column_};
        }
        ++length;
    }
    if (const auto parsed = decimal_float<float>(input_.substr(0, length))) {
        input_.remove_prefix(length);
        column_ += length;
        return *parsed;
    }
    throw SpectrumFileIssue{"invalid-spectrum-file", "Expected a finite binary32 decimal number",
                            line_, column_};
}

std::vector<SpectralKnot> Decoder::decode() {
    std::vector<SpectralKnot> knots;
    skip_separators();
    while (!input_.empty()) {
        const auto wavelength_column = column_;
        const auto wavelength = value();
        if (!knots.empty() && wavelength <= knots.back().wavelength) {
            throw SpectrumFileIssue{"invalid-spectrum-file",
                                    "Spectrum wavelengths must strictly increase in binary32",
                                    line_, wavelength_column};
        }
        skip_separators();
        if (input_.empty()) {
            throw SpectrumFileIssue{"invalid-spectrum-file", "Missing value for wavelength", line_,
                                    column_};
        }
        const auto sample = value();
        knots.push_back({wavelength, sample});
        skip_separators();
    }
    if (knots.size() < 2) {
        throw SpectrumFileIssue{"invalid-spectrum-file", "Spectrum requires at least two pairs",
                                line_, column_};
    }
    return knots;
}

} // namespace

SpectrumFileResult decode_spectrum_file(const std::filesystem::path &path, const Limits &limits,
                                        PlyBudget &remaining) {
    if (remaining.files == 0) {
        return SpectrumFileIssue{"limit-exceeded", "Spectrum input file count limit exceeded"};
    }
    --remaining.files;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return SpectrumFileIssue{"unreadable-file",
                                 "Cannot size spectrum input: " + error.message()};
    }
    if (size > limits.file_bytes || size > std::string{}.max_size() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return SpectrumFileIssue{"limit-exceeded", "Spectrum file byte limit exceeded"};
    }
    if (size > remaining.bytes) {
        return SpectrumFileIssue{"limit-exceeded", "Expanded input byte limit exceeded"};
    }
    remaining.bytes -= static_cast<std::size_t>(size);
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return SpectrumFileIssue{"unreadable-file", "Cannot open spectrum input"};
        }
        std::string data(static_cast<std::size_t>(size), '\0');
        if (!input.read(data.data(), static_cast<std::streamsize>(data.size())) ||
            input.peek() != std::char_traits<char>::eof() || input.bad()) {
            return SpectrumFileIssue{"unreadable-file",
                                     "Spectrum input changed or could not be read"};
        }
        auto knots = Decoder(data, limits, remaining).decode();
        return std::make_shared<const CheckedSpectrumSource>(
            CheckedSpectrumSource{path, std::move(knots)});
    } catch (SpectrumFileIssue &issue) {
        return std::move(issue);
    } catch (const std::bad_alloc &) {
        return SpectrumFileIssue{"limit-exceeded", "Cannot allocate spectrum input or samples"};
    }
}

} // namespace rypath::pbrt
