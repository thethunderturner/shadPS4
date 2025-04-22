// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <memory>
#include "base_device.h"

namespace Core::Devices {

class GcDevice final : BaseDevice {
    u32 handle;

public:
    static std::shared_ptr<BaseDevice> Create(u32 handle, const char*, int, u16);
    explicit GcDevice(u32 handle) : handle(handle) {}

    ~GcDevice() override = default;

    int ioctl(u64 cmd, Common::VaCtx* args) override;
    s64 write(const void* buf, size_t nbytes) override;
    size_t readv(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt) override;
    size_t writev(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt) override;
    s64 preadv(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt, u64 offset) override;
    s64 lseek(s64 offset, int whence) override;
    s64 read(void* buf, size_t nbytes) override;
    int fstat(Libraries::Kernel::OrbisKernelStat* sb) override;
    s32 fsync() override;
    int ftruncate(s64 length) override;
    int getdents(void* buf, u32 nbytes, s64* basep) override;
    s64 pwrite(const void* buf, size_t nbytes, u64 offset) override;
private:
    enum class GcCommands : u64 {
        FlushGarlic = 0xc0048114,
        SubmitDone = 0xc0048116,
        WaitIdle = 0xc0048117,
        WaitFree = 0xc004811d,
        GetNumTcaUnits = 0xc004811f,
        SwitchBuffer = 0xc0088101,
        DebugHardwareStatus = 0xc0088111,
        InitializeSubmits = 0xc008811b,
        UnmapComputeQueue = 0xc00c810e,
        SetGsRingSizes = 0xc00c8110,
        Submit = 0xc0108102,
        GetCuMask = 0xc010810b,
        DingDong = 0xc010811c,
        RequiresNeoCompat = 0xc0108120,
        SubmitEop = 0xc020810c,
        MapComputeQueue = 0xc030810d,
        MapComputeQueueWithPriority = 0xc030811a,
        SetWaveLimitMultipliers = 0xc030811e,
        MipStatsReport = 0xc0848119,
    };

    struct SetGsRingSizesArgs {
        u32 esgs_ring_size;
        u32 gsvs_ring_size;
        u32 unk;
    };

    struct SubmitArgs {
        u32 pid;
        u32 count;
        u64* cmds;
    };
    
    struct SubmitEopArgs {
        u32 pid;
        u32 count;
        u64* cmds;
        u64 eop_v;
        s32 wait;
    };

    struct MapComputeQueueArgs {
        u32 pipe_hi;
        u32 pipe_lo;
        u32 queue_id;
        u32 g_queue_id;
        VAddr ring_base_addr;
        u32* read_ptr_addr;
        VAddr ding_dong_ptr;
        u32 ring_size_dw;
        u32 pipe_priority;
    };

    struct SetWaveLimitMultipliersArgs {
        s32 bitset;
        s32 values[8];
        s32 unk0;
        s32 unk1;
        s32 unk2;
    };

    struct SetMipStatsReportArgs {
        u32 type;
        u32 unk0;
        u32 unk1;
        u32 unk2;
    };
};

} // namespace Core::Devices