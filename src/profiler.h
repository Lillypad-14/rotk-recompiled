#pragma once

namespace rotk {

// Test aid: a sampling profiler of the calling (emulation) thread, written to `path` at exit.
void profilerStart(const char* path);
// Samples only while on.
void profilerSample(bool on);

}  // namespace rotk
