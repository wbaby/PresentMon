// Copyright (C) 2020-2021 Intel Corporation
// SPDX-License-Identifier: MIT
//
// This file originally generated by etw_list
//     version:    development branch e5985e637875db6cb6f90e8c92d0857b6fb95324
//     parameters: --provider=Intel-PCAT-Metrics --event=*
#pragma once

namespace Intel_PCAT_Metrics {

struct __declspec(uuid("{1B3CF3CC-8167-47D2-9B6A-8E99043CCAFC}")) GUID_STRUCT;
static const auto GUID = __uuidof(GUID_STRUCT);

enum class Keyword : uint64_t {
    Intel_PCAT_Metrics_Analytic = 0x8000000000000000,
};

enum class Level : uint8_t {
    win_Informational = 0x4,
};

enum class Channel : uint8_t {
    Intel_PCAT_Metrics_Analytic = 0x10,
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

EVENT_DESCRIPTOR_DECL(Task_0_Opcode_0, 0x0001, 0x00, 0x10, 0x04, 0x00, 0x0000, 0x8000000000000000)

#undef EVENT_DESCRIPTOR_DECL

#pragma warning(push)
#pragma warning(disable: 4200) // nonstandard extension used: zero-sized array in struct
#pragma pack(push)
#pragma pack(1)

struct Task_0_Opcode_0_Struct {
    int64_t     frequency;
    int64_t     system_tick;
    float       timestamp;
    float       p1_12v_8p_v;
    float       p1_12v_8p_a;
    float       p2_12v_8p_v;
    float       p2_12v_8p_a;
    float       p3_12v_8p_v;
    float       p3_12v_8p_a;
    float       pcie_12v_v;
    float       pcie_12v_a;
    float       three_point_three_v;
    float       three_point_three_a;
    float       three_point_three_aux_v;
    float       three_point_three_aux_a;
};

#pragma pack(pop)
#pragma warning(pop)

}
