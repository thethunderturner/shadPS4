// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <common/va_ctx.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal.h"
#include "libc_internal_io.h"
#include "libc_internal_math.h"
#include "libc_internal_memory.h"
#include "libc_internal_str.h"
#include "libc_internal_threads.h"
#include "printf.h"

namespace Libraries::LibcInternal {

// Itanium C++ ABI runtime support.
s32 PS4_SYSV_ABI internal_cxa_guard_acquire(u64* guard) {
    return *reinterpret_cast<volatile u8*>(guard) == 0 ? 1 : 0;
}

void PS4_SYSV_ABI internal_cxa_guard_release(u64* guard) {
    *reinterpret_cast<volatile u8*>(guard) = 1;
}

void PS4_SYSV_ABI internal_cxa_guard_abort(u64* guard) {
    *reinterpret_cast<volatile u8*>(guard) = 0;
}

s32 PS4_SYSV_ABI internal_cxa_atexit(void (*func)(void*), void* arg, void* dso) {
    // Destructors at process exit are not run by the emulator.
    return 0;
}

void PS4_SYSV_ABI internal_cxa_finalize(void* dso) {}

void PS4_SYSV_ABI internal_cxa_pure_virtual() {
    LOG_CRITICAL(Lib_LibcInternal, "Pure virtual function called!");
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    RegisterlibSceLibcInternalMath(sym);
    RegisterlibSceLibcInternalStr(sym);
    RegisterlibSceLibcInternalMemory(sym);
    RegisterlibSceLibcInternalIo(sym);
    RegisterlibSceLibcInternalThreads(sym);

    LIB_FUNCTION("3GPpjQdAMTw", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_cxa_guard_acquire);
    LIB_FUNCTION("9rAeANT2tyE", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_cxa_guard_release);
    LIB_FUNCTION("2emaaluWzUw", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_cxa_guard_abort);
    LIB_FUNCTION("tsvEmnenz48", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_cxa_atexit);
    LIB_FUNCTION("H2e8t5ScQGc", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_cxa_finalize);
    LIB_FUNCTION("zr094EQ39Ww", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_cxa_pure_virtual);
}

void ForceRegisterLib(Core::Loader::SymbolsResolver* sym) {
    // Used to forcibly enable HLEs for broken LLE functions.
    ForceRegisterlibSceLibcInternalIo(sym);
}
} // namespace Libraries::LibcInternal