// Copyright (C) 2020-2023 Intel Corporation
// SPDX-License-Identifier: MIT
//
// This file originally generated by etw_list
//     version:    public 1b19f39ddb669f7a700a5d0c16cf079943e996d5
//     parameters: --no_event_structs --event=Present::Start --event=Present::Stop --provider=Microsoft-Windows-D3D9
#pragma once

namespace Microsoft_Windows_D3D9 {

struct __declspec(uuid("{783ACA0A-790E-4D7F-8451-AA850511C6B9}")) GUID_STRUCT;
static const auto GUID = __uuidof(GUID_STRUCT);

enum class Keyword : uint64_t {
    Events                               = 0x2,
    Microsoft_Windows_Direct3D9_Analytic = 0x8000000000000000,
};

enum class Level : uint8_t {
    win_LogAlways = 0x0,
};

enum class Channel : uint8_t {
    Microsoft_Windows_Direct3D9_Analytic = 0x10,
};

// Event descriptors:
#define EVENT_DESCRIPTOR_DECL(name_, id_, version_, channel_, level_, opcode_, task_, keyword_) struct name_ { \
    static uint16_t const Id      = id_; \
    static uint8_t  const Version = version_; \
    static uint8_t  const Channel = channel_; \
    static uint8_t  const Level   = level_; \
    static uint8_t  const Opcode  = opcode_; \
    static uint16_t const Task    = task_; \
    static Keyword  const Keyword = (Keyword) keyword_; \
};

EVENT_DESCRIPTOR_DECL(Present_Start, 0x0001, 0x00, 0x10, 0x00, 0x01, 0x0001, 0x8000000000000002)
EVENT_DESCRIPTOR_DECL(Present_Stop , 0x0002, 0x00, 0x10, 0x00, 0x02, 0x0001, 0x8000000000000002)

#undef EVENT_DESCRIPTOR_DECL

enum class D3D9PresentFlags : uint32_t {
    D3DPRESENT_DONOTWAIT = 1,
    D3DPRESENT_LINEAR_CONTENT = 2,
    D3DPRESENT_DONOTFLIP = 4,
    D3DPRESENT_FLIPRESTART = 8,
    D3DPRESENT_VIDEO_RESTRICT_TO_MONITOR = 16,
    D3DPRESENT_FORCEIMMEDIATE = 256,
};

}
