#pragma once

#include <windows.h>
#include <string>

namespace gf {

void LogInit(const std::wstring& directory);
void LogShutdown();

// Directory the log file lives in, for the "open log" affordance.  Empty before
// LogInit or after LogShutdown.
const std::wstring& LogDirectory();
void LogLine(const wchar_t* format, ...);
void LogWin32(const wchar_t* what, DWORD error);

std::wstring FormatString(const wchar_t* format, ...);
std::wstring Win32Message(DWORD error);

}  // namespace gf
