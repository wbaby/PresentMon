// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
#pragma once
#include <Core/source/win/WinAPI.h>
#include <vector>
#include <Core/source/gfx/layout/Enums.h>
#include <Core/source/gfx/base/Geometry.h>
#include <Core/source/gfx/layout/style/Stylesheet.h>
#include <variant>


namespace p2c::kern
{
    enum class WidgetType
    {
        Graph,
        Readout,
    };

    struct GraphSpec
    {
        std::vector<size_t> metricIndices;
        gfx::lay::GraphType type;
        std::string tag;
    };

    struct ReadoutSpec
    {
        size_t metricIndex;
    };

    struct OverlaySpec
    {
        // types
        enum class OverlayPosition
        {
            TopLeft,
            TopRight,
            BottomLeft,
            BottomRight,
            Center,
        };
        // members
        DWORD pid;
        std::wstring capturePath;
        std::wstring captureName = L"pmcap";
        double graphDataWindowSize;
        double averagingWindowSize;
        double metricsOffset;
        OverlayPosition overlayPosition;
        std::vector<std::variant<GraphSpec, ReadoutSpec>> widgets;
        int overlayWidth;
        int samplingPeriodMs = 4;
        int samplesPerFrame = 4;
        uint32_t telemetrySamplingPeriodMs;
        bool hideDuringCapture;
        bool hideAlways;
        bool independentKernelWindow;
        std::vector<std::shared_ptr<gfx::lay::sty::Stylesheet>> sheets;
    };
}