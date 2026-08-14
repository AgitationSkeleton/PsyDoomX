#pragma once

#include "Macros.h"
#include "XboxStrConv.h"

#include <cerrno>
#include <cstdlib>
#include <functional>
#include <string>
#include <cstdint>

BEGIN_NAMESPACE(IniUtils)

//------------------------------------------------------------------------------------------------------------------------------------------
// Stores the value for an entry in an INI file
//------------------------------------------------------------------------------------------------------------------------------------------
struct IniValue {
    std::string strValue;

    template <class T>
    inline void set(const T& newValue) noexcept {
        if constexpr (std::is_same_v<T, std::string>) {
            strValue = newValue;
        } else if constexpr (std::is_same_v<T, bool>) {
            if (newValue) {
                strValue = "1";
            } else {
                strValue = "0";
            }
        } else {
            strValue = std::to_string(newValue);
        }
    }

    inline const std::string& getAsString() const noexcept {
        return strValue;
    }

    inline bool getAsBool() const THROWS {
        const bool isLiteralTrue = (
            (strValue.length() == 4) &&
            (strValue[0] == 't' || strValue[0] == 'T') &&
            (strValue[1] == 'r' || strValue[1] == 'R') &&
            (strValue[2] == 'u' || strValue[2] == 'U') &&
            (strValue[3] == 'e' || strValue[3] == 'E')
        );

        if (isLiteralTrue)
            return true;

        const bool isLiteralFalse = (
            (strValue.length() == 5) &&
            (strValue[0] == 'f' || strValue[0] == 'F') &&
            (strValue[1] == 'a' || strValue[1] == 'A') &&
            (strValue[2] == 'l' || strValue[2] == 'L') &&
            (strValue[3] == 's' || strValue[3] == 'S') &&
            (strValue[4] == 'e' || strValue[4] == 'E')
        );

        if (isLiteralFalse)
            return false;

        const int32_t intValue = getAsInt();
        return (intValue > 0);
    }

    inline bool tryGetAsBool(const bool bDefaultValue) const noexcept {
#if defined(__XBOX__)
        // No exceptions on Xbox: attempt bool conversion manually
        const int32_t intVal = tryGetAsInt(bDefaultValue ? 1 : 0);
        return (intVal > 0);
#else
        try {
            return getAsBool();
        } catch (...) {
            return bDefaultValue;
        }
#endif
    }

    inline int32_t getAsInt() const THROWS {
        return std::stoi(strValue);
    }

    inline int32_t tryGetAsInt(const int32_t defaultValue) const noexcept {
#if defined(__XBOX__)
        if (strValue.empty()) return defaultValue;
        char* end = nullptr;
        errno = 0;
        long val = std::strtol(strValue.c_str(), &end, 10);
        return (end == strValue.c_str() || errno != 0) ? defaultValue : (int32_t)val;
#else
        try {
            return getAsInt();
        } catch (...) {
            return defaultValue;
        }
#endif
    }

    inline uint32_t getAsUint() const THROWS {
        return (uint32_t) std::stoul(strValue);
    }

    inline uint32_t tryGetAsUint(const uint32_t defaultValue) const noexcept {
#if defined(__XBOX__)
        if (strValue.empty()) return defaultValue;
        char* end = nullptr;
        errno = 0;
        unsigned long val = std::strtoul(strValue.c_str(), &end, 10);
        return (end == strValue.c_str() || errno != 0) ? defaultValue : (uint32_t)val;
#else
        try {
            return getAsUint();
        } catch (...) {
            return defaultValue;
        }
#endif
    }

    inline float getAsFloat() const THROWS {
        return std::stof(strValue);
    }

    inline float tryGetAsFloat(const float defaultValue) const noexcept {
#if defined(__XBOX__)
        if (strValue.empty()) return defaultValue;
        char* end = nullptr;
        float val = XboxStrConv::parseFloat(strValue.c_str(), &end);
        return (end == strValue.c_str()) ? defaultValue : val;
#else
        try {
            return getAsFloat();
        } catch (...) {
            return defaultValue;
        }
#endif
    }
};

//------------------------------------------------------------------------------------------------------------------------------------------
// Represents an entry in an INI file
//------------------------------------------------------------------------------------------------------------------------------------------
struct IniEntry {
    std::string     section;
    std::string     key;
    IniValue        value;
};

// Represents a callback/handler that receives parsed ini entries (SAX style).
// Note: I would make this function prototype 'noexcept' but std::function<> doesn't handle that so well currently...
typedef std::function<void (const IniEntry& entry)> IniEntryHandler;

void parseIniFromString(const char* const pStr, const size_t len, const IniEntryHandler entryHandler) noexcept;

END_NAMESPACE(IniUtils)
