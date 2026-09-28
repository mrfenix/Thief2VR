#pragma once
#include <string>

// SHA-256 of the running game exe, lowercase hex. Used to pick the per-build
// address table (see docs/re-map.md).
std::string ExeSha256();
