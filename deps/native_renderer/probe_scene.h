// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "renderer.h"
namespace tfnative {
// Four bounded 1080p correctness views, shared by the standalone tool and the
// opt-in in-process host test. Includes diagnostic readbacks; never benchmark it.
void runMeshProbe(Renderer& renderer, const std::filesystem::path& asset,
                  const std::filesystem::path& freshOutput);
}
