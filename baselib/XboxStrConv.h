#pragma once
//-----------------------------------------------------------------------------
// XboxStrConv.h — Xbox-safe replacements for nxdk's assert(0) strtof/strtod.
//
// nxdk's libxboxrt provides strtof() and strtod() as stub bodies that always
// call assert(0), aborting immediately.  Include this header (AFTER all system
// headers) in any Xbox translation unit that needs std::strtof / std::strtod,
// then call XboxStrConv::parseFloat() / XboxStrConv::parseDouble() instead.
//
// Handles: optional leading sign, integer digits, optional fractional part.
// Does NOT handle: scientific notation (1e3), Inf, NaN.  These are not needed
// by PsyDoom's MAPINFO / DECORATE / INI parsers.
//-----------------------------------------------------------------------------

#if defined(__XBOX__)

namespace XboxStrConv {

    inline double parseDouble(const char* str, char** endptr) noexcept {
        if (!str) { if (endptr) *endptr = nullptr; return 0.0; }

        const char* p = str;

        // Skip leading ASCII whitespace
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;

        int sign = 1;
        if      (*p == '-') { sign = -1; ++p; }
        else if (*p == '+') {             ++p; }

        const char* numStart = p;
        double val  = 0.0;
        double frac = 0.0;
        double div  = 1.0;
        bool   hasFrac = false;

        while (*p >= '0' && *p <= '9') {
            val = val * 10.0 + static_cast<double>(*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
            hasFrac = true;
            while (*p >= '0' && *p <= '9') {
                frac = frac * 10.0 + static_cast<double>(*p - '0');
                div *= 10.0;
                ++p;
            }
        }

        // No digits consumed — per C standard, return 0 and leave pointer at start
        if (p == numStart) {
            if (endptr) *endptr = const_cast<char*>(str);
            return 0.0;
        }

        if (hasFrac) val += frac / div;
        if (endptr)  *endptr = const_cast<char*>(p);
        return static_cast<double>(sign) * val;
    }

    inline float parseFloat(const char* str, char** endptr) noexcept {
        return static_cast<float>(parseDouble(str, endptr));
    }

} // namespace XboxStrConv

#endif // defined(__XBOX__)
