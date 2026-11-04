// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Optional read-only diagnostics. The adapter calls these on its ordered
// producer; the implementation marshals all trace state onto the GS thread.
void tfGpuDrawTraceOpen();
void tfGpuDrawTraceField();
void tfGpuDrawTraceClose();
