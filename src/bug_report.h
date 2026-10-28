#pragma once

#include "runtime.h"

namespace rotk {

// The BugReport hotkey (default F12): gathers this session's replay, saves and settings into
// bug-reports/<time>/ beside the executable. See docs/BUG_REPORTS.md.
void bugReportRegister(gbarecomp::RunOptions& options);

}  // namespace rotk
