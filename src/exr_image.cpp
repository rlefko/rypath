// SPDX-License-Identifier: GPL-3.0-only
#include "exr_image.hpp"

#include <openexr.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace rypath {
namespace {
// Core and classic codecs execute synchronously. Their allocator ABI has no
// userdata, so a stack-scoped TLS owner supplies the budget. Each allocation
// carries its actual owner, including while a positioned write nests a codec.
struct Budget {
    std::size_t limit, used = 0;
    bool exceeded = false;
};
thread_local Budget *active_budget = nullptr;
struct BudgetScope {
    Budget budget;
    Budget *previous = active_budget;
    explicit BudgetScope(std::size_t limit) : budget{limit} { active_budget = &budget; }
    ~BudgetScope() { active_budget = previous; }
};
struct alignas(std::max_align_t) Allocation {
    Budget *owner;
    std::size_t bytes;
};
void *allocate(std::size_t bytes) noexcept {
    auto *owner = active_budget;
    if (!owner || bytes > std::numeric_limits<std::size_t>::max() - sizeof(Allocation) ||
        bytes + sizeof(Allocation) > owner->limit - owner->used) {
        if (owner)
            owner->exceeded = true;
        return nullptr;
    }
    const auto total = bytes + sizeof(Allocation);
    auto *header = static_cast<Allocation *>(std::malloc(total));
    if (!header)
        return nullptr;
    *header = {owner, total};
    owner->used += total;
    return header + 1;
}
void release(void *ptr) noexcept {
    if (!ptr)
        return;
    auto *header = static_cast<Allocation *>(ptr) - 1;
    header->owner->used -= header->bytes;
    std::free(header);
}
void silence(exr_const_context_t, exr_result_t, const char *message) {
    // Core reports context dimension limits using INVALID_ATTR, so retain
    // its precise resource classification separately from malformed attributes.
    if (active_budget && message &&
        (std::strstr(message, "too large (max") || std::strstr(message, "tile exceeds max size")))
        active_budget->exceeded = true;
}
struct Stream {
    std::span<const std::uint8_t> bytes;
    const ExrWrite *write = nullptr;
    bool failed = false;
};
// OpenEXR fixes the userdata/buffer ordering in its C callback ABI.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int64_t read_bytes(exr_const_context_t, void *user, void *buffer, uint64_t count, uint64_t offset,
                   exr_stream_error_func_ptr_t) noexcept {
    auto &s = *static_cast<Stream *>(user);
    if (offset > s.bytes.size() || count > INT64_MAX)
        return -1;
    const auto n = std::min<std::uint64_t>(count, s.bytes.size() - offset);
    if (n)
        std::memcpy(buffer, s.bytes.data() + offset, static_cast<std::size_t>(n));
    return static_cast<int64_t>(n);
}
int64_t stream_size(exr_const_context_t, void *user) noexcept {
    return static_cast<int64_t>(static_cast<Stream *>(user)->bytes.size());
}
// OpenEXR fixes the userdata/buffer ordering in its C callback ABI.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int64_t write_bytes(exr_const_context_t, void *user, const void *buffer, uint64_t count,
                    uint64_t offset, exr_stream_error_func_ptr_t) noexcept {
    auto &s = *static_cast<Stream *>(user);
    if (s.failed || count > INT64_MAX || offset > UINT64_MAX - count)
        return -1;
    try {
        if ((*s.write)(offset, {static_cast<const std::uint8_t *>(buffer),
                                static_cast<std::size_t>(count)}))
            return static_cast<int64_t>(count);
    } catch (...) {
        // Never unwind through C. The caller receives a normal output failure.
        s.failed = true;
        return -1;
    }
    s.failed = true;
    return -1;
}
exr_context_initializer_t initializer(Stream &stream) {
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.error_handler_fn = silence;
    init.alloc_fn = allocate;
    init.free_fn = release;
    init.user_data = &stream;
    init.flags = EXR_CONTEXT_FLAG_STRICT_HEADER | EXR_CONTEXT_FLAG_DISABLE_CHUNK_RECONSTRUCTION;
    init.max_image_width = init.max_image_height = INT_MAX;
    init.max_tile_width = init.max_tile_height = INT_MAX;
    return init;
}
struct Failure {
    std::string message;
    bool unsupported = false, limit = false;
};
void check(exr_result_t result) {
    if (result != EXR_ERR_SUCCESS)
        throw Failure{std::string("EXR: ") + exr_get_default_error_message(result), false,
                      result == EXR_ERR_OUT_OF_MEMORY || active_budget->exceeded};
}
struct Context {
    exr_context_t value = nullptr;
    ~Context() {
        if (value)
            exr_finish(&value);
    }
    void finish() { check(exr_finish(&value)); }
};
struct Decoder {
    exr_context_t context;
    exr_decode_pipeline_t pipeline{};
    explicit Decoder(exr_context_t c) : context(c) { pipeline.pipe_size = sizeof(pipeline); }
    ~Decoder() { exr_decoding_destroy(context, &pipeline); }
};
struct Encoder {
    exr_context_t context;
    exr_encode_pipeline_t pipeline{};
    explicit Encoder(exr_context_t c) : context(c) { pipeline.pipe_size = sizeof(pipeline); }
    ~Encoder() { exr_encoding_destroy(context, &pipeline); }
};
constexpr exr_attr_chromaticities_t srgb{0.64f, 0.33f, 0.30f,   0.60f,
                                         0.15f, 0.06f, 0.3127f, 0.3290f};
void check_chromaticities(exr_context_t context) {
    exr_attr_chromaticities_t chroma;
    auto result = exr_attr_get_chromaticities(context, 0, "chromaticities", &chroma);
    if (result == EXR_ERR_NO_ATTR_BY_NAME)
        return;
    check(result);
    const std::array<float, 8> actual{chroma.red_x,  chroma.red_y,  chroma.green_x, chroma.green_y,
                                      chroma.blue_x, chroma.blue_y, chroma.white_x, chroma.white_y};
    const std::array<float, 8> expected{srgb.red_x,  srgb.red_y,  srgb.green_x, srgb.green_y,
                                        srgb.blue_x, srgb.blue_y, srgb.white_x, srgb.white_y};
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!std::isfinite(actual[i]))
            throw Failure{"EXR: nonfinite chromaticities"};
        if (std::abs(actual[i] - expected[i]) >= 1e-3f * expected[i])
            throw Failure{"EXR: unsupported chromaticities (linear sRGB required)", true};
    }
}
std::size_t pixel_count(std::uint64_t w, std::uint64_t h, std::uint32_t channels,
                        const ImageLimits &limits) {
    if (!w || !h)
        throw Failure{"EXR: empty data window"};
    if (w > limits.max_dimension || h > limits.max_dimension || w > INT_MAX / 16 || h > INT_MAX ||
        w > std::numeric_limits<std::size_t>::max() / h ||
        w * h > limits.retained_bytes / (channels * sizeof(float)))
        throw Failure{"EXR: image exceeds retained size or dimension limit", false, true};
    return static_cast<std::size_t>(w * h);
}
int component(std::string_view name) {
    if (name == "R" || name == "Y")
        return 0;
    if (name == "G")
        return 1;
    if (name == "B")
        return 2;
    if (name == "A")
        return 3;
    return -1;
}
} // namespace

ImageResult<DecodedImage> decode_exr(std::span<const std::uint8_t> bytes,
                                     const ImageLimits &requested_limits) {
    if (!valid_image_limits(requested_limits) || bytes.size() > requested_limits.file_bytes)
        return {.value = {},
                .diagnostic = "EXR: input exceeds the configured limit",
                .limit_exceeded = true};
    const auto &limits = requested_limits;
    BudgetScope scope(limits.scratch_bytes);
    try {
        if (!limits.max_dimension)
            throw Failure{"EXR: zero dimension limit", false, true};
        if (bytes.size() < 8 || bytes.size() > INT64_MAX)
            throw Failure{"EXR: truncated or oversized input"};
        constexpr std::array<std::uint8_t, 4> magic{0x76, 0x2f, 0x31, 0x01};
        if (!std::equal(magic.begin(), magic.end(), bytes.begin()))
            throw Failure{"EXR: invalid signature"};
        // Reject multipart/deep before even parsing their potentially large headers.
        if ((bytes[5] & 0x18) != 0)
            throw Failure{"EXR: multipart and deep images are unsupported", true};
        Stream stream{bytes};
        auto init = initializer(stream);
        init.max_image_width = init.max_image_height = static_cast<int>(limits.max_dimension);
        init.max_tile_width = init.max_tile_height = static_cast<int>(limits.max_dimension);
        init.read_fn = read_bytes;
        init.size_fn = stream_size;
        Context context;
        check(exr_start_read(&context.value, "memory.exr", &init));
        int parts = 0;
        check(exr_get_count(context.value, &parts));
        exr_storage_t storage;
        check(exr_get_storage(context.value, 0, &storage));
        if (parts != 1 || (storage != EXR_STORAGE_SCANLINE && storage != EXR_STORAGE_TILED))
            throw Failure{"EXR: only single-part flat images are supported", true};
        exr_compression_t compression;
        check(exr_get_compression(context.value, 0, &compression));
        if (compression > EXR_COMPRESSION_DWAB)
            throw Failure{"EXR: HTJ2K, LJ2K and ZSTD compression are unsupported", true};
        exr_attr_box2i_t window;
        check(exr_get_data_window(context.value, 0, &window));
        const auto width = static_cast<std::int64_t>(window.max.x) - window.min.x + 1;
        const auto height = static_cast<std::int64_t>(window.max.y) - window.min.y + 1;
        if (width <= 0 || height <= 0)
            throw Failure{"EXR: invalid data window"};
        const exr_attr_chlist_t *channels = nullptr;
        check(exr_get_channels(context.value, 0, &channels));
        if (channels->num_channels != 1 && channels->num_channels != 3 &&
            channels->num_channels != 4)
            throw Failure{"EXR: only Y, RGB and RGBA channel layouts are supported", true};
        unsigned mask = 0;
        const auto type = channels->entries[0].pixel_type;
        for (int i = 0; i < channels->num_channels; ++i) {
            const auto &channel = channels->entries[i];
            const std::string_view name(channel.name.str,
                                        static_cast<std::size_t>(channel.name.length));
            const int c = component(name);
            if (c < 0 || (channels->num_channels == 1 ? name != "Y" : name == "Y") ||
                (mask & (1u << c)) || channel.pixel_type != type ||
                (type != EXR_PIXEL_HALF && type != EXR_PIXEL_FLOAT) || channel.x_sampling != 1 ||
                channel.y_sampling != 1)
                throw Failure{"EXR: unsupported channel name, type, or sampling", true};
            mask |= 1u << c;
        }
        if (mask != (channels->num_channels == 1 ? 1u : channels->num_channels == 3 ? 7u : 15u))
            throw Failure{"EXR: partial channel layout is unsupported", true};
        check_chromaticities(context.value);
        const auto count = pixel_count(width, height, channels->num_channels == 4 ? 4 : 3, limits);
        DecodedImage image;
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.source_channels = static_cast<std::uint32_t>(channels->num_channels);
        image.file_format = ImageFileFormat::exr;
        image.sample_format =
            type == EXR_PIXEL_HALF ? ImageSampleFormat::half : ImageSampleFormat::float32;
        image.floating_rgb.resize(count * 3);
        if (image.source_channels == 4)
            image.floating_alpha.resize(count);
        auto decode_chunk = [&](const exr_chunk_info_t &chunk, std::int64_t x, std::int64_t y) {
            if (x < 0 || y < 0 || chunk.width <= 0 || chunk.height <= 0 ||
                x + chunk.width > width || y + chunk.height > height ||
                chunk.data_offset > bytes.size() ||
                chunk.packed_size > bytes.size() - chunk.data_offset)
                throw Failure{"EXR: invalid chunk extent or offset"};
            Decoder decoder{context.value};
            check(exr_decoding_initialize(context.value, 0, &chunk, &decoder.pipeline));
            for (int i = 0; i < decoder.pipeline.channel_count; ++i) {
                auto &c = decoder.pipeline.channels[i];
                const int index = component(c.channel_name);
                const auto offset = static_cast<std::size_t>(y * width + x);
                c.user_data_type = EXR_PIXEL_FLOAT;
                c.user_bytes_per_element = sizeof(float);
                c.user_pixel_stride = static_cast<int32_t>(sizeof(float) * (index == 3 ? 1 : 3));
                c.user_line_stride = static_cast<int32_t>(width * c.user_pixel_stride);
                c.decode_to_ptr = reinterpret_cast<std::uint8_t *>(
                    index == 3 ? image.floating_alpha.data() + offset
                               : image.floating_rgb.data() + offset * 3 + index);
            }
            check(exr_decoding_choose_default_routines(context.value, 0, &decoder.pipeline));
            check(exr_decoding_run(context.value, 0, &decoder.pipeline));
        };
        if (storage == EXR_STORAGE_SCANLINE) {
            for (std::int64_t y = 0; y < height;) {
                exr_chunk_info_t chunk{};
                check(exr_read_scanline_chunk_info(context.value, 0,
                                                   static_cast<int>(window.min.y + y), &chunk));
                if (chunk.start_y != window.min.y + y || chunk.start_x != window.min.x)
                    throw Failure{"EXR: inconsistent scanline chunk origin"};
                decode_chunk(chunk, 0, y);
                y += chunk.height;
            }
        } else {
            uint32_t tw, th;
            exr_tile_level_mode_t mode;
            exr_tile_round_mode_t rounding;
            check(exr_get_tile_descriptor(context.value, 0, &tw, &th, &mode, &rounding));
            if (mode != EXR_TILE_ONE_LEVEL)
                throw Failure{"EXR: mipmap and ripmap tiled images are unsupported", true};
            if (!tw || !th)
                throw Failure{"EXR: invalid tile dimensions"};
            for (std::int64_t y = 0; y * th < height; ++y)
                for (std::int64_t x = 0; x * tw < width; ++x) {
                    exr_chunk_info_t chunk{};
                    check(exr_read_tile_chunk_info(context.value, 0, static_cast<int>(x),
                                                   static_cast<int>(y), 0, 0, &chunk));
                    if (chunk.start_x != x || chunk.start_y != y)
                        throw Failure{"EXR: inconsistent tile index"};
                    decode_chunk(chunk, x * tw, y * th);
                }
        }
        if (image.source_channels == 1)
            for (std::size_t i = 0; i < count; ++i)
                image.floating_rgb[i * 3 + 1] = image.floating_rgb[i * 3 + 2] =
                    image.floating_rgb[i * 3];
        for (float value : image.floating_rgb)
            if (!std::isfinite(value))
                throw Failure{"EXR: nonfinite RGB sample"};
        for (float value : image.floating_alpha)
            if (!std::isfinite(value))
                throw Failure{"EXR: nonfinite alpha sample"};
        context.finish();
        return {std::move(image), {}};
    } catch (const Failure &failure) {
        return {{}, failure.message, failure.unsupported, failure.limit};
    } catch (const std::bad_alloc &) {
        return {{}, "EXR: allocation failed", false, true};
    }
}

std::string encode_exr(const ExrImageView &image, const ExrWrite &write,
                       const ImageLimits &limits) {
    BudgetScope scope(limits.scratch_bytes);
    try {
        const auto count = pixel_count(image.width, image.height, 3, limits);
        if ((image.precision != ExrPrecision::half && image.precision != ExrPrecision::float32) ||
            image.rgb.size() != count * 3 || !write)
            throw Failure{"EXR: invalid RGB extent or write callback"};
        for (float value : image.rgb) {
            if (!std::isfinite(value))
                throw Failure{"EXR: nonfinite output sample"};
            if (image.precision == ExrPrecision::half && std::abs(value) > 65504.f)
                throw Failure{"EXR: output sample overflows HALF"};
        }
        Stream stream{{}, &write};
        auto init = initializer(stream);
        init.write_fn = write_bytes;
        Context context;
        check(exr_start_write(&context.value, "memory.exr", EXR_WRITE_FILE_DIRECTLY, &init));
        int part = 0;
        check(exr_add_part(context.value, nullptr, EXR_STORAGE_SCANLINE, &part));
        check(exr_initialize_required_attr_simple(
            context.value, part, static_cast<int>(image.width), static_cast<int>(image.height),
            EXR_COMPRESSION_ZIP));
        for (const char *name : {"R", "G", "B"})
            check(exr_add_channel(context.value, part, name,
                                  image.precision == ExrPrecision::half ? EXR_PIXEL_HALF
                                                                        : EXR_PIXEL_FLOAT,
                                  EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));
        check(exr_attr_set_chromaticities(context.value, part, "chromaticities", &srgb));
        for (std::size_t i = 0; i < image.attributes.size(); ++i) {
            const auto &[name, value] = image.attributes[i];
            constexpr std::array<std::string_view, 16> reserved{"channels",
                                                                "compression",
                                                                "dataWindow",
                                                                "displayWindow",
                                                                "lineOrder",
                                                                "pixelAspectRatio",
                                                                "screenWindowCenter",
                                                                "screenWindowWidth",
                                                                "chromaticities",
                                                                "tiles",
                                                                "name",
                                                                "type",
                                                                "version",
                                                                "chunkCount",
                                                                "maxSamplesPerPixel",
                                                                "view"};
            if (std::find(reserved.begin(), reserved.end(), name) != reserved.end())
                throw Failure{"EXR: reserved output attribute name"};
            for (std::size_t j = 0; j < i; ++j)
                if (image.attributes[j].first == name)
                    throw Failure{"EXR: duplicate output attribute name"};
            if (name.empty() || name.size() > 255 || name.find('\0') != std::string::npos ||
                value.find('\0') != std::string::npos || value.size() > INT_MAX)
                throw Failure{"EXR: invalid output string attribute"};
            if (name.size() > 31)
                check(exr_set_longname_support(context.value, 1));
            check(exr_attr_set_string(context.value, part, name.c_str(), value.c_str()));
        }
        check(exr_write_header(context.value));
        for (std::uint32_t y = 0; y < image.height;) {
            exr_chunk_info_t chunk{};
            check(exr_write_scanline_chunk_info(context.value, part, static_cast<int>(y), &chunk));
            Encoder encoder{context.value};
            check(exr_encoding_initialize(context.value, part, &chunk, &encoder.pipeline));
            for (int i = 0; i < encoder.pipeline.channel_count; ++i) {
                auto &c = encoder.pipeline.channels[i];
                c.user_data_type = EXR_PIXEL_FLOAT;
                c.user_bytes_per_element = sizeof(float);
                c.user_pixel_stride = 3 * sizeof(float);
                c.user_line_stride =
                    static_cast<int32_t>(std::size_t{image.width} * 3 * sizeof(float));
                c.encode_from_ptr = reinterpret_cast<const std::uint8_t *>(
                    image.rgb.data() + static_cast<std::size_t>(y) * image.width * 3 +
                    component(c.channel_name));
            }
            check(exr_encoding_choose_default_routines(context.value, part, &encoder.pipeline));
            check(exr_encoding_run(context.value, part, &encoder.pipeline));
            y += static_cast<std::uint32_t>(chunk.height);
        }
        context.finish();
        if (stream.failed)
            throw Failure{"EXR: positioned output write failed"};
        return {};
    } catch (const Failure &failure) {
        return failure.message;
    } catch (const std::bad_alloc &) {
        return "EXR: allocation failed";
    }
}
} // namespace rypath
