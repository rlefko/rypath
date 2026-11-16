// SPDX-License-Identifier: GPL-3.0-only
// Oracle adapter: calls the separately linked, pinned upstream implementation.
#include <iomanip>
#include <iostream>
#include <pbrt/options.h>
#include <pbrt/pbrt.h>
#include <pbrt/util/loopsubdiv.h>
#include <pbrt/util/mesh.h>
#include <pbrt/util/transform.h>
#include <vector>
int main() {
    int levels, nv, ni;
    if (!(std::cin >> levels >> nv >> ni) || nv < 3 || ni < 3 || ni % 3 || nv > 1000000 ||
        ni > 2000000 || levels > 6)
        return 1;
    std::vector<pbrt::Point3f> p(nv);
    std::vector<int> indices(ni);
    for (auto &v : p)
        if (!(std::cin >> v.x >> v.y >> v.z))
            return 1;
    for (auto &i : indices)
        if (!(std::cin >> i) || i < 0 || i >= nv)
            return 1;
    pbrt::PBRTOptions options;
    options.nThreads = 2;
    pbrt::InitPBRT(options);
    pbrt::Transform transform;
    auto mesh = pbrt::LoopSubdivide(&transform, false, levels, indices, p, pbrt::Allocator{});
    std::cout << std::setprecision(9) << "{\"indices\":[";
    for (int i = 0; i < mesh->nTriangles * 3; ++i)
        std::cout << (i ? "," : "") << mesh->vertexIndices[i];
    std::cout << "],\"P\":[";
    for (int i = 0; i < mesh->nVertices; ++i) {
        auto v = mesh->p[i];
        std::cout << (i ? "," : "") << '[' << v.x << ',' << v.y << ',' << v.z << ']';
    }
    std::cout << "],\"N\":[";
    for (int i = 0; i < mesh->nVertices; ++i) {
        auto v = mesh->n[i];
        std::cout << (i ? "," : "") << '[' << v.x << ',' << v.y << ',' << v.z << ']';
    }
    std::cout << "]}\n";
    pbrt::CleanupPBRT();
}
