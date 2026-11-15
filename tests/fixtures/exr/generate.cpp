// SPDX-License-Identifier: GPL-3.0-only
// One-time independent fixture generator; never linked by routine CI.
#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfChromaticitiesAttribute.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfTileDescription.h>
#include <OpenEXR/ImfTiledInputFile.h>
#include <OpenEXR/ImfTiledOutputFile.h>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
using namespace OPENEXR_IMF_NAMESPACE;
using namespace IMATH_NAMESPACE;
int main(int argc, char **argv) {
    static_assert(std::endian::native == std::endian::little, "fixture oracle is little-endian");
    const std::string directory = argc > 1 ? argv[1] : ".";
    const int width = 19, height = 17;
    const std::array<std::string, 10> names{"none",  "rle", "zips", "zip",  "piz",
                                            "pxr24", "b44", "b44a", "dwaa", "dwab"};
    auto generate = [&](std::string name, Compression compression, bool tiled, int channels,
                        PixelType type, bool mixed, bool wide, bool nonfinite) {
        const int ox = tiled ? -13 : -5, oy = tiled ? 21 : -9;
        Header header(width, height);
        header.dataWindow() = Box2i(V2i(ox, oy), V2i(ox + width - 1, oy + height - 1));
        header.compression() = compression;
        if (tiled)
            header.setTileDescription(TileDescription(7, 5, ONE_LEVEL));
        if (wide)
            header.insert("chromaticities",
                          ChromaticitiesAttribute(
                              Chromaticities(V2f(.708, .292), V2f(.170, .797), V2f(.131, .046))));
        else
            header.insert("chromaticities", ChromaticitiesAttribute(Chromaticities()));
        std::vector<std::string> cn = channels == 1   ? std::vector<std::string>{"Y"}
                                      : channels == 4 ? std::vector<std::string>{"R", "G", "B", "A"}
                                                      : std::vector<std::string>{"R", "G", "B"};
        std::array<std::vector<half>, 4> hp;
        std::array<std::vector<float>, 4> fp;
        std::array<std::vector<unsigned>, 4> up;
        FrameBuffer fb;
        for (int c = 0; c < channels; ++c) {
            auto ct = mixed && c == 1 ? FLOAT : type;
            header.channels().insert(cn[c], Channel(ct));
            // Extra prefix and a shifted base keep the independent fixture's
            // negative/positive origin pointers within their own allocation.
            hp[c].resize(4096);
            fp[c].resize(4096);
            up[c].resize(4096);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    float v = nonfinite && (channels == 3 || c == 3)
                                  ? std::numeric_limits<float>::infinity()
                                  : (c == 3 ? .5f : float((x + 3 * y + 5 * c) % 17 - 4) * .25f);
                    if (channels == 1 && y == 0 && x < 8) {
                        const std::array<float, 8> values{0.f,       -0.f,        0x1p-24f,
                                                          -0x1p-24f, 0x1p-14f,    65504.f,
                                                          -65504.f,  0x1.ffcp-15f};
                        v = values[x];
                    }
                    const auto i = 1024 + (oy + y) * width + ox + x;
                    hp[c][i] = half(v);
                    fp[c][i] = v;
                    up[c][i] = unsigned(x + y);
                }
            const auto b = ct == HALF    ? reinterpret_cast<char *>(hp[c].data() + 1024)
                           : ct == FLOAT ? reinterpret_cast<char *>(fp[c].data() + 1024)
                                         : reinterpret_cast<char *>(up[c].data() + 1024);
            const auto size = ct == HALF ? sizeof(half) : sizeof(float);
            fb.insert(cn[c], Slice(ct, b, size, width * size));
        }
        const auto path = directory + "/" + name + ".exr";
        if (tiled) {
            TiledOutputFile out(path.c_str(), header);
            out.setFrameBuffer(fb);
            out.writeTiles(0, out.numXTiles() - 1, 0, out.numYTiles() - 1);
        } else {
            OutputFile out(path.c_str(), header);
            out.setFrameBuffer(fb);
            out.writePixels(height);
        }
        if (mixed || wide || type == UINT || nonfinite)
            return;
        std::vector<float> expected(16384, 0);
        FrameBuffer decoded;
        for (int c = 0; c < channels; ++c)
            decoded.insert(cn[c], Slice(FLOAT, reinterpret_cast<char *>(expected.data() + 4096 + c),
                                        4 * sizeof(float), width * 4 * sizeof(float)));
        if (tiled) {
            TiledInputFile in(path.c_str());
            in.setFrameBuffer(decoded);
            in.readTiles(0, in.numXTiles() - 1, 0, in.numYTiles() - 1);
        } else {
            InputFile in(path.c_str());
            in.setFrameBuffer(decoded);
            in.readPixels(oy, oy + height - 1);
        }
        std::ofstream oracle(directory + "/" + name + ".f32", std::ios::binary);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                for (int c = 0; c < channels; ++c) {
                    const float value = expected[4096 + ((oy + y) * width + ox + x) * 4 + c];
                    oracle.write(reinterpret_cast<const char *>(&value), sizeof(value));
                }
    };
    for (int c = 0; c < 10; ++c)
        generate(names[c], Compression(c), false, 3, HALF, false, false, false);
    generate("tile-origin", ZIP_COMPRESSION, true, 4, FLOAT, false, false, false);
    generate("gray", ZIP_COMPRESSION, false, 1, HALF, false, false, false);
    generate("mixed", ZIP_COMPRESSION, false, 3, HALF, true, false, false);
    generate("uint", ZIP_COMPRESSION, false, 3, UINT, false, false, false);
    generate("wide", ZIP_COMPRESSION, false, 3, HALF, false, true, false);
    generate("nonfinite", ZIP_COMPRESSION, false, 3, FLOAT, false, false, true);
    generate("alpha-nonfinite", ZIP_COMPRESSION, false, 4, FLOAT, false, false, true);
}
