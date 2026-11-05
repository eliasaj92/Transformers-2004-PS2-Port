// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tf_hwgs.h"
class GSDevice12;
class GSTexture12;
namespace tf_native_probe {
bool requested();
void enableHostValidation();
// Call only on the GS thread after flushing the host's command list. Diagnostic
// only: owns separate targets, never replaces a guest submission or presentation.
void afterSubmit(GSDevice12& device, GSTexture12& current, const TfGpuFrame& frame);
}
