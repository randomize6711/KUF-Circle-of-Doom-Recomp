// The game's mid-asm hooks (config/v1/codegen.toml) are referenced by the recompiled code; the game defines them in
// src/runtime. These tests run only pure routines (zlib, CRC) that never reach a hooked address, so the hooks are
// stubs here.

#include <rex/ppc.h>

void KufUiQuadHook(PPCRegister& /*r25*/, PPCRegister& /*r30*/) {}  // display.cpp: the 2D renderer's quads
