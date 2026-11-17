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

#define GF_VERSION_STRING L"2.0.0"
#define GF_VERSION_STRING_A "2.0.0"
