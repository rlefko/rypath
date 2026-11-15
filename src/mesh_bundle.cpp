// SPDX-License-Identifier: GPL-3.0-only
#include "mesh_bundle.hpp"
#include <cstdio>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#endif
namespace rypath::mesh_bundle {
// The no-replacement rename is atomic even if another process creates the destination.
void publish_directory(const fs::path &from, const fs::path &to, std::string_view label) {
    int result;
    do {
#if defined(__APPLE__)
        result = renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
        constexpr unsigned no_replace = 1;
        result = static_cast<int>(
            syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), no_replace));
#else
        errno = ENOTSUP;
        result = -1;
#endif
    } while (result < 0 && errno == EINTR);
    if (result != 0)
        throw std::system_error(errno, std::generic_category(),
                                "publish new " + std::string(label) + " bundle directory");
}

} // namespace rypath::mesh_bundle
