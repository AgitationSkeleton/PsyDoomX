#pragma once

#include "Macros.h"

#include <cstdint>

BEGIN_NAMESPACE(MobjSpritePrecacher)

void doPrecaching() noexcept;

// How much the last precache actually cached, for whoever wants to know what a level cost.
//
// Counted as it caches rather than worked out afterwards, so that it is the same set of lumps and cannot drift from
// what was really loaded.
int64_t getLastPrecachedBytes() noexcept;
int32_t getLastPrecachedLumps() noexcept;

END_NAMESPACE(MobjSpritePrecacher)
