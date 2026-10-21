#pragma once
// #137 build number (version + build date + short git hash): generated at configure time from runtime/build_info.cpp.in
// (CMakeLists.txt); shown on the in-game menu's System page and written to the log at start.
namespace build_info {
const char* version();   // VERSION file at the repo root
const char* date();      // the configure time, YYYY-MM-DD HH:MM
const char* git();       // short hash of HEAD, "+" when tracked files are modified; "nogit" without git
const char* text();      // "v<version> · <date> · <git>"
}  // namespace build_info
