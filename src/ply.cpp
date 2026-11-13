// SPDX-License-Identifier: GPL-3.0-only

#include "ply.hpp"
#include "pbrt_numeric.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <type_traits>

namespace rypath::pbrt {
namespace {

using std::string_view;

enum class Encoding { ascii, little_endian, big_endian };
enum class Scalar { int8, uint8, int16, uint16, int32, uint32, float32, float64 };

std::optional<Scalar> scalar_type(string_view name) {
    constexpr std::array names{
        std::pair{"char", "int8"},     std::pair{"uchar", "uint8"},    std::pair{"short", "int16"},
        std::pair{"ushort", "uint16"}, std::pair{"int", "int32"},      std::pair{"uint", "uint32"},
        std::pair{"float", "float32"}, std::pair{"double", "float64"},
    };
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (name == names[i].first || name == names[i].second) {
            return static_cast<Scalar>(i);
        }
    }
    return std::nullopt;
}

bool integral(Scalar type) { return type != Scalar::float32 && type != Scalar::float64; }

std::size_t scalar_bytes(Scalar type) {
    constexpr std::array<std::size_t, 8> widths{1, 1, 2, 2, 4, 4, 4, 8};
    return widths[static_cast<std::size_t>(type)];
}

enum class Attribute { ignored, x, y, z, nx, ny, nz, u, v, indices };

struct Property {
    string_view name;
    Scalar type;
    std::optional<Scalar> list_count;
    Attribute attribute = Attribute::ignored;
};

struct Element {
    string_view name;
    std::size_t rows;
    std::vector<Property> properties{};
};

bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

string_view next_token(string_view &input) {
    while (!input.empty() && whitespace(input.front())) {
        input.remove_prefix(1);
    }
    const auto end = std::ranges::find_if(input, whitespace);
    const auto size = static_cast<std::size_t>(end - input.begin());
    const auto token = input.substr(0, size);
    input.remove_prefix(size);
    return token;
}

template <typename T> std::optional<T> decimal_integer(string_view token) {
    if (token.starts_with('+')) {
        token.remove_prefix(1);
        if (token.starts_with('-')) {
            return std::nullopt;
        }
    }
    if (token.empty()) {
        return std::nullopt;
    }
    T result{};
    const auto end = token.data() + token.size();
    const auto [ptr, error] = std::from_chars(token.data(), end, result);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return result;
}

template <typename T> std::optional<T> decimal_float(string_view token) {
    if (!is_decimal(token)) {
        return std::nullopt;
    }
    if (!nonzero_significand(token)) {
        return token.starts_with('-') ? -T{0} : T{0};
    }
    // Floating from_chars is unavailable on the supported macOS deployment target.
    std::istringstream input{std::string(token)};
    input.imbue(std::locale::classic());
    T result = 0;
    input >> result;
    if (!input.eof() || !std::isfinite(result) || result == 0 ||
        (input.fail() && std::fpclassify(result) != FP_SUBNORMAL)) {
        return std::nullopt;
    }
    return result;
}

class Decoder {
  public:
    Decoder(string_view input, const Limits &limits, PlyBudget &remaining)
        : input_(input), limits_(limits), remaining_(remaining) {}

    PlyMesh decode();

  private:
    string_view input_;
    const Limits &limits_;
    PlyBudget &remaining_;
    Encoding encoding_ = Encoding::ascii;
    std::vector<Element> elements_;
    std::size_t offset_ = 0;
    std::size_t header_line_ = 0;
    string_view element_;
    std::size_t row_ = 0;
    std::size_t vertex_count_ = 0;
    std::size_t mesh_values_ = 0;
    bool has_normals_ = false;
    bool has_uvs_ = false;
    GeometryMesh mesh_;
    std::string unsupported_polygon_;

    std::string location() const;
    [[noreturn]] void fail(std::string message, const char *code = "invalid-ply") const {
        throw PlyIssue{Severity::error, code, location() + std::move(message)};
    }
    void read_header();
    void identify_attributes();
    void charge(std::size_t count);
    void check_minimum_payload();
    string_view body_token();
    template <typename T> double ascii_value(string_view token) const;
    double scalar(Scalar type);
    std::uint64_t binary_scalar(Scalar type);
    std::size_t list_length(double length, Scalar item_type);
    void read_row(const Element &element);
    void read_indices(Scalar type, std::size_t count);
};

std::string Decoder::location() const {
    std::string result = "at byte " + std::to_string(offset_);
    if (header_line_ != 0) {
        result += ", header line " + std::to_string(header_line_);
    } else if (!element_.empty()) {
        result += ", " + std::string(element_) + " row " + std::to_string(row_ + 1);
    }
    return result + ": ";
}

void Decoder::read_header() {
    constexpr std::size_t max_header_bytes = std::size_t{256} * 1024;
    const auto header = input_.substr(0, max_header_bytes);
    const auto first_end = header.find_first_of("\r\n");
    if (first_end == string_view::npos) {
        fail("Missing PLY magic line",
             input_.size() > max_header_bytes ? "limit-exceeded" : "invalid-ply");
    }
    const auto newline = header[first_end] == '\n'                      ? "\n"
                         : header.substr(first_end).starts_with("\r\n") ? "\r\n"
                                                                        : "\r";
    const auto read_line = [&]() {
        ++header_line_;
        if (header_line_ > limits_.directives) {
            fail("PLY header line limit exceeded", "limit-exceeded");
        }
        const auto end = header.find(newline, offset_);
        if (end == string_view::npos) {
            fail("Incomplete PLY header",
                 input_.size() > max_header_bytes ? "limit-exceeded" : "invalid-ply");
        }
        const auto line = header.substr(offset_, end - offset_);
        if (line.find('\0') != string_view::npos) {
            fail("NUL byte in PLY header");
        }
        offset_ = end + std::char_traits<char>::length(newline);
        return line;
    };
    const auto token = [&](string_view &line) {
        const auto word = next_token(line);
        if (word.size() > limits_.token_bytes) {
            fail("PLY header token byte limit exceeded", "limit-exceeded");
        }
        return word;
    };
    if (read_line() != "ply") {
        fail("Expected PLY magic");
    }
    auto format_line = read_line();
    const auto format_directive = token(format_line);
    const auto format = token(format_line);
    const auto version = token(format_line);
    if (format_directive != "format" || version != "1.0" || !token(format_line).empty()) {
        fail("Expected a PLY format declaration with version 1.0");
    }
    if (format == "binary_little_endian") {
        encoding_ = Encoding::little_endian;
    } else if (format == "binary_big_endian") {
        encoding_ = Encoding::big_endian;
    } else if (format != "ascii") {
        fail("Unknown PLY encoding");
    }

    std::set<string_view> element_names;
    std::set<string_view> property_names;
    std::size_t rows_remaining = limits_.total_values;
    while (true) {
        auto line = read_line();
        const auto command = token(line);
        if (command == "comment" || command == "obj_info" || command.empty()) {
            continue;
        }
        if (command == "end_header" && token(line).empty()) {
            return;
        }
        if (command == "element") {
            const auto name = token(line);
            const auto count = decimal_integer<std::size_t>(token(line));
            if (name.empty() || !count || !token(line).empty() ||
                !element_names.insert(name).second) {
                fail("Invalid or duplicate PLY element declaration");
            }
            if (*count > rows_remaining || *count > std::numeric_limits<std::uint32_t>::max()) {
                fail("PLY declared row limit exceeded", "limit-exceeded");
            }
            rows_remaining -= *count;
            elements_.push_back({name, *count});
            property_names.clear();
        } else if (command == "property" && !elements_.empty()) {
            auto type_name = token(line);
            std::optional<Scalar> count_type;
            if (type_name == "list") {
                count_type = scalar_type(token(line));
                if (!count_type || !integral(*count_type)) {
                    fail("PLY list count must have an integer type");
                }
                type_name = token(line);
            }
            const auto type = scalar_type(type_name);
            const auto name = token(line);
            if (!type || name.empty() || !token(line).empty() ||
                !property_names.insert(name).second) {
                fail("Invalid or duplicate PLY property declaration");
            }
            elements_.back().properties.push_back({name, *type, count_type});
        } else {
            fail("Unexpected PLY header declaration");
        }
    }
}

void Decoder::identify_attributes() {
    const auto vertex = std::ranges::find(elements_, "vertex", &Element::name);
    const auto face = std::ranges::find(elements_, "face", &Element::name);
    if (vertex == elements_.end() || vertex->rows == 0 || face == elements_.end() ||
        face->rows == 0) {
        fail("PLY requires nonempty vertex and face elements");
    }
    vertex_count_ = vertex->rows;
    const auto bind = [&](auto names, Attribute first, bool required = false) {
        std::size_t present = 0;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto property = std::ranges::find(vertex->properties, names[i], &Property::name);
            if (property != vertex->properties.end()) {
                ++present;
                if (property->list_count) {
                    fail("PLY vertex property \"" + std::string(names[i]) + "\" must be scalar");
                }
                if (first != Attribute::ignored) {
                    property->attribute =
                        static_cast<Attribute>(static_cast<std::size_t>(first) + i);
                }
            }
        }
        if ((required || present != 0) && present != names.size()) {
            fail("Incomplete PLY vertex property group beginning with \"" + std::string(names[0]) +
                 "\"");
        }
        return present != 0;
    };
    bind(std::array{"x", "y", "z"}, Attribute::x, true);
    has_normals_ = bind(std::array{"nx", "ny", "nz"}, Attribute::nx);
    bool any_uv = false;
    for (const auto names :
         {std::array{"u", "v"}, std::array{"s", "t"}, std::array{"texture_u", "texture_v"},
          std::array{"texture_s", "texture_t"}}) {
        const bool first = std::ranges::find(vertex->properties, names[0], &Property::name) !=
                           vertex->properties.end();
        const bool second = std::ranges::find(vertex->properties, names[1], &Property::name) !=
                            vertex->properties.end();
        any_uv |= first || second;
        if (first && second) {
            has_uvs_ = bind(names, Attribute::u);
            break;
        }
    }
    if (any_uv && !has_uvs_) {
        fail("Incomplete PLY vertex UV property group");
    }

    const auto indices = std::ranges::find(face->properties, "vertex_indices", &Property::name);
    if (indices == face->properties.end() || !indices->list_count || !integral(indices->type)) {
        fail("PLY faces require an integer vertex_indices list");
    }
    indices->attribute = Attribute::indices;
}

void Decoder::charge(std::size_t count) {
    if (count > remaining_.values) {
        fail("PLY decoded value limit exceeded", "limit-exceeded");
    }
    remaining_.values -= count;
}

void Decoder::check_minimum_payload() {
    // Each scalar occupies at least one ASCII byte, or exactly its binary width.
    // A list contributes its count here and its items when the count is decoded.
    std::size_t bytes = input_.size() - offset_;
    for (const auto &element : elements_) {
        element_ = element.name;
        const auto properties = element.properties.size();
        if (properties == 0) {
            // A propertyless element contributes neither payload nor decode work.
            continue;
        }
        if (element.rows > remaining_.values / properties) {
            fail("PLY declared row/property value limit exceeded", "limit-exceeded");
        }
        charge(element.rows * properties);
        std::size_t row_bytes = 0;
        for (const auto &property : element.properties) {
            row_bytes += encoding_ == Encoding::ascii
                             ? 1
                             : scalar_bytes(property.list_count.value_or(property.type));
        }
        if (element.rows > bytes / row_bytes) {
            fail("PLY payload is too short for its declared rows and properties");
        }
        bytes -= element.rows * row_bytes;
    }
}

string_view Decoder::body_token() {
    auto body = input_.substr(offset_);
    const auto token = next_token(body);
    if (token.empty()) {
        fail("Unexpected end of ASCII PLY payload");
    }
    offset_ = static_cast<std::size_t>(token.data() - input_.data());
    if (token.size() > limits_.token_bytes) {
        fail("PLY payload token byte limit exceeded", "limit-exceeded");
    }
    return token;
}

std::uint64_t Decoder::binary_scalar(Scalar type) {
    const auto width = scalar_bytes(type);
    if (width > input_.size() - offset_) {
        fail("Unexpected end of binary PLY payload");
    }
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < width; ++i) {
        const auto index = encoding_ == Encoding::big_endian ? i : width - 1 - i;
        bits = (bits << 8) | static_cast<unsigned char>(input_[offset_ + index]);
    }
    return bits;
}

template <typename T> double Decoder::ascii_value(string_view token) const {
    const auto value = [&] {
        if constexpr (std::is_integral_v<T>) {
            return decimal_integer<T>(token);
        } else {
            return decimal_float<T>(token);
        }
    }();
    if (!value) {
        fail("Invalid ASCII PLY scalar or value outside its declared type");
    }
    return *value;
}

double Decoder::scalar(Scalar type) {
    if (encoding_ == Encoding::ascii) {
        const auto token = body_token();
        double value = 0;
        switch (type) {
        case Scalar::int8:
            value = ascii_value<std::int8_t>(token);
            break;
        case Scalar::uint8:
            value = ascii_value<std::uint8_t>(token);
            break;
        case Scalar::int16:
            value = ascii_value<std::int16_t>(token);
            break;
        case Scalar::uint16:
            value = ascii_value<std::uint16_t>(token);
            break;
        case Scalar::int32:
            value = ascii_value<std::int32_t>(token);
            break;
        case Scalar::uint32:
            value = ascii_value<std::uint32_t>(token);
            break;
        case Scalar::float32:
            value = ascii_value<float>(token);
            break;
        case Scalar::float64:
            value = ascii_value<double>(token);
            break;
        }
        offset_ += token.size();
        return value;
    }

    const auto bits = binary_scalar(type);
    double value = 0;
    switch (type) {
    case Scalar::int8:
        value = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(bits));
        break;
    case Scalar::int16:
        value = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(bits));
        break;
    case Scalar::int32:
        value = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
        break;
    case Scalar::uint8:
    case Scalar::uint16:
    case Scalar::uint32:
        value = static_cast<double>(bits);
        break;
    case Scalar::float32:
        value = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
        break;
    case Scalar::float64:
        value = std::bit_cast<double>(bits);
        break;
    }
    if (!std::isfinite(value)) {
        fail("Nonfinite binary PLY scalar");
    }
    offset_ += scalar_bytes(type);
    return value;
}

std::size_t Decoder::list_length(double length, Scalar item_type) {
    if (length < 0) {
        fail("Negative PLY list length");
    }
    const auto count = static_cast<std::size_t>(length);
    if (count > limits_.list_values) {
        fail("PLY list length limit exceeded", "limit-exceeded");
    }
    charge(count);
    const auto width = encoding_ == Encoding::ascii ? 1 : scalar_bytes(item_type);
    if (count > (input_.size() - offset_) / width) {
        fail("PLY list exceeds remaining payload");
    }
    return count;
}

void Decoder::read_indices(Scalar type, std::size_t count) {
    if (count < 3) {
        fail("A PLY face must contain at least three vertex indices");
    }
    if (count > 4 && unsupported_polygon_.empty()) {
        unsupported_polygon_ = location() + "PLY face has " + std::to_string(count) +
                               " vertices. Only triangles and quads are supported";
    }
    std::array<std::uint32_t, 4> indices{};
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = scalar(type);
        if (value < 0 || value >= static_cast<double>(vertex_count_)) {
            fail("PLY vertex index is out of bounds");
        }
        if (i < indices.size()) {
            indices[i] = static_cast<std::uint32_t>(value);
        }
    }
    if (count == 3) {
        mesh_.triangles.push_back({indices[0], indices[1], indices[2]});
        mesh_values_ += 3;
    } else if (count == 4) {
        mesh_.patches.push_back({indices[0], indices[1], indices[3], indices[2]});
        mesh_values_ += 4;
    }
}

void Decoder::read_row(const Element &element) {
    // Retain the numeric slots selected by the header in semantic component order.
    std::array<float, 8> vertex{};
    for (const auto &property : element.properties) {
        if (property.list_count) {
            const auto count = list_length(scalar(*property.list_count), property.type);
            if (property.attribute == Attribute::indices) {
                read_indices(property.type, count);
            } else {
                for (std::size_t i = 0; i < count; ++i) {
                    scalar(property.type);
                }
            }
        } else {
            const auto value = scalar(property.type);
            if (property.attribute != Attribute::ignored) {
                if (std::abs(value) > std::numeric_limits<float>::max()) {
                    fail("PLY vertex property overflows binary32");
                }
                const auto index = static_cast<std::size_t>(property.attribute) - 1;
                vertex[index] = static_cast<float>(value);
            }
        }
    }
    if (element.name == "vertex") {
        mesh_.positions.push_back({vertex[0], vertex[1], vertex[2]});
        if (has_normals_) {
            mesh_.normals.push_back({vertex[3], vertex[4], vertex[5]});
        }
        if (has_uvs_) {
            mesh_.uvs.push_back({vertex[6], vertex[7]});
        }
    }
}

PlyMesh Decoder::decode() {
    read_header();
    identify_attributes();
    header_line_ = 0;
    check_minimum_payload();

    // The complete lower bound on decoded slots and file bytes was checked above.
    mesh_.positions.reserve(vertex_count_);
    mesh_values_ = vertex_count_ * 3;
    if (has_normals_) {
        mesh_.normals.reserve(vertex_count_);
        mesh_values_ += vertex_count_ * 3;
    }
    if (has_uvs_) {
        mesh_.uvs.reserve(vertex_count_);
        mesh_values_ += vertex_count_ * 2;
    }
    for (const auto &element : elements_) {
        if (element.properties.empty()) {
            continue;
        }
        element_ = element.name;
        for (row_ = 0; row_ < element.rows; ++row_) {
            read_row(element);
        }
    }
    element_ = {};
    if (encoding_ == Encoding::ascii) {
        while (offset_ < input_.size() && whitespace(input_[offset_])) {
            ++offset_;
        }
    }
    if (offset_ != input_.size()) {
        fail("Unexpected data after declared PLY elements");
    }

    GeometryData checked;
    checked.meshes.push_back(std::move(mesh_));
    if (const auto message = validate_geometry(checked); !message.empty()) {
        throw PlyIssue{Severity::error, "invalid-geometry",
                       message + " after conversion to binary32"};
    }
    if (!unsupported_polygon_.empty()) {
        throw PlyIssue{Severity::unsupported, "unsupported-ply-polygon",
                       std::move(unsupported_polygon_)};
    }
    return {std::move(checked.meshes.front()), mesh_values_};
}

} // namespace

PlyResult decode_ply(const std::filesystem::path &path, const Limits &limits,
                     PlyBudget &remaining) {
    if (remaining.files == 0) {
        return PlyIssue{Severity::error, "limit-exceeded", "PLY input file count limit exceeded"};
    }
    --remaining.files;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return PlyIssue{Severity::error, "unreadable-file",
                        "Cannot size PLY input: " + error.message()};
    }
    if (size > limits.file_bytes || size > std::string{}.max_size() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return PlyIssue{Severity::error, "limit-exceeded", "PLY file byte limit exceeded"};
    }
    if (size > remaining.bytes) {
        return PlyIssue{Severity::error, "limit-exceeded", "Expanded input byte limit exceeded"};
    }
    remaining.bytes -= static_cast<std::size_t>(size);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return PlyIssue{Severity::error, "unreadable-file", "Cannot open PLY input"};
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size())) ||
        input.peek() != std::char_traits<char>::eof() || input.bad()) {
        return PlyIssue{Severity::error, "unreadable-file",
                        "PLY input changed or could not be read"};
    }
    try {
        return Decoder(data, limits, remaining).decode();
    } catch (PlyIssue &issue) {
        return std::move(issue);
    }
}

} // namespace rypath::pbrt
