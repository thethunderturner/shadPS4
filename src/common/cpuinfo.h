// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

class CPUInfo {
    public:
        static std::string Vendor(void) { return CPU_Rep.vendor_; }
        static std::string Brand(void) { return CPU_Rep.brand_; }
}