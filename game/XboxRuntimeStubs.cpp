//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox-specific stubs for minimal runtime compatibility
// With -fno-exceptions, these are rarely needed, but provided as fallback
//------------------------------------------------------------------------------------------------------------------------------------------

#include <cstdlib>

//------------------------------------------------------------------------------------------------------------------------------------------
// Shared helper: parse a decimal string into a double and update the pointer.
// Handles optional leading sign, integer digits, and a fractional part.
// Does NOT handle scientific notation or special values (inf/nan).
//------------------------------------------------------------------------------------------------------------------------------------------
static double parseDecimal(const char* str, const char** endOut) noexcept {
    const char* p = str;

    // Skip leading whitespace
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;

    int sign = 1;
    if (*p == '-') { sign = -1; ++p; }
    else if (*p == '+') { ++p; }

    const char* numStart = p;
    double result = 0.0;
    double frac   = 0.0;
    double fracDiv = 1.0;
    int    hasFrac = 0;

    while (*p >= '0' && *p <= '9') {
        result = result * 10.0 + (*p - '0');
        ++p;
    }
    if (*p == '.') {
        ++p;
        hasFrac = 1;
        while (*p >= '0' && *p <= '9') {
            frac    = frac * 10.0 + (*p - '0');
            fracDiv *= 10.0;
            ++p;
        }
    }

    if (p == numStart) {
        // No digits consumed - return str unchanged per C standard
        if (endOut) *endOut = str;
        return 0.0;
    }

    if (hasFrac) result += frac / fracDiv;
    if (endOut) *endOut = p;
    return sign * result;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert ASCII string to double - nxdk stub is assert(0), provide real implementation
//------------------------------------------------------------------------------------------------------------------------------------------
extern "C" double strtod(const char* str, char** endptr) {
    const char* end = str;
    double val = parseDecimal(str, &end);
    if (endptr) *endptr = const_cast<char*>(end);
    return val;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert ASCII string to float - nxdk stub is assert(0), provide real implementation
//------------------------------------------------------------------------------------------------------------------------------------------
extern "C" float strtof(const char* str, char** endptr) {
    return (float) strtod(str, endptr);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert ASCII string to double - missing from some nxdk paths
//------------------------------------------------------------------------------------------------------------------------------------------
extern "C" double atof(const char* str) {
    return strtod(str, nullptr);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// RTTI stub
//------------------------------------------------------------------------------------------------------------------------------------------
extern "C" void* __RTDynamicCast(void* inptr, int, void*, void*, int) {
    return nullptr;
}

