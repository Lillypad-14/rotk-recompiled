#pragma once

#include <string>
#include <vector>

#include "runtime.h"

// Runs the recomp-ui pre-boot launcher. Returns non-zero when the player quit.
int rotk_launcher_preboot(std::vector<std::string>& args, const gbarecomp::RunOptions& options);
