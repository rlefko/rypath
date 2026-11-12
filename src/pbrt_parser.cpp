// SPDX-License-Identifier: Apache-2.0
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// Modified for rypath. See third_party/pbrt/README.md.

#include "pbrt.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>

namespace rypath::pbrt {

void diagnose(Document &document, const Limits &limits, Severity severity, std::string code,
              const Location &location, std::string message) {
    if (document.diagnostics_truncated) {
        return;
    }
    if (document.diagnostics.size() < limits.diagnostics) {
        document.diagnostics.push_back({severity, std::move(code), location, std::move(message)});
        return;
    }
    document.diagnostics_truncated = true;
    document.dependencies_complete = false;
    if (!document.diagnostics.empty()) {
        document.diagnostics.back() = {Severity::error, "diagnostic-limit", location,
                                       "diagnostic limit exceeded"};
    }
}

namespace {

// A fatal lexical/typed error leaves no reliable token boundary for recovery.
struct StopParsing {};

enum class TokenKind { word, quoted, open, close, end };
struct Token {
    TokenKind kind;
    Location location;
    std::string text;
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Adapted from PBRT's decodeEscaped and Tokenizer::Next. Here a token owns its
// text, and locations share the owning file name with the frame.
std::optional<char> decode_escape(char c) {
    switch (c) {
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case '\\':
    case '\'':
    case '"':
        return c;
    default:
        return {};
    }
}

class Lexer {
  public:
    Lexer(std::filesystem::path identity, std::string input, Location start)
        : identity(std::move(identity)), input_(std::move(input)), current_(std::move(start)) {}

    Token next(Document &document, const Limits &limits) {
        while (!at_end()) {
            if (is_space(peek())) {
                advance();
            } else if (peek() == '#') {
                while (!at_end() && peek() != '\n' && peek() != '\r') {
                    if (peek() == '\0') {
                        fail(document, limits, current_, "invalid-encoding", "NUL byte in input");
                    }
                    advance();
                }
            } else {
                break;
            }
        }
        const Location location = current_;
        const std::size_t start = offset_;
        if (at_end()) {
            return {TokenKind::end, location, {}};
        }
        if (peek() == '"') {
            std::string decoded;
            advance_token(document, limits, location, start);
            while (!at_end() && peek() != '"' && peek() != '\n' && peek() != '\r') {
                char value = peek();
                advance_token(document, limits, location, start);
                if (value == '\\') {
                    if (at_end() || peek() == '\n' || peek() == '\r') {
                        fail(document, limits, location, "invalid-string",
                             "unterminated quoted string");
                    }
                    const auto escaped = decode_escape(peek());
                    if (!escaped) {
                        fail(document, limits, current_, "invalid-escape",
                             "unknown escape in quoted string");
                    }
                    value = *escaped;
                    advance_token(document, limits, location, start);
                }
                decoded.push_back(value);
            }
            if (at_end() || peek() != '"') {
                fail(document, limits, location, "invalid-string", "unterminated quoted string");
            }
            advance_token(document, limits, location, start);
            return {TokenKind::quoted, location, std::move(decoded)};
        }
        if (peek() == '[' || peek() == ']') {
            const TokenKind kind = peek() == '[' ? TokenKind::open : TokenKind::close;
            advance_token(document, limits, location, start);
            return {kind, location, {}};
        }
        do {
            advance_token(document, limits, location, start);
        } while (!at_end() && !is_space(peek()) && peek() != '"' && peek() != '[' && peek() != ']');
        return {TokenKind::word, location, input_.substr(start, offset_ - start)};
    }

    std::filesystem::path identity;

  private:
    std::string input_;
    Location current_;
    std::size_t offset_ = 0;

    bool at_end() const { return offset_ == input_.size(); }
    char peek() const { return input_[offset_]; }

    void advance() {
        const char c = input_[offset_++];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && !at_end() && peek() == '\n') {
                ++offset_;
            }
            ++current_.line;
            current_.column = 1;
        } else {
            ++current_.column;
        }
    }

    [[noreturn]] static void fail(Document &document, const Limits &limits,
                                  const Location &location, const char *code, const char *message) {
        diagnose(document, limits, Severity::error, code, location, message);
        throw StopParsing{};
    }

    void advance_token(Document &document, const Limits &limits, const Location &start,
                       std::size_t start_offset) {
        if (offset_ - start_offset == limits.token_bytes) {
            fail(document, limits, start, "limit-exceeded", "token byte limit exceeded");
        }
        if (peek() == '\0') {
            fail(document, limits, current_, "invalid-encoding", "NUL byte in input");
        }
        advance();
    }
};

enum class Operands { none, quoted, medium, texture, numbers, matrix, active, option };
struct DirectiveRule {
    std::string_view name;
    Operands operands = Operands::none;
    std::size_t numeric_count = 0;
    bool parameters = false;
    bool legacy = false;
};

constexpr auto rules = std::to_array<DirectiveRule>({
    {"Accelerator", Operands::quoted, 0, true},
    {"ActiveTransform", Operands::active},
    {"AreaLightSource", Operands::quoted, 0, true},
    {"Attribute", Operands::quoted, 0, true},
    {"AttributeBegin"},
    {"AttributeEnd"},
    {"Camera", Operands::quoted, 0, true},
    {"ColorSpace", Operands::quoted},
    {"ConcatTransform", Operands::matrix, 16},
    {"CoordinateSystem", Operands::quoted},
    {"CoordSysTransform", Operands::quoted},
    {"Film", Operands::quoted, 0, true},
    {"Identity"},
    {"Import", Operands::quoted},
    {"Include", Operands::quoted},
    {"Integrator", Operands::quoted, 0, true},
    {"LightSource", Operands::quoted, 0, true},
    {"LookAt", Operands::numbers, 9},
    {"MakeNamedMaterial", Operands::quoted, 0, true},
    {"MakeNamedMedium", Operands::quoted, 0, true},
    {"Material", Operands::quoted, 0, true},
    {"MediumInterface", Operands::medium},
    {"NamedMaterial", Operands::quoted},
    {"ObjectBegin", Operands::quoted},
    {"ObjectEnd"},
    {"ObjectInstance", Operands::quoted},
    {"Option", Operands::option},
    {"PixelFilter", Operands::quoted, 0, true},
    {"ReverseOrientation"},
    {"Rotate", Operands::numbers, 4},
    {"Sampler", Operands::quoted, 0, true},
    {"Scale", Operands::numbers, 3},
    {"Shape", Operands::quoted, 0, true},
    {"Texture", Operands::texture, 0, true},
    {"Transform", Operands::matrix, 16},
    {"TransformBegin", Operands::none, 0, false, true},
    {"TransformEnd", Operands::none, 0, false, true},
    {"TransformTimes", Operands::numbers, 2},
    {"Translate", Operands::numbers, 3},
    {"WorldBegin"},
    {"WorldEnd", Operands::none, 0, false, true},
});

enum class ParameterType { boolean, string, integer, number, spectrum };
struct TypeRule {
    std::string_view name;
    ParameterType type;
    std::size_t tuple = 1;
};

constexpr auto types = std::to_array<TypeRule>({
    {"bool", ParameterType::boolean},
    {"string", ParameterType::string},
    {"texture", ParameterType::string},
    {"integer", ParameterType::integer},
    {"float", ParameterType::number},
    {"point2", ParameterType::number, 2},
    {"vector2", ParameterType::number, 2},
    {"point3", ParameterType::number, 3},
    {"vector3", ParameterType::number, 3},
    {"normal", ParameterType::number, 3},
    {"normal3", ParameterType::number, 3},
    {"rgb", ParameterType::number, 3},
    {"blackbody", ParameterType::number},
    {"spectrum", ParameterType::spectrum, 2},
    {"point", ParameterType::number, 3},
    {"vector", ParameterType::number, 3},
    {"color", ParameterType::number, 3},
});

std::string_view take_word(std::string_view &text) {
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos) {
        text = {};
        return {};
    }
    text.remove_prefix(start);
    const auto end = text.find_first_of(" \t");
    const auto word = text.substr(0, end);
    text.remove_prefix(word.size());
    return word;
}

std::size_t skip_digits(std::string_view text, std::size_t position) {
    while (position < text.size() && is_digit(text[position])) {
        ++position;
    }
    return position;
}

bool is_decimal(std::string_view text) {
    if (text.starts_with('+') || text.starts_with('-')) {
        text.remove_prefix(1);
    }
    std::size_t position = skip_digits(text, 0);
    bool has_digits = position != 0;
    if (position < text.size() && text[position] == '.') {
        ++position;
        const std::size_t fractional_end = skip_digits(text, position);
        has_digits = has_digits || fractional_end != position;
        position = fractional_end;
    }
    if (has_digits && position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        const std::size_t exponent_end = skip_digits(text, position);
        if (exponent_end == position) {
            return false;
        }
        position = exponent_end;
    }
    return has_digits && position == text.size();
}

bool is_decimal_integer(std::string_view text) {
    if (text.starts_with('+') || text.starts_with('-')) {
        text.remove_prefix(1);
    }
    return !text.empty() && skip_digits(text, 0) == text.size();
}

bool option_is(const std::string_view authored, std::string_view normalized) {
    for (const char character : authored) {
        if (character == '-' || character == '_') {
            continue;
        }
        const char lower = character >= 'A' && character <= 'Z'
                               ? static_cast<char>(character + ('a' - 'A'))
                               : character;
        if (normalized.empty() || lower != normalized.front()) {
            return false;
        }
        normalized.remove_prefix(1);
    }
    return normalized.empty();
}

bool is_hex(std::string_view text) {
    if (text.starts_with('+') || text.starts_with('-')) {
        text.remove_prefix(1);
    }
    return text.starts_with("0x") || text.starts_with("0X");
}

bool valid_hex(std::string_view text) {
    if (text.starts_with('+') || text.starts_with('-')) {
        text.remove_prefix(1);
    }
    text.remove_prefix(2); // Called only for an already recognized 0x prefix.
    const auto digit = [](char c) {
        return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    std::size_t position = 0;
    std::size_t digits = 0;
    while (position < text.size() && digit(text[position])) {
        ++position;
        ++digits;
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        while (position < text.size() && digit(text[position])) {
            ++position;
            ++digits;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (position < text.size() && (text[position] == 'p' || text[position] == 'P')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        const auto end = skip_digits(text, position);
        if (end == position) {
            return false;
        }
        position = end;
    }
    return position == text.size();
}

bool nonzero_significand(std::string_view text) {
    return text.substr(0, text.find_first_of("eE")).find_first_of("123456789") !=
           std::string_view::npos;
}

enum class LoadResult { unvisited, gzip, text };

class Reader {
  public:
    explicit Reader(const Limits &limits) : limits_(limits) {}

    Document read(const std::filesystem::path &entry) {
        const Location entry_location(entry.string());
        try {
            std::error_code error;
            const auto absolute = std::filesystem::absolute(entry, error);
            if (error) {
                dependency_error(entry_location, "unreadable-file",
                                 "cannot resolve entry path: " + error.message());
                document_.syntax_complete = false;
                return std::move(document_);
            }
            document_.entry_directory = absolute.parent_path();
            Dependency source{entry_location, DependencyKind::scene, entry.string(), absolute};
            const auto result = inspect(source) ? load(source) : LoadResult::unvisited;
            if (!source.visited) {
                document_.syntax_complete = false;
                if (result == LoadResult::gzip) {
                    document_.dependencies.push_back(std::move(source));
                }
            }
            while (!document_.diagnostics_truncated) {
                Token token = take();
                if (token.kind == TokenKind::end) {
                    break;
                }
                read_directive(std::move(token));
            }
        } catch (const StopParsing &) {
            document_.syntax_complete = false;
            document_.dependencies_complete = false;
        }
        if (document_.diagnostics_truncated) {
            document_.syntax_complete = false;
        }
        return std::move(document_);
    }

  private:
    const Limits &limits_;
    Document document_;
    std::vector<Lexer> files_;
    std::optional<Token> lookahead_;
    bool unknown_include_boundary_ = false;

    [[noreturn]] void fail(const Location &location, std::string code, std::string message,
                           Severity severity = Severity::error) {
        diagnose(document_, limits_, severity, std::move(code), location, std::move(message));
        throw StopParsing{};
    }

    void dependency_error(const Location &location, std::string code, std::string message,
                          Severity severity = Severity::error) {
        document_.dependencies_complete = false;
        diagnose(document_, limits_, severity, std::move(code), location, std::move(message));
    }

    bool inspect(Dependency &dependency) {
        const auto &location = dependency.location;
        if (dependency.authored_path.empty()) {
            dependency_error(location, "missing-file", "empty scene file path");
            return false;
        }
        std::error_code error;
        const auto status = std::filesystem::status(dependency.resolved_path, error);
        dependency.exists = std::filesystem::exists(status);
        if (!dependency.exists) {
            if (error && error != std::errc::no_such_file_or_directory &&
                error != std::errc::not_a_directory) {
                dependency_error(location, "unreadable-file",
                                 "cannot inspect scene file: " + dependency.resolved_path.string() +
                                     " (" + error.message() + ")");
            } else {
                dependency_error(location, "missing-file",
                                 "missing scene file: " + dependency.resolved_path.string());
            }
            return false;
        }
        if (!std::filesystem::is_regular_file(status)) {
            dependency_error(location, "unreadable-file",
                             "scene path is not a regular file: " +
                                 dependency.resolved_path.string());
            return false;
        }
        return true;
    }

    LoadResult gzip(const Dependency &source) {
        dependency_error(source.location, "unsupported-gzip",
                         "gzip scene input is outside this preflight profile",
                         Severity::unsupported);
        return LoadResult::gzip;
    }

    LoadResult load(Dependency &source) {
        if (document_.diagnostics_truncated) {
            return LoadResult::unvisited;
        }
        const auto &path = source.resolved_path;
        if (path.extension() == ".gz") {
            return gzip(source);
        }

        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error) {
            dependency_error(source.location, "unreadable-file",
                             "cannot size scene file: " + path.string() + " (" + error.message() +
                                 ")");
            return LoadResult::unvisited;
        }
        const auto identity = std::filesystem::canonical(path, error);
        if (error) {
            dependency_error(source.location, "unreadable-file",
                             "cannot resolve scene file: " + path.string() + " (" +
                                 error.message() + ")");
            return LoadResult::unvisited;
        }
        if (std::ranges::any_of(files_,
                                [&](const Lexer &file) { return file.identity == identity; })) {
            dependency_error(source.location, "include-cycle",
                             "active Include cycle through " + path.string());
            return LoadResult::unvisited;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            dependency_error(source.location, "unreadable-file",
                             "cannot open scene file: " + path.string());
            return LoadResult::unvisited;
        }
        if (size >= 2) {
            std::array<unsigned char, 2> magic{};
            if (!input.read(reinterpret_cast<char *>(magic.data()),
                            static_cast<std::streamsize>(magic.size()))) {
                dependency_error(source.location, "unreadable-file",
                                 "cannot read scene file: " + path.string());
                return LoadResult::unvisited;
            }
            if (magic[0] == 0x1f && magic[1] == 0x8b) {
                return gzip(source);
            }
            input.seekg(0);
        }
        if (size > limits_.file_bytes || size > std::string{}.max_size() ||
            size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
            fail(source.location, "limit-exceeded", "scene file byte limit exceeded");
        }
        if (size > limits_.expanded_bytes - document_.expanded_bytes) {
            fail(source.location, "limit-exceeded", "expanded scene byte limit exceeded");
        }
        if (files_.size() >= limits_.active_files) {
            fail(source.location, "limit-exceeded", "active scene file limit exceeded");
        }
        if (document_.expansion_count >= limits_.expansions) {
            fail(source.location, "limit-exceeded", "scene file expansion limit exceeded");
        }
        std::string data(static_cast<std::size_t>(size), '\0');
        if (!input.read(data.data(), static_cast<std::streamsize>(data.size())) ||
            input.peek() != std::char_traits<char>::eof() || input.bad()) {
            dependency_error(source.location, "unreadable-file",
                             "scene file changed or could not be read: " + path.string());
            return LoadResult::unvisited;
        }
        const Location location(path.string());
        if (std::string_view(data).starts_with("\xfe\xff") ||
            std::string_view(data).starts_with("\xff\xfe")) {
            fail(location, "invalid-encoding", "UTF-16 scene files are unsupported");
        }
        document_.expanded_bytes += data.size();
        ++document_.expansion_count;
        files_.emplace_back(identity, std::move(data), location);
        source.visited = true;
        return LoadResult::text;
    }

    // PBRT's Include is a stack of tokenizers. End of an included file
    // exposes the following parent token, including during parameter parsing.
    Token next() {
        Token end{TokenKind::end, {}, {}};
        while (!files_.empty()) {
            auto token = files_.back().next(document_, limits_);
            if (token.kind != TokenKind::end) {
                return token;
            }
            end = std::move(token);
            files_.pop_back();
        }
        return end;
    }

    const Token &peek() {
        if (!lookahead_) {
            lookahead_ = next();
        }
        return *lookahead_;
    }

    Token take() {
        if (!lookahead_) {
            return next();
        }
        Token token = std::move(*lookahead_);
        lookahead_.reset();
        return token;
    }

    Token required(const Location &owner) {
        Token token = take();
        if (token.kind == TokenKind::end) {
            fail(token.location.file && !token.location.file->empty() ? token.location : owner,
                 "unexpected-eof",
                 "unexpected end of scene while reading input begun at " + *owner.file + ":" +
                     std::to_string(owner.line) + ":" + std::to_string(owner.column));
        }
        return token;
    }

    Token expect(TokenKind kind, const Location &owner, const char *message) {
        auto token = required(owner);
        if (token.kind != kind) {
            fail(token.location, "expected-token", message);
        }
        return token;
    }

    void check_hex(const Token &token) {
        if (token.kind == TokenKind::word && is_hex(token.text)) {
            if (!valid_hex(token.text)) {
                fail(token.location, "invalid-number", "malformed hexadecimal numeric literal");
            }
            fail(token.location, "unsupported-number",
                 "hexadecimal numbers are outside this preflight profile", Severity::unsupported);
        }
    }

    double number(const Token &token) {
        check_hex(token);
        if (token.kind != TokenKind::word || !is_decimal(token.text)) {
            fail(token.location, "invalid-number", "expected a decimal floating-point value");
        }
        std::istringstream input(token.text);
        input.imbue(std::locale::classic());
        double value = 0;
        input >> value;
        // libc++ can set failbit for a successfully rounded subnormal.
        if (!input.eof() || !std::isfinite(value) ||
            (input.fail() && std::fpclassify(value) != FP_SUBNORMAL) ||
            (value == 0 && nonzero_significand(token.text))) {
            fail(token.location, "invalid-number",
                 "floating-point value is outside binary64 range");
        }
        return value;
    }

    std::int32_t integer(const Token &token) {
        check_hex(token);
        std::string_view digits(token.text);
        if (digits.starts_with('+')) {
            digits.remove_prefix(1);
            if (digits.starts_with('-')) {
                fail(token.location, "invalid-integer", "expected a signed decimal 32-bit integer");
            }
        }
        std::int32_t value = 0;
        const char *const end = digits.data() + digits.size();
        const auto [next_digit, error] = std::from_chars(digits.data(), end, value);
        if (token.kind != TokenKind::word || error != std::errc{} || next_digit != end) {
            fail(token.location, "invalid-integer", "expected a signed decimal 32-bit integer");
        }
        return value;
    }

    void value(std::vector<Value> &output, Token token, ParameterType type) {
        if (document_.value_count >= limits_.total_values) {
            fail(token.location, "limit-exceeded", "total scene value limit exceeded");
        }
        Value value;
        value.location = token.location;
        switch (type) {
        case ParameterType::string:
            if (token.kind != TokenKind::quoted) {
                fail(token.location, "invalid-value", "expected a quoted string value");
            }
            value.kind = ValueKind::string;
            break;
        case ParameterType::boolean:
            if (token.kind != TokenKind::word || (token.text != "true" && token.text != "false")) {
                fail(token.location, "invalid-value", "expected unquoted true or false");
            }
            value.kind = ValueKind::boolean;
            value.boolean = token.text == "true";
            break;
        case ParameterType::integer:
            value.kind = ValueKind::integer;
            value.integer = integer(token);
            break;
        case ParameterType::number:
        case ParameterType::spectrum:
            value.kind = ValueKind::number;
            value.number = number(token);
            break;
        }
        value.text = std::move(token.text);
        output.push_back(std::move(value));
        ++document_.value_count;
    }

    Parameter raw_option(const Token &declaration) {
        auto operand = required(declaration.location);
        if (operand.kind == TokenKind::open || operand.kind == TokenKind::close) {
            fail(operand.location, "invalid-list", "Option requires one unbracketed value");
        }
        Parameter output{declaration.location, "float", declaration.text, {}};
        auto type = ParameterType::number;
        if (operand.kind == TokenKind::quoted) {
            type = ParameterType::string;
            output.type = "string";
        } else if (operand.text == "true" || operand.text == "false") {
            type = ParameterType::boolean;
            output.type = "bool";
        } else if (option_is(output.name, "seed") ||
                   (!option_is(output.name, "displacementedgescale") &&
                    is_decimal_integer(operand.text))) {
            type = ParameterType::integer;
            output.type = "integer";
        }
        value(output.values, std::move(operand), type);
        return output;
    }

    Parameter parameter(const Location &owner, bool scalar_only) {
        const auto declaration =
            expect(TokenKind::quoted, owner, "expected a quoted parameter declaration");
        std::string_view text(declaration.text);
        const auto type_name = take_word(text);
        const auto name = take_word(text);
        // The format reference documents a typed declaration, while the pinned
        // BasicSceneBuilder recognizes a name by itself.
        if (scalar_only && !type_name.empty() && name.empty()) {
            return raw_option(declaration);
        }
        if (type_name.empty() || name.empty() || !take_word(text).empty()) {
            fail(declaration.location, "invalid-parameter", "expected a parameter type and name");
        }
        const auto *rule = std::ranges::find(types, type_name, &TypeRule::name);
        if (rule == types.end()) {
            fail(declaration.location, "invalid-parameter",
                 "unknown parameter type: " + std::string(type_name));
        }
        Parameter output{declaration.location, std::string(type_name), std::string(name), {}};
        const bool bracketed = peek().kind == TokenKind::open;
        if (bracketed && scalar_only) {
            fail(peek().location, "invalid-list", "Option requires one unbracketed value");
        }
        if (bracketed) {
            take();
        }
        auto type = rule->type;
        std::size_t tuple = rule->tuple;
        if (type == ParameterType::spectrum && peek().kind == TokenKind::quoted) {
            type = ParameterType::string;
            tuple = 1;
        }
        if (!bracketed) {
            value(output.values, required(declaration.location), type);
        } else {
            while (peek().kind != TokenKind::close) {
                if (output.values.size() >= limits_.list_values) {
                    fail(peek().location, "limit-exceeded", "parameter list value limit exceeded");
                }
                if (peek().kind == TokenKind::open) {
                    fail(peek().location, "invalid-list", "nested parameter list");
                }
                value(output.values, required(declaration.location), type);
            }
            take();
        }
        if (output.values.empty() || output.values.size() % tuple != 0) {
            fail(declaration.location, "invalid-list",
                 "parameter requires one or more complete tuples of " + std::to_string(tuple) +
                     " value(s)");
        }
        if (type_name == "point" || type_name == "vector" || type_name == "color") {
            diagnose(document_, limits_, Severity::unsupported, "legacy-parameter",
                     declaration.location, "legacy PBRT parameter type: " + std::string(type_name));
        }
        return output;
    }

    void quoted(Directive &directive) {
        value(directive.arguments,
              expect(TokenKind::quoted, directive.location, "expected a quoted directive operand"),
              ParameterType::string);
    }

    void arguments(Directive &directive, const DirectiveRule &rule) {
        switch (rule.operands) {
        case Operands::none:
            break;
        case Operands::quoted:
            quoted(directive);
            break;
        case Operands::medium:
            quoted(directive);
            if (peek().kind == TokenKind::quoted) {
                quoted(directive);
            }
            break;
        case Operands::texture:
            for (int i = 0; i < 3; ++i) {
                quoted(directive);
            }
            break;
        case Operands::matrix:
        case Operands::numbers: {
            const bool matrix = rule.operands == Operands::matrix;
            if (matrix) {
                expect(TokenKind::open, directive.location, "expected '[' before transform matrix");
            }
            for (std::size_t i = 0; i < rule.numeric_count; ++i) {
                if (matrix && i >= limits_.list_values) {
                    fail(peek().location, "limit-exceeded", "transform list value limit exceeded");
                }
                value(directive.arguments, required(directive.location), ParameterType::number);
            }
            if (matrix) {
                expect(TokenKind::close, directive.location, "expected ']' after 16 matrix values");
            }
            break;
        }
        case Operands::active: {
            auto mode =
                expect(TokenKind::word, directive.location, "expected ActiveTransform mode");
            if (mode.text != "All" && mode.text != "StartTime" && mode.text != "EndTime") {
                fail(mode.location, "invalid-value", "expected All, StartTime, or EndTime");
            }
            mode.kind = TokenKind::quoted;
            value(directive.arguments, std::move(mode), ParameterType::string);
            break;
        }
        case Operands::option:
            directive.parameters.push_back(parameter(directive.location, true));
            break;
        }
    }

    void read_directive(Token token) {
        // An unvisited textual include may end partway through a directive.
        // Its parent's remaining operands have unknown arity; retain the prior
        // unavailable-content diagnostic and withhold that suffix.
        if (unknown_include_boundary_ &&
            (token.kind != TokenKind::word ||
             std::ranges::find(rules, token.text, &DirectiveRule::name) == rules.end())) {
            throw StopParsing{};
        }
        unknown_include_boundary_ = false;
        if (token.kind != TokenKind::word) {
            fail(token.location, "expected-token", "expected a scene directive");
        }
        const auto *rule = std::ranges::find(rules, token.text, &DirectiveRule::name);
        if (rule == rules.end()) {
            fail(token.location, "unknown-directive", "unknown directive: " + token.text);
        }
        if (document_.directives.size() >= limits_.directives) {
            fail(token.location, "limit-exceeded", "scene directive limit exceeded");
        }
        Directive directive{std::move(token.location), std::move(token.text), {}, {}};
        arguments(directive, *rule);
        if (rule->parameters) {
            while (!document_.diagnostics_truncated && peek().kind == TokenKind::quoted) {
                directive.parameters.push_back(parameter(directive.location, false));
            }
        }
        document_.directives.push_back(std::move(directive));
        const auto &stored = document_.directives.back();
        if (rule->legacy) {
            diagnose(document_, limits_, Severity::unsupported, "legacy-directive", stored.location,
                     "legacy PBRT directive: " + stored.name);
        }
        if (stored.name == "Include" || stored.name == "Import") {
            dependency(stored, stored.name == "Import");
        }
    }

    void dependency(const Directive &directive, bool import) {
        const auto &operand = directive.arguments.front();
        const auto resolved = document_.entry_directory / operand.text;
        Dependency source{operand.location,
                          import ? DependencyKind::import : DependencyKind::include, operand.text,
                          resolved};
        const bool regular = inspect(source);
        document_.dependencies.push_back(std::move(source));
        if (import) {
            dependency_error(directive.location, "unsupported-import",
                             "Import requires isolated scene state and is outside this profile",
                             Severity::unsupported);
        } else if (!regular || load(document_.dependencies.back()) != LoadResult::text) {
            unknown_include_boundary_ = true;
            document_.syntax_complete = false;
        }
    }
};

} // namespace

Document parse_file(const std::filesystem::path &entry, const Limits &limits) {
    return Reader(limits).read(entry);
}

} // namespace rypath::pbrt
