//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox-specific stubs for missing C++ runtime symbols that nxdk doesn't provide
// These are weak symbols that allow the linker to proceed when exception handling or RTTI features are used
//------------------------------------------------------------------------------------------------------------------------------------------

#include <cstdlib>
#include <cstring>

//------------------------------------------------------------------------------------------------------------------------------------------
// Exception handling stubs
//------------------------------------------------------------------------------------------------------------------------------------------

// Undefined termination handler - called when an exception cannot be caught
extern "C" void ___std_terminate(void) {
    // Terminate the process gracefully
    std::abort();
}

// Exception handler frame  - called during C++ exception cleanup
extern "C" void ___CxxFrameHandler3(void) {
    std::abort();
}

// Throw exception - should not be reached if exceptions are properly handled
extern "C" void __CxxThrowException(void*, void*) {
    std::abort();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// C runtime stubs
//------------------------------------------------------------------------------------------------------------------------------------------

// Convert ASCII string to double - apparently missing from nxdk's libc
extern "C" double atof(const char* str) {
    if (!str) return 0.0;
    
    // Simple implementation that handles basic cases
    double result = 0.0;
    double factor = 1.0;
    int decimalFound = 0;
    int negativeSign = 0;
    
    // Handle sign
    if (*str == '-') {
        negativeSign = 1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Process digits
    while (*str) {
        if (*str >= '0' && *str <= '9') {
            result = result * 10.0 + (*str - '0');
            if (decimalFound) {
                factor *= 10.0;
            }
        } else if (*str == '.' && !decimalFound) {
            decimalFound = 1;
        } else {
            break;
        }
        str++;
    }
    
    if (decimalFound) {
        result /= factor;
    }
    
    return negativeSign ? -result : result;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// RTTI stubs
//------------------------------------------------------------------------------------------------------------------------------------------

// Dynamic cast - if reached, just return nullptr (unsafe but better than undefined symbol)
extern "C" void* __RTDynamicCast(void* inptr, int, void*, void*, int) {
    return nullptr;
}
