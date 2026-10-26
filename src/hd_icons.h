#pragma once

#include "runtime.h"

namespace rotk {

// HD skill icons: the "hd-icons" feature of the rotk.pc package. Loads the HD art packs shipped in
// the package (hd/<pack>/manifest.txt) into the renderer's HD tile registry and raises the internal
// resolution so the art draws at full detail. See docs/HD_ART.md.
void hdIconsRegister(gbarecomp::RunOptions& options);

}  // namespace rotk
