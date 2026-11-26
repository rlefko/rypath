// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#include "render_wire.h"
#include "rgb_spectrum.hpp"
#include "texture_graph_wire.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
using namespace rypath;
namespace w = render_wire;
void need(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string why(NSError *e) {
    return e ? e.localizedDescription.UTF8String : "No Metal diagnostic";
}
id<MTLComputePipelineState> pipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString *name,
                                     bool families) {
    auto values = [MTLFunctionConstantValues new];
    for (unsigned i = 0; i <= 18; ++i) {
        const bool value = i == 0 || families;
        [values setConstantValue:&value type:MTLDataTypeBool atIndex:i];
    }
    const bool yes = true, no = false;
    [values setConstantValue:&no type:MTLDataTypeBool atIndex:20];
    [values setConstantValue:&yes type:MTLDataTypeBool atIndex:25];
    NSError *error = nil;
    auto function = [library newFunctionWithName:name constantValues:values error:&error];
    need(function != nil, "Current hero function: " + why(error));
    auto result = [device newComputePipelineStateWithFunction:function error:&error];
    need(result != nil, "Current hero pipeline: " + why(error));
    return result;
}
template <class T> id<MTLBuffer> upload(id<MTLDevice> device, const std::vector<T> &data) {
    auto b = [device newBufferWithBytes:data.data()
                                 length:data.size() * sizeof(T)
                                options:MTLResourceStorageModeShared];
    need(b != nil, "Current hero fixture buffer");
    return b;
}
void finish(id<MTLCommandBuffer> command, id<MTLBuffer> output, unsigned count,
            const std::string &label, bool preparation = false) {
    [command commit];
    [command waitUntilCompleted];
    need(command.status == MTLCommandBufferStatusCompleted, label + ": " + why(command.error));
    const auto *result = static_cast<const std::uint32_t *>(output.contents);
    for (unsigned i = 0; i < count; ++i) {
        if (result[i] == 0)
            continue;
        std::string detail;
        if (preparation) {
            constexpr unsigned masks[] = {0, 1, 3, 15, 2, 17};
            constexpr const char *fixtures[] = {"diffuse constant",
                                                "conductor",
                                                "dielectric",
                                                "thin dielectric",
                                                "diffuse transmission",
                                                "coated diffuse",
                                                "coated conductor",
                                                "measured table",
                                                "Fourier table",
                                                "glTF",
                                                "diffuse RGB image",
                                                "coated RGB image",
                                                "coated invalid companion eta",
                                                "coated invalid companion graph"};
            detail = ", material " + std::to_string(i / 6) + " (" + fixtures[i / 6] +
                     "), active mask " + std::to_string(masks[i % 6]) + ", original valid " +
                     std::to_string((result[i] >> 24) & 1) + ", compact valid " +
                     std::to_string((result[i] >> 25) & 1) + ", expected valid " +
                     std::to_string((result[i] >> 26) & 1);
        }
        throw std::runtime_error(label + " mismatch at case " + std::to_string(i) +
                                 ", field mask " + std::to_string(result[i] & 0x00ffffffu) +
                                 detail);
    }
    need(result[count] == 0xa5a5a5a5u, label + " output guard");
}
int run(const char *path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for current hero differential");
    NSError *error = nil;
    auto library =
        [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]
                            error:&error];
    need(library != nil, "Current hero library: " + why(error));
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("rypath-current-hero-" +
         std::to_string(reinterpret_cast<std::uintptr_t>((__bridge void *)device)));
    std::filesystem::create_directories(directory);
    const auto measured_path = directory / "measured.bsdf",
               fourier_path = directory / "fourier.bsdf";
    table_fixture::write(measured_path, table_fixture::measured());
    table_fixture::write(fourier_path, table_fixture::fourier(3, true, 1, true));
    auto measured_result = read_bsdf_table(measured_path, BsdfTableKind::measured);
    auto fourier_result = read_bsdf_table(fourier_path, BsdfTableKind::fourier);
    std::filesystem::remove_all(directory);
    need(measured_result.table && fourier_result.table, "Current hero real table fixtures");
    auto measured = upload(device, measured_result.table->words),
         fourier = upload(device, fourier_result.table->words);
    const auto rgb_values = srgb_reconstruction_table();
    auto rgb = [device newBufferWithBytes:rgb_values.data()
                                   length:rgb_values.size() * sizeof(float)
                                  options:MTLResourceStorageModeShared];
    auto queue = [device newCommandQueue];
    need(queue && rgb, "Current hero resources");
    for (bool families : {true, false}) {
        const unsigned count = 4224;
        auto state = pipeline(device, library, @"rypath_hero_current_differential", families);
        auto output = [device newBufferWithLength:(count + 1) * 4
                                          options:MTLResourceStorageModeShared];
        need(output != nil, "Current hero output");
        std::memset(output.contents, 0xa5, output.length);
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        need(command && encoder, "Current hero command");
        [encoder setComputePipelineState:state];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:measured offset:0 atIndex:1];
        [encoder setBuffer:fourier offset:0 atIndex:2];
        [encoder setBuffer:rgb offset:0 atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        finish(command, output, count,
               families ? "Current hero enabled families" : "Current hero disabled families");
    }
    // Real preparation fixtures share the original image lookup, graph and
    // scalar material preparation. Last cases invalidate companions only.
    const unsigned kinds[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 11};
    std::vector<w::Material> materials(14);
    std::vector<w::MaterialAuxiliary> auxiliary(14);
    // Zero eta is a valid native alias for eta=1. Use negative finite eta
    // for the companion-only scalar-preparation rejection witness instead.
    std::vector<w::SpectralKnot> knots{{360, 1.5f},
                                       {830, 1.5f},
                                       {360, 2.5f},
                                       {830, 2.5f},
                                       {360, 1.5f},
                                       {420, 1.5f},
                                       {490, -1.f},
                                       {830, -1.f},
                                       {360, .5f},
                                       {420, .5f},
                                       {490, std::numeric_limits<float>::quiet_NaN()},
                                       {830, std::numeric_limits<float>::quiet_NaN()}};
    const auto auxiliary_words = sizeof(w::MaterialAuxiliary) / 4;
    for (unsigned i = 0; i < materials.size(); ++i) {
        auto &m = materials[i];
        auto &a = auxiliary[i];
        m.kind = w::constant_material;
        m.scalar_value = .5f;
        m.scattering = i < 10 ? kinds[i] : i == 10 ? 0 : 5;
        m.eta_begin = 0;
        m.eta_count = 2;
        m.k_begin = 2;
        m.k_count = 2;
        m.optical_extensions = 3;
        m.u_roughness = .2f;
        m.v_roughness = .3f;
        m.remap_roughness = 0;
        m.auxiliary = i * auxiliary_words;
        a.eta_root = a.k_root = a.conductor_reflectance_root = a.transmittance_root =
            a.transmission_scale_root = 0xffffffffu;
        a.interface_eta_root = a.interface_u_roughness_root = a.interface_v_roughness_root =
            0xffffffffu;
        a.thickness_root = a.albedo_root = a.g_root = a.mix_amount_root = 0xffffffffu;
        a.transmittance = .25f;
        a.transmission_scale = 1;
        a.interface_eta_begin = 0;
        a.interface_eta_count = 2;
        a.interface_eta_extend = 1;
        a.interface_u_roughness = .2f;
        a.interface_v_roughness = .3f;
        a.thickness = .1f;
        a.albedo = 0;
        a.g = .125f;
        a.max_depth = 3;
        a.samples = 2;
        for (auto &c : a.gltf.base_color)
            c = .5f;
        a.gltf.metallic = .25f;
        a.gltf.roughness = .5f;
        a.gltf.base_color_texture = a.gltf.metallic_roughness_texture = 0xffffffffu;
    }
    materials[10].kind = materials[11].kind = w::image_material;
    materials[10].image_texture = materials[11].image_texture = 0;
    materials[12].eta_begin = 4;
    materials[12].eta_count = 4;
    materials[13].kind = w::graph_material;
    materials[13].texture_root = 0;
    std::vector<std::uint32_t> data(materials.size() * auxiliary_words);
    auxiliary[7].table_offset = unsigned(data.size());
    data.insert(data.end(), measured_result.table->words.begin(),
                measured_result.table->words.end());
    auxiliary[8].table_offset = unsigned(data.size());
    data.insert(data.end(), fourier_result.table->words.begin(), fourier_result.table->words.end());
    std::memcpy(data.data(), auxiliary.data(), auxiliary.size() * sizeof(auxiliary[0]));
    auto material_buffer = upload(device, materials), data_buffer = upload(device, data),
         knot_buffer = upload(device, knots);
    w::ImageTexture texture{};
    texture.level_count = 1;
    texture.texel_format = w::linear_float_texel_format;
    texture.uscale = texture.vscale = texture.scale = 1;
    texture.filter = w::trilinear_texture_filter;
    texture.max_anisotropy = 8;
    auto textures = upload(device, std::vector{texture});
    auto levels = upload(device, std::vector<w::TextureLevel>{{1, 1, 0}});
    auto texels = upload(device, std::vector<std::uint32_t>{std::bit_cast<std::uint32_t>(.2f),
                                                            std::bit_cast<std::uint32_t>(.5f),
                                                            std::bit_cast<std::uint32_t>(.8f),
                                                            std::bit_cast<std::uint32_t>(1.f)});
    texture_graph_wire::Node node{};
    node.kind = texture_graph_wire::knots;
    node.type = texture_graph_wire::spectrum;
    node.payload[0] = 8;
    node.payload[1] = 4;
    auto nodes = upload(device, std::vector{node});
    w::Parameters parameters{};
    parameters.packet_transport = 2;
    parameters.knot_count = unsigned(knots.size());
    parameters.material_data_words = unsigned(data.size());
    parameters.material_seed = -11;
    w::TextureParameters tp{};
    tp.texture_count = tp.level_count = 1;
    // The FLOAT texel format stores RGBA words even when channels == 3.
    tp.texel_word_count = unsigned(texels.length / sizeof(std::uint32_t));
    tp.spp_scale = 1;
    w::TextureGraphParameters gp{};
    gp.node_count = 1;
    gp.knot_count = unsigned(knots.size());
    const unsigned count = 84;
    auto state = pipeline(device, library, @"rypath_hero_current_preparation", true);
    auto output = [device newBufferWithLength:(count + 1) * 4 options:MTLResourceStorageModeShared];
    need(output != nil, "Current hero preparation output");
    std::memset(output.contents, 0xa5, output.length);
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    need(command && encoder, "Current hero preparation command");
    [encoder setComputePipelineState:state];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder setBuffer:material_buffer offset:0 atIndex:1];
    [encoder setBuffer:data_buffer offset:0 atIndex:2];
    [encoder setBuffer:knot_buffer offset:0 atIndex:3];
    [encoder setBuffer:rgb offset:0 atIndex:4];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:5];
    [encoder setBytes:&tp length:sizeof(tp) atIndex:6];
    [encoder setBuffer:textures offset:0 atIndex:7];
    [encoder setBuffer:levels offset:0 atIndex:8];
    [encoder setBuffer:texels offset:0 atIndex:9];
    [encoder setBuffer:nodes offset:0 atIndex:10];
    [encoder setBytes:&gp length:sizeof(gp) atIndex:11];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    finish(command, output, count, "Current hero preparation", true);
    std::cout << "Current hero: 8448 native sampling/evaluation differentials and 84 preparation "
                 "fixtures passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Usage: hero-current-probe LIBRARY");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
