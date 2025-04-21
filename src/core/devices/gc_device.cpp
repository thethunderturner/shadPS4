// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "gc_device.h"

namespace Core::Devices {

std::shared_ptr<BaseDevice> GcDevice::Create(u32 handle, const char*, int, u16) {
    return std::shared_ptr<BaseDevice>(
        reinterpret_cast<Devices::BaseDevice*>(new GcDevice(handle)));
}

int GcDevice::ioctl(u64 cmd, Common::VaCtx* args) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::write(const void* buf, size_t nbytes) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

size_t GcDevice::writev(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

size_t GcDevice::readv(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::preadv(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt, u64 offset) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::lseek(s64 offset, int whence) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::read(void* buf, size_t nbytes) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

int GcDevice::fstat(Libraries::Kernel::OrbisKernelStat* sb) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s32 GcDevice::fsync() {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

int GcDevice::ftruncate(s64 length) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

int GcDevice::getdents(void* buf, u32 nbytes, s64* basep) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::pwrite(const void* buf, size_t nbytes, u64 offset) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

} // namespace Core::Devices