// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/libs.h"
#include "libc_internal_memory.h"

namespace Libraries::LibcInternal {

void* PS4_SYSV_ABI internal_memset(void* s, int c, size_t n) {
    return std::memset(s, c, n);
}

void* PS4_SYSV_ABI internal_memcpy(void* dest, const void* src, size_t n) {
    return std::memcpy(dest, src, n);
}

s32 PS4_SYSV_ABI internal_memcpy_s(void* dest, size_t destsz, const void* src, size_t count) {
#ifdef _WIN64
    return memcpy_s(dest, destsz, src, count);
#else
    std::memcpy(dest, src, count);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_memcmp(const void* s1, const void* s2, size_t n) {
    return std::memcmp(s1, s2, n);
}

void* PS4_SYSV_ABI internal_memmove(void* dest, const void* src, size_t n) {
    return std::memmove(dest, src, n);
}

void* PS4_SYSV_ABI internal_operator_new(size_t size) {
    if (size == 0) {
        size = 1;
    }
    void* ptr = std::malloc(size);
    ASSERT_MSG(ptr != nullptr, "operator new failed to allocate {:#x} bytes", size);
    return ptr;
}

void PS4_SYSV_ABI internal_operator_delete(void* ptr) {
    if (ptr != nullptr) {
        std::free(ptr);
    }
}

struct SceLibcMallocManagedSize {
    u16 size;
    u16 version;
    u32 reserved1;
    u64 max_system_size;
    u64 current_system_size;
    u64 max_inuse_size;
    u64 current_inuse_size;
};

// Heap over a caller-provided (or self-allocated) region. Metadata lives on the
// host heap so a misbehaving game can't corrupt the allocator state.
class Mspace {
public:
    static constexpr size_t MinAlign = 16;

    Mspace(std::string_view name_, u8* base_, size_t capacity_, bool owns_memory_)
        : name{name_}, base{base_}, capacity{capacity_}, owns_memory{owns_memory_} {
        free_blocks.emplace(0, capacity);
    }

    ~Mspace() {
        if (owns_memory) {
            std::free(base);
        }
    }

    void* Allocate(size_t size, size_t align = MinAlign) {
        if (size == 0) {
            size = MinAlign;
        }
        align = std::max<size_t>(align, MinAlign);
        size = Common::AlignUp(size, MinAlign);
        std::scoped_lock lk{mutex};
        for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
            const size_t block_off = it->first;
            const size_t block_size = it->second;
            const size_t aligned_addr =
                Common::AlignUp(reinterpret_cast<size_t>(base) + block_off, align);
            const size_t front_pad = aligned_addr - reinterpret_cast<size_t>(base) - block_off;
            if (front_pad + size > block_size) {
                continue;
            }
            const size_t back_pad = block_size - front_pad - size;
            free_blocks.erase(it);
            if (front_pad != 0) {
                free_blocks.emplace(block_off, front_pad);
            }
            if (back_pad != 0) {
                free_blocks.emplace(block_off + front_pad + size, back_pad);
            }
            u8* const ptr = base + block_off + front_pad;
            allocations.emplace(ptr, size);
            current_inuse += size;
            max_inuse = std::max(max_inuse, current_inuse);
            return ptr;
        }
        LOG_WARNING(Lib_LibcInternal, "mspace '{}' out of memory, size = {:#x}, in use = {:#x}",
                    name, size, current_inuse);
        return nullptr;
    }

    bool Free(void* ptr) {
        std::scoped_lock lk{mutex};
        const auto it = allocations.find(static_cast<u8*>(ptr));
        if (it == allocations.end()) {
            return false;
        }
        size_t off = static_cast<u8*>(ptr) - base;
        size_t size = it->second;
        allocations.erase(it);
        current_inuse -= size;
        auto next = free_blocks.lower_bound(off);
        if (next != free_blocks.end() && off + size == next->first) {
            size += next->second;
            next = free_blocks.erase(next);
        }
        if (next != free_blocks.begin()) {
            const auto prev = std::prev(next);
            if (prev->first + prev->second == off) {
                off = prev->first;
                size += prev->second;
                free_blocks.erase(prev);
            }
        }
        free_blocks.emplace(off, size);
        return true;
    }

    size_t UsableSize(void* ptr) {
        std::scoped_lock lk{mutex};
        const auto it = allocations.find(static_cast<u8*>(ptr));
        return it != allocations.end() ? it->second : 0;
    }

    bool Contains(void* ptr) const {
        return ptr >= base && ptr < base + capacity;
    }

    bool IsEmpty() {
        std::scoped_lock lk{mutex};
        return allocations.empty();
    }

    void GetStats(SceLibcMallocManagedSize* stats) {
        std::scoped_lock lk{mutex};
        stats->max_system_size = capacity;
        stats->current_system_size = capacity;
        stats->max_inuse_size = max_inuse;
        stats->current_inuse_size = current_inuse;
    }

    std::string name;

private:
    u8* base;
    size_t capacity;
    bool owns_memory;
    std::mutex mutex;
    std::map<size_t, size_t> free_blocks;       // offset -> size
    std::unordered_map<u8*, size_t> allocations; // ptr -> usable size
    size_t current_inuse{};
    size_t max_inuse{};
};

static std::mutex g_mspace_mutex;
static std::vector<Mspace*> g_mspaces;

static Mspace* FindMspaceFor(void* ptr) {
    std::scoped_lock lk{g_mspace_mutex};
    for (Mspace* msp : g_mspaces) {
        if (msp->Contains(ptr)) {
            return msp;
        }
    }
    return nullptr;
}

void* PS4_SYSV_ABI internal_sceLibcMspaceCreate(const char* name, void* base, size_t capacity,
                                                u32 flag) {
    if (capacity == 0) {
        return nullptr;
    }
    bool owns_memory = false;
    if (base == nullptr) {
        base = std::malloc(capacity);
        if (base == nullptr) {
            return nullptr;
        }
        owns_memory = true;
    }
    auto* msp =
        new Mspace(name ? name : "", static_cast<u8*>(base), capacity, owns_memory);
    {
        std::scoped_lock lk{g_mspace_mutex};
        g_mspaces.push_back(msp);
    }
    LOG_INFO(Lib_LibcInternal, "name = '{}', base = {}, capacity = {:#x}, flag = {:#x}",
             msp->name, fmt::ptr(base), capacity, flag);
    return msp;
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceDestroy(Mspace* msp) {
    if (msp == nullptr) {
        return -1;
    }
    LOG_INFO(Lib_LibcInternal, "name = '{}'", msp->name);
    {
        std::scoped_lock lk{g_mspace_mutex};
        std::erase(g_mspaces, msp);
    }
    delete msp;
    return 0;
}

void* PS4_SYSV_ABI internal_sceLibcMspaceMalloc(Mspace* msp, size_t size) {
    if (msp == nullptr) {
        return nullptr;
    }
    return msp->Allocate(size);
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceFree(Mspace* msp, void* ptr) {
    if (msp == nullptr || ptr == nullptr) {
        return 0;
    }
    if (!msp->Free(ptr)) {
        LOG_WARNING(Lib_LibcInternal, "freeing unknown pointer {} from mspace '{}'",
                    fmt::ptr(ptr), msp->name);
    }
    return 0;
}

void* PS4_SYSV_ABI internal_sceLibcMspaceCalloc(Mspace* msp, size_t nelem, size_t size) {
    if (msp == nullptr || (size != 0 && nelem > std::numeric_limits<size_t>::max() / size)) {
        return nullptr;
    }
    void* ptr = msp->Allocate(nelem * size);
    if (ptr != nullptr) {
        std::memset(ptr, 0, nelem * size);
    }
    return ptr;
}

void* PS4_SYSV_ABI internal_sceLibcMspaceRealloc(Mspace* msp, void* ptr, size_t size) {
    if (msp == nullptr) {
        return nullptr;
    }
    if (ptr == nullptr) {
        return msp->Allocate(size);
    }
    if (size == 0) {
        msp->Free(ptr);
        return nullptr;
    }
    const size_t old_size = msp->UsableSize(ptr);
    if (size <= old_size) {
        return ptr;
    }
    void* new_ptr = msp->Allocate(size);
    if (new_ptr != nullptr) {
        std::memcpy(new_ptr, ptr, old_size);
        msp->Free(ptr);
    }
    return new_ptr;
}

void* PS4_SYSV_ABI internal_sceLibcMspaceMemalign(Mspace* msp, size_t boundary, size_t size) {
    if (msp == nullptr || !std::has_single_bit(boundary)) {
        return nullptr;
    }
    return msp->Allocate(size, boundary);
}

void* PS4_SYSV_ABI internal_sceLibcMspaceAlignedAlloc(Mspace* msp, size_t boundary, size_t size) {
    return internal_sceLibcMspaceMemalign(msp, boundary, size);
}

void* PS4_SYSV_ABI internal_sceLibcMspaceReallocalign(Mspace* msp, void* ptr, size_t boundary,
                                                      size_t size) {
    if (msp == nullptr || !std::has_single_bit(boundary)) {
        return nullptr;
    }
    if (ptr == nullptr) {
        return msp->Allocate(size, boundary);
    }
    if (size == 0) {
        msp->Free(ptr);
        return nullptr;
    }
    const size_t old_size = msp->UsableSize(ptr);
    if (size <= old_size && Common::IsAligned(reinterpret_cast<size_t>(ptr), boundary)) {
        return ptr;
    }
    void* new_ptr = msp->Allocate(size, boundary);
    if (new_ptr != nullptr) {
        std::memcpy(new_ptr, ptr, std::min(old_size, size));
        msp->Free(ptr);
    }
    return new_ptr;
}

s32 PS4_SYSV_ABI internal_sceLibcMspacePosixMemalign(Mspace* msp, void** memptr, size_t boundary,
                                                     size_t size) {
    if (msp == nullptr || memptr == nullptr || !std::has_single_bit(boundary) ||
        boundary % sizeof(void*) != 0) {
        return POSIX_EINVAL;
    }
    void* ptr = msp->Allocate(size, boundary);
    if (ptr == nullptr) {
        return POSIX_ENOMEM;
    }
    *memptr = ptr;
    return 0;
}

size_t PS4_SYSV_ABI internal_sceLibcMspaceMallocUsableSize(void* ptr) {
    if (ptr == nullptr) {
        return 0;
    }
    Mspace* msp = FindMspaceFor(ptr);
    return msp != nullptr ? msp->UsableSize(ptr) : 0;
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceMallocStats(Mspace* msp, SceLibcMallocManagedSize* stats) {
    if (msp == nullptr || stats == nullptr) {
        return -1;
    }
    msp->GetStats(stats);
    return 0;
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceMallocStatsFast(Mspace* msp,
                                                       SceLibcMallocManagedSize* stats) {
    return internal_sceLibcMspaceMallocStats(msp, stats);
}

s32 PS4_SYSV_ABI internal_sceLibcMspaceIsHeapEmpty(Mspace* msp) {
    if (msp == nullptr) {
        return 1;
    }
    return msp->IsEmpty() ? 1 : 0;
}

void RegisterlibSceLibcInternalMemory(Core::Loader::SymbolsResolver* sym) {

    LIB_FUNCTION("NFLs+dRJGNg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcpy_s);
    LIB_FUNCTION("Q3VBxCXhUHs", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcpy);
    LIB_FUNCTION("8zTFvBIAIN8", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memset);
    LIB_FUNCTION("DfivPArhucg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcmp);
    LIB_FUNCTION("+P6FRGH4LfA", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memmove);
    LIB_FUNCTION("fJnpuVVBbKk", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_operator_new);
    LIB_FUNCTION("z+P+xCnWLBk", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_operator_delete);

    LIB_FUNCTION("-hn1tcVHq5Q", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceCreate);
    LIB_FUNCTION("W6SiVSiCDtI", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceDestroy);
    LIB_FUNCTION("OJjm-QOIHlI", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceMalloc);
    LIB_FUNCTION("Vla-Z+eXlxo", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceFree);
    LIB_FUNCTION("LYo3GhIlB38", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceCalloc);
    LIB_FUNCTION("gigoVHZvVPE", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceRealloc);
    LIB_FUNCTION("iF1iQHzxBJU", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceMemalign);
    LIB_FUNCTION("ljkqMcC4-mk", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceAlignedAlloc);
    LIB_FUNCTION("p6lrRW8-MLY", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceReallocalign);
    LIB_FUNCTION("qWESlyXMI3E", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspacePosixMemalign);
    LIB_FUNCTION("fEoW6BJsPt4", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceMallocUsableSize);
    LIB_FUNCTION("mfHdJTIvhuo", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceMallocStats);
    LIB_FUNCTION("k04jLXu3+Ic", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceMallocStatsFast);
    LIB_FUNCTION("pzUa7KEoydw", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_sceLibcMspaceIsHeapEmpty);
}

} // namespace Libraries::LibcInternal
