// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if !defined(__METAL_VERSION__)
#include <cstdint>
#endif
namespace rypath {
namespace checkpoint_contract {
#if defined(__METAL_VERSION__)
using Word = uint;
#else
using Word = std::uint32_t;
#endif
// Bump the appropriate contract whenever persisted rendering semantics change.
enum : Word { format = 1, identity = 8, renderer = 9, sampling = 2, accumulation = 1 };
} // namespace checkpoint_contract
} // namespace rypath
