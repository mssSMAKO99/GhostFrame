// Resource ids of the payloads the launcher embeds as RCDATA.
//
// The resource script defines them and the core injector loads them, so the ids
// live in one place to keep the two from drifting apart.  This header is plain
// preprocessor so it can also be included from a resource script.
#pragma once

#define IDR_AGENT_X64 201
#define IDR_AGENT_X86 202
#define IDR_HELPER_X86 203
