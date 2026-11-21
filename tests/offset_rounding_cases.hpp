// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "offset_rounding_shared.h"
#include <array>
namespace offset_rounding_test {
using Word = rypath_offset_rounding::Word;
struct Case {
    Word input, positive, negative;
    bool positive_valid = true, negative_valid = true;
};
// Explicit expected words, independent of the helper's branching algorithm.
inline constexpr std::array cases{Case{0x00000000u, 0x00800000u, 0x80800000u},
                                  Case{0x80000000u, 0x00800000u, 0x80800000u},
                                  Case{0x00000001u, 0x00800000u, 0x00000000u},
                                  Case{0x80000001u, 0x80000000u, 0x80800000u},
                                  Case{0x00400000u, 0x00800000u, 0x00000000u},
                                  Case{0x80400000u, 0x80000000u, 0x80800000u},
                                  Case{0x007fffffu, 0x00800000u, 0x00000000u},
                                  Case{0x807fffffu, 0x80000000u, 0x80800000u},
                                  Case{0x00800000u, 0x00800001u, 0x00000000u},
                                  Case{0x80800000u, 0x80000000u, 0x80800001u},
                                  Case{0x00800001u, 0x00800002u, 0x00800000u},
                                  Case{0x80800001u, 0x80800000u, 0x80800002u},
                                  Case{0x3f000000u, 0x3f000001u, 0x3effffffu},
                                  Case{0xbf000000u, 0xbeffffffu, 0xbf000001u},
                                  Case{0x3f800000u, 0x3f800001u, 0x3f7fffffu},
                                  Case{0xbf800000u, 0xbf7fffffu, 0xbf800001u},
                                  Case{0x7f7ffffeu, 0x7f7fffffu, 0x7f7ffffdu},
                                  Case{0xff7ffffeu, 0xff7ffffdu, 0xff7fffffu},
                                  Case{0x7f7fffffu, 0x7f800000u, 0x7f7ffffeu, false, true},
                                  Case{0xff7fffffu, 0xff7ffffeu, 0xff800000u, true, false},
                                  Case{0x7f800000u, 0x7f800000u, 0x7f800000u, false, false},
                                  Case{0xff800000u, 0xff800000u, 0xff800000u, false, false},
                                  Case{0x7fc00001u, 0x7fc00001u, 0x7fc00001u, false, false},
                                  Case{0xffc00001u, 0xffc00001u, 0xffc00001u, false, false}};
} // namespace offset_rounding_test
