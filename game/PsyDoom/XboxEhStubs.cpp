#if defined(__XBOX__)

#include <climits>

extern "C" {

void __std_terminate() {
    for (;;) {
    }
}

int __cdecl __CxxFrameHandler3(...) {
    return 0;
}

__declspec(noreturn) void __stdcall _CxxThrowException(void*, void*) {
    __std_terminate();
}

//------------------------------------------------------------------------------------------------------------------
// Xbox-safe replacements for threadsafe_statics (libxboxrt).
//
// The nxdk implementation in threadsafe_statics.c uses a __declspec(thread) variable (_Init_thread_epoch).
// On Xbox, nxdk stores a NEGATIVE value in _tls_index so that the Windows TLS access pattern
// (FS:[0x2C] + _tls_index*4) resolves via the TLS self-pointer trick. For the MAIN THREAD this
// setup may not be complete when C++ global constructors fire, causing an immediate crash.
//
// These replacements provide thread-UNSAFE (single-threaded) magic static initialization that
// avoids ANY __declspec(thread) / TLS access. On Xbox startup is effectively single-threaded
// during global construction so this is safe.
//
// By defining these symbols here (in a directly-linked .obj), the linker won't pull in
// threadsafe_statics.obj from libxboxrt.lib, thus removing _Init_thread_epoch from the binary.
//------------------------------------------------------------------------------------------------------------------
long _Init_global_epoch = LONG_MIN;

// NOTE: this uses __declspec(thread) only as a dummy — we define it so the symbol exists
// but we bypass the standard TLS access in the header/footer below.
__declspec(thread) long _Init_thread_epoch = LONG_MIN;

void _Init_thread_header(volatile int* ptss) {
    if (*ptss == 0) {
        *ptss = -1;  // Mark as "being initialized" so we proceed to initialize
    }
    // If *ptss is already -1 or a positive epoch value, do nothing —
    // the compiler-generated code checks for -1 to decide whether to run the initializer.
}

void _Init_thread_footer(volatile int* ptss) {
    *ptss = ++_Init_global_epoch;
    // Note: we intentionally skip updating _Init_thread_epoch (TLS variable) here.
    // The consequence is that the per-thread fast-path cache is never updated,
    // so every magic-static call does a slow-path check. Correct, but slightly slower.
}

void _Init_thread_abort(volatile int* ptss) {
    *ptss = 0;  // Reset guard so initialization can be retried
}

}

#endif
