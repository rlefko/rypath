// SPDX-License-Identifier: GPL-3.0-only
#include "shading_normal_cases.hpp"
#include "shading_normal_shared.h"
#include <iostream>
int main() {
    try {
        auto queries = shading_normal_test::queries();
        // CPU binary32 supports subnormal inputs. Metal does not guarantee this
        // input contract, so this case is intentionally portable-only.
        queries.push_back({0x1p-149f, 0x1p-149f, 0x1p-149f, 0x1p-149f, 0x1p100f, 1});
        for (auto q : queries) {
            auto c = rypath_shading_normal::correction(q.wo_ns, q.wi_ns, q.wo_ng, q.wi_ng);
            auto a = rypath_shading_normal::apply(q.a, c);
            auto ab = rypath_shading_normal::multiply(q.a, q.b, c);
            shading_normal_test::check(a, shading_normal_test::oracle(q, false));
            shading_normal_test::check(ab, shading_normal_test::oracle(q, true));
            shading_normal_test::check_identity(q, a, ab);
            const auto endpoint =
                rypath_shading_normal::cosine_correction(q.wo_ns, q.wi_ns, q.wo_ng, q.wi_ng);
            shading_normal_test::check(rypath_shading_normal::apply(q.a, endpoint),
                                       shading_normal_test::endpoint_oracle(q, false));
            shading_normal_test::check(rypath_shading_normal::multiply(q.a, q.b, endpoint),
                                       shading_normal_test::endpoint_oracle(q, true));
        }
        // Weighted-adjoint reciprocity with independent nonzero signed cosines.
        for (int i = 1; i < 100; ++i) {
            const float a = float(i) / 101, b = .37f, g = -.81f, h = .59f;
            const auto reverse = rypath_shading_normal::correction(b, a, h, g);
            const float importance = rypath_shading_normal::apply(std::abs(a), reverse);
            const double lhs = std::abs(double(b) * g);
            const double rhs = importance * std::abs(double(h));
            if (std::abs(lhs - rhs) > 8 * std::numeric_limits<float>::epsilon() * lhs)
                throw std::runtime_error("Weighted-adjoint reciprocity failed");
        }
        std::cout << "PASS shading-normal long-double oracle " << queries.size()
                  << " cases; flat eta bits, boundaries, dynamic range, reciprocity\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
