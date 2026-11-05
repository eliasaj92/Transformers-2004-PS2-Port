// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe_scene.h"
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) try {
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: tf_native_mesh_probe asset.bin NEW_OUTPUT_DIRECTORY mesh.hlsl [--debug]\n";
        return 2;
    }
    const bool debug = argc == 5 && std::wstring(argv[4]) == L"--debug";
    if (argc == 5 && !debug) throw std::runtime_error("Unknown option");
    tfnative::Renderer renderer(1920, 1080, L"NVIDIA GeForce RTX 3080 Laptop GPU", argv[3], debug);
    tfnative::runMeshProbe(renderer, argv[1], argv[2]);
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Native mesh probe failed: " << error.what() << '\n';
    return 1;
}
