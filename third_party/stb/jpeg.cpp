// SPDX-License-Identifier: GPL-3.0-only

#include "jpeg.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <string_view>

namespace {

// stb has no allocator context argument. This pointer is active only for the
// duration of a synchronous decode, independently on each rendering thread.
struct Budget {
    std::size_t available;
    bool exceeded = false;
};
thread_local Budget *active_budget = nullptr;

struct BudgetScope {
    explicit BudgetScope(Budget &budget) { active_budget = &budget; }
    ~BudgetScope() { active_budget = nullptr; }
};

struct alignas(std::max_align_t) Allocation {
    std::size_t bytes;
};

void *jpeg_malloc(std::size_t bytes) {
    if (active_budget->available < sizeof(Allocation) ||
        bytes > active_budget->available - sizeof(Allocation)) {
        active_budget->exceeded = true;
        return nullptr;
    }
    const auto allocation_size = bytes + sizeof(Allocation);
    auto *allocation = static_cast<Allocation *>(std::malloc(allocation_size));
    if (allocation == nullptr) {
        active_budget->exceeded = true;
        return nullptr;
    }
    *allocation = {allocation_size};
    active_budget->available -= allocation_size;
    return allocation + 1;
}

void jpeg_free(void *pointer) {
    if (pointer == nullptr)
        return;
    auto *allocation = static_cast<Allocation *>(pointer) - 1;
    active_budget->available += allocation->bytes;
    std::free(allocation);
}

} // namespace

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_MALLOC(size) jpeg_malloc(size)
#define STBI_FREE(pointer) jpeg_free(pointer)
#define STBI_REALLOC(pointer, size) nullptr
#include "stb_image.h"

namespace rypath::jpeg {
namespace {

unsigned read_be16(std::span<const std::uint8_t> bytes) {
    return (static_cast<unsigned>(bytes[0]) << 8) | bytes[1];
}

Result inspect(std::span<const std::uint8_t> bytes, Limits limits) {
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8)
        return {Error::invalid, "Missing JPEG start marker"};

    Result result{Error::none, nullptr};
    bool entropy = false;
    unsigned scans = 0;
    std::size_t offset = 2;
    while (offset < bytes.size()) {
        if (entropy) {
            const auto remainder = bytes.subspan(offset);
            offset +=
                static_cast<std::size_t>(std::ranges::find(remainder, 0xff) - remainder.begin());
        }
        if (offset == bytes.size() || bytes[offset] != 0xff)
            break;
        ++offset;
        const auto after_prefix = offset;
        while (offset < bytes.size() && bytes[offset] == 0xff)
            ++offset;
        if (offset == bytes.size())
            break;
        const auto marker = bytes[offset++];
        if (entropy && marker == 0 && offset != after_prefix + 1)
            return {Error::invalid, "Invalid JPEG entropy escape"};
        if (entropy && (marker == 0 || (marker >= 0xd0 && marker <= 0xd7)))
            continue;
        entropy = false;
        if (marker == 0xd9) {
            if (scans != 0 && offset == bytes.size())
                return result;
            return {Error::invalid, "Invalid JPEG end marker"};
        }
        if (marker == 0 || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7))
            return {Error::invalid, "Unexpected standalone JPEG marker"};
        if (bytes.size() - offset < 2)
            break;
        const auto length = read_be16(bytes.subspan(offset));
        if (length < 2 || length > bytes.size() - offset)
            break;
        const auto data = bytes.subspan(offset + 2, length - 2);
        offset += length;
        const bool app = marker >= 0xe0 && marker <= 0xef;
        const bool sof = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xcc;
        if (sof) {
            if (result.width != 0 || data.size() < 6)
                return {Error::invalid, "Invalid JPEG frame header"};
            if ((marker != 0xc0 && marker != 0xc2) || data[0] != 8)
                return {Error::unsupported, "JPEG requires 8-bit baseline or progressive DCT"};
            result.height = read_be16(data.subspan(1));
            result.width = read_be16(data.subspan(3));
            result.channels = data[5];
            if (result.channels != 1 && result.channels != 3)
                return {Error::unsupported, "JPEG requires grayscale, RGB, or YCbCr"};
            if (data.size() != 6 + 3 * result.channels || result.width == 0)
                return {Error::invalid, "Invalid JPEG frame header"};
            if (result.height == 0)
                return {Error::unsupported, "Delayed JPEG frame dimensions are unsupported"};
            if (result.width > limits.max_dimension || result.height > limits.max_dimension)
                return {Error::dimension, "JPEG exceeds the image dimension limit"};
            if (static_cast<std::size_t>(result.width) * result.height * 3 > limits.retained_bytes)
                return {Error::retained, "JPEG exceeds the decoded image byte budget"};
        } else if (marker == 0xda) {
            if (result.width == 0)
                return {Error::invalid, "JPEG scan precedes the frame header"};
            if (data.size() < 6 || data[0] == 0 || data[0] > result.channels ||
                data.size() != 4u + 2u * data[0])
                return {Error::invalid, "Invalid JPEG scan header"};
            if (++scans > 256)
                return {Error::work, "JPEG exceeds the 256-scan limit"};
            entropy = true;
        } else if (marker == 0xee && data.size() >= 5 &&
                   std::equal(data.begin(), data.begin() + 5, "Adobe")) {
            if (data.size() < 12)
                return {Error::invalid, "Truncated JPEG Adobe marker"};
            if (data[11] > 1)
                return {Error::unsupported, "Unsupported JPEG Adobe color transform"};
        } else if (!app && marker != 0xc4 && marker != 0xdb && marker != 0xdd && marker != 0xfe) {
            return {Error::unsupported, "Unsupported JPEG marker"};
        }
    }
    return {Error::invalid, "Truncated or malformed JPEG segment"};
}

} // namespace

Result decode(std::span<const std::uint8_t> bytes, Limits limits) {
    auto result = inspect(bytes, limits);
    if (result.error != Error::none)
        return result;

    Budget budget{limits.scratch_bytes};
    BudgetScope scope(budget);
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi__g_failure_reason = nullptr;
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> raw(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                              &channels, 3),
        &stbi_image_free);
    if (budget.exceeded)
        return {Error::scratch, "JPEG decoder exceeded the scratch byte budget"};
    if (!raw) {
        const auto *reason = stbi_failure_reason();
        const bool work = reason && std::string_view(reason) == "JPEG work limit";
        return {work ? Error::work : Error::invalid,
                work ? "JPEG exceeds the DCT block-work limit" : reason};
    }
    if (static_cast<unsigned>(width) != result.width ||
        static_cast<unsigned>(height) != result.height ||
        static_cast<unsigned>(channels) != result.channels)
        return {Error::invalid, "JPEG decoded dimensions or channels differ from the frame"};
    const auto size = static_cast<std::size_t>(result.width) * result.height * 3;
    if (budget.available < size)
        return {Error::scratch, "JPEG retained copy exceeds the decoder scratch byte budget"};
    try {
        result.rgb.assign(raw.get(), raw.get() + size);
    } catch (const std::bad_alloc &) {
        return {Error::scratch, "JPEG decoder could not allocate required memory"};
    }
    return result;
}

} // namespace rypath::jpeg
