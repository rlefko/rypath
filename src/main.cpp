// SPDX-License-Identifier: GPL-3.0-only

#include <iostream>
#include <string_view>

int main(int argc, char *argv[]) {
    if (argc == 1 || (argc == 2 && std::string_view{argv[1]} == "--help")) {
        std::cout << "Usage: rypath [--help | --version]\n";
        return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "rypath " RYPATH_VERSION "\n";
        return 0;
    }

    std::cerr << "rypath: unsupported arguments\n"
                 "Try 'rypath --help' for usage.\n";
    return 1;
}
