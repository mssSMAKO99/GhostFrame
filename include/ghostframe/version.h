// Single source of truth for the product version.
// Included by C++ code and by the resource compiler.
#pragma once

#define GF_VERSION_MAJOR 2
#define GF_VERSION_MINOR 0
#define GF_VERSION_PATCH 0
#define GF_VERSION_BUILD 0

// Resource compiler wants "a,b,c,d".
#define GF_VERSION_FILE \
    GF_VERSION_MAJOR, GF_VERSION_MINOR, GF_VERSION_PATCH, GF_VERSION_BUILD

// Stringized forms so the numbers above stay the single source of truth.
#define GF_VERSION_STRINGIZE_(value) #value
#define GF_VERSION_STRINGIZE(value) GF_VERSION_STRINGIZE_(value)

#define GF_VERSION_STRING_A \
    GF_VERSION_STRINGIZE(GF_VERSION_MAJOR) "." GF_VERSION_STRINGIZE(GF_VERSION_MINOR) "." \
    GF_VERSION_STRINGIZE(GF_VERSION_PATCH)

// "a.b.c.d", the form the resource compiler wants in StringFileInfo.
#define GF_VERSION_STRING4_A GF_VERSION_STRING_A "." GF_VERSION_STRINGIZE(GF_VERSION_BUILD)

#define GF_VERSION_STRING L"" GF_VERSION_STRING_A
