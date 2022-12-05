// Copyright (C) 2019-2022 Intel Corporation
// SPDX-License-Identifier: MIT

#include "Debug.hpp"

#include "PresentMonTraceConsumer.hpp"

#include "ETW/Intel_Graphics_D3D10.h"
#include "ETW/Intel_PCAT_Metrics.h"
#include "ETW/Microsoft_Windows_D3D9.h"
#include "ETW/Microsoft_Windows_Dwm_Core.h"
#include "ETW/Microsoft_Windows_DXGI.h"
#include "ETW/Microsoft_Windows_DxgKrnl.h"
#include "ETW/Microsoft_Windows_Win32k.h"

#include <assert.h>
#include <dxgi.h>

namespace {

bool gVerboseTraceEnabled = false;

PresentEvent const* gModifiedPresent = nullptr;
PresentEvent gOriginalPresentValues;

LARGE_INTEGER* gFirstTimestamp = nullptr;
LARGE_INTEGER gTimestampFrequency = {};

uint64_t ConvertTimestampDeltaToNs(uint64_t timestampDelta)
{
    return 1000000000ull * timestampDelta / gTimestampFrequency.QuadPart;
}

uint64_t ConvertTimestampToNs(uint64_t timestamp)
{
    return ConvertTimestampDeltaToNs(timestamp - gFirstTimestamp->QuadPart);
}

char* AddCommas(uint64_t t)
{
    static char buf[128];
    auto r = sprintf_s(buf, "%llu", t);

    auto commaCount = r == 0 ? 0 : ((r - 1) / 3);
    for (int i = 0; i < commaCount; ++i) {
        auto p = r + commaCount - 4 * i;
        auto q = r - 3 * i;
        buf[p - 1] = buf[q - 1];
        buf[p - 2] = buf[q - 2];
        buf[p - 3] = buf[q - 3];
        buf[p - 4] = ',';
    }

    r += commaCount;
    buf[r] = '\0';
    return buf;
}

void PrintFloat(float value) { printf("%g", value); }
void PrintU32(uint32_t value) { printf("%u", value); }
void PrintU64(uint64_t value) { printf("%llu", value); }
void PrintU64x(uint64_t value) { printf("0x%llx", value); }
void PrintBool(bool value) { printf("%s", value ? "true" : "false"); }
void PrintRuntime(Runtime value)
{
    switch (value) {
    case Runtime::DXGI:  printf("DXGI");  break;
    case Runtime::D3D9:  printf("D3D9");  break;
    case Runtime::Other: printf("Other"); break;
    default:             printf("Unknown (%u)", value); assert(false); break;
    }
}
void PrintPresentMode(PresentMode value)
{
    switch (value) {
    case PresentMode::Unknown:                              printf("Unknown"); break;
    case PresentMode::Hardware_Legacy_Flip:                 printf("Hardware_Legacy_Flip"); break;
    case PresentMode::Hardware_Legacy_Copy_To_Front_Buffer: printf("Hardware_Legacy_Copy_To_Front_Buffer"); break;
    case PresentMode::Hardware_Independent_Flip:            printf("Hardware_Independent_Flip"); break;
    case PresentMode::Composed_Flip:                        printf("Composed_Flip"); break;
    case PresentMode::Composed_Copy_GPU_GDI:                printf("Composed_Copy_GPU_GDI"); break;
    case PresentMode::Composed_Copy_CPU_GDI:                printf("Composed_Copy_CPU_GDI"); break;
    case PresentMode::Hardware_Composed_Independent_Flip:   printf("Hardware_Composed_Independent_Flip"); break;
    default:                                                printf("Unknown (%u)", value); assert(false); break;
    }
}
void PrintPresentResult(PresentResult value)
{
    switch (value) {
    case PresentResult::Unknown:   printf("Unknown");   break;
    case PresentResult::Presented: printf("Presented"); break;
    case PresentResult::Discarded: printf("Discarded"); break;
    default:                       printf("Unknown (%u)", value); assert(false); break;
    }
}
void PrintPresentHistoryModel(uint32_t model)
{
    using namespace Microsoft_Windows_DxgKrnl;
    switch (model) {
    case PresentModel::D3DKMT_PM_UNINITIALIZED:          printf("UNINITIALIZED");          break;
    case PresentModel::D3DKMT_PM_REDIRECTED_GDI:         printf("REDIRECTED_GDI");         break;
    case PresentModel::D3DKMT_PM_REDIRECTED_FLIP:        printf("REDIRECTED_FLIP");        break;
    case PresentModel::D3DKMT_PM_REDIRECTED_BLT:         printf("REDIRECTED_BLT");         break;
    case PresentModel::D3DKMT_PM_REDIRECTED_VISTABLT:    printf("REDIRECTED_VISTABLT");    break;
    case PresentModel::D3DKMT_PM_SCREENCAPTUREFENCE:     printf("SCREENCAPTUREFENCE");     break;
    case PresentModel::D3DKMT_PM_REDIRECTED_GDI_SYSMEM:  printf("REDIRECTED_GDI_SYSMEM");  break;
    case PresentModel::D3DKMT_PM_REDIRECTED_COMPOSITION: printf("REDIRECTED_COMPOSITION"); break;
    case PresentModel::D3DKMT_PM_SURFACECOMPLETE:        printf("SURFACECOMPLETE");        break;
    case PresentModel::D3DKMT_PM_FLIPMANAGER:            printf("FLIPMANAGER");            break;
    default:                                             printf("Unknown (%u)", model); assert(false); break;
    }
}
void PrintQueuePacketType(uint32_t type)
{
    using namespace Microsoft_Windows_DxgKrnl;
    switch (type) {
    case QueuePacketType::DXGKETW_RENDER_COMMAND_BUFFER:   printf("RENDER"); break;
    case QueuePacketType::DXGKETW_DEFERRED_COMMAND_BUFFER: printf("DEFERRED"); break;
    case QueuePacketType::DXGKETW_SYSTEM_COMMAND_BUFFER:   printf("SYSTEM"); break;
    case QueuePacketType::DXGKETW_MMIOFLIP_COMMAND_BUFFER: printf("MMIOFLIP"); break;
    case QueuePacketType::DXGKETW_WAIT_COMMAND_BUFFER:     printf("WAIT"); break;
    case QueuePacketType::DXGKETW_SIGNAL_COMMAND_BUFFER:   printf("SIGNAL"); break;
    case QueuePacketType::DXGKETW_DEVICE_COMMAND_BUFFER:   printf("DEVICE"); break;
    case QueuePacketType::DXGKETW_SOFTWARE_COMMAND_BUFFER: printf("SOFTWARE"); break;
    case QueuePacketType::DXGKETW_PAGING_COMMAND_BUFFER:   printf("PAGING"); break;
    default:                                               printf("Unknown (%u)", type); assert(false); break;
    }
}
void PrintDmaPacketType(uint32_t type)
{
    using namespace Microsoft_Windows_DxgKrnl;
    switch (type) {
    case DmaPacketType::DXGKETW_CLIENT_RENDER_BUFFER:    printf("CLIENT_RENDER"); break;
    case DmaPacketType::DXGKETW_CLIENT_PAGING_BUFFER:    printf("CLIENT_PAGING"); break;
    case DmaPacketType::DXGKETW_SYSTEM_PAGING_BUFFER:    printf("SYSTEM_PAGING"); break;
    case DmaPacketType::DXGKETW_SYSTEM_PREEMTION_BUFFER: printf("SYSTEM_PREEMTION"); break;
    default:                                             printf("Unknown (%u)", type); assert(false); break;
    }
}
void PrintPresentFlags(uint32_t flags)
{
    if (flags & DXGI_PRESENT_TEST) printf("TEST");
}

void PrintEventHeader(EVENT_HEADER const& hdr)
{
    printf("%16s %5u %5u ", AddCommas(ConvertTimestampToNs(hdr.TimeStamp.QuadPart)), hdr.ProcessId, hdr.ThreadId);
}

void PrintEventHeader(EVENT_HEADER const& hdr, char const* name)
{
    PrintEventHeader(hdr);
    printf("%s\n", name);
}

void PrintEventHeader(EVENT_RECORD* eventRecord, EventMetadata* metadata, char const* name, std::initializer_list<void*> props)
{
    assert((props.size() % 2) == 0);

    PrintEventHeader(eventRecord->EventHeader);
    printf("%s", name);
    for (auto ii = props.begin(), ie = props.end(); ii != ie; ++ii) {
        auto propName = (wchar_t const*) *ii; ++ii;
        auto propFunc = *ii;

        printf(" %ls=", propName);

             if (propFunc == PrintBool)                 PrintBool(metadata->GetEventData<uint32_t>(eventRecord, propName) != 0);
        else if (propFunc == PrintU32)                  PrintU32(metadata->GetEventData<uint32_t>(eventRecord, propName));
        else if (propFunc == PrintU64)                  PrintU64(metadata->GetEventData<uint64_t>(eventRecord, propName));
        else if (propFunc == PrintU64x)                 PrintU64x(metadata->GetEventData<uint64_t>(eventRecord, propName));
        else if (propFunc == PrintTime)                 PrintTime(metadata->GetEventData<uint64_t>(eventRecord, propName));
        else if (propFunc == PrintTimeDelta)            PrintTimeDelta(metadata->GetEventData<uint64_t>(eventRecord, propName));
        else if (propFunc == PrintQueuePacketType)      PrintQueuePacketType(metadata->GetEventData<uint32_t>(eventRecord, propName));
        else if (propFunc == PrintDmaPacketType)        PrintDmaPacketType(metadata->GetEventData<uint32_t>(eventRecord, propName));
        else if (propFunc == PrintPresentFlags)         PrintPresentFlags(metadata->GetEventData<uint32_t>(eventRecord, propName));
        else if (propFunc == PrintPresentHistoryModel)  PrintPresentHistoryModel(metadata->GetEventData<uint32_t>(eventRecord, propName));
        else assert(false);
    }
    printf("\n");
}

void FlushModifiedPresent()
{
    if (gModifiedPresent == nullptr) return;

    uint32_t changedCount = 0;

#ifdef NDEBUG
#define PRINT_PRESENT_ID(_P)
#else
#define PRINT_PRESENT_ID(_P) printf("p%llu", gModifiedPresent->Id)
#endif
#define FLUSH_MEMBER(_Fn, _Name) \
    if (gModifiedPresent->_Name != gOriginalPresentValues._Name) { \
        if (changedCount++ == 0) { \
            printf("%*s", 17 + 6 + 6, ""); \
            PRINT_PRESENT_ID(gModifiedPresent); \
        } \
        printf(" " #_Name "="); \
        _Fn(gOriginalPresentValues._Name); \
        printf("->"); \
        _Fn(gModifiedPresent->_Name); \
    }
    FLUSH_MEMBER(PrintTime,          PresentStopTime)
    FLUSH_MEMBER(PrintTime,          ReadyTime)
    FLUSH_MEMBER(PrintTime,          ScreenTime)
    FLUSH_MEMBER(PrintTime,          InputTime)
    FLUSH_MEMBER(PrintTime,          GPUStartTime)
    FLUSH_MEMBER(PrintTimeDelta,     GPUDuration)
    FLUSH_MEMBER(PrintTimeDelta,     GPUVideoDuration)
    FLUSH_MEMBER(PrintU64x,          SwapChainAddress)
    FLUSH_MEMBER(PrintU32,           SyncInterval)
    FLUSH_MEMBER(PrintU32,           PresentFlags)
    FLUSH_MEMBER(PrintU64x,          Hwnd)
    FLUSH_MEMBER(PrintU64x,          DxgkPresentHistoryToken)
    FLUSH_MEMBER(PrintU32,           QueueSubmitSequence)
    FLUSH_MEMBER(PrintU32,           DriverThreadId)
    FLUSH_MEMBER(PrintPresentMode,   PresentMode)
    FLUSH_MEMBER(PrintPresentResult, FinalState)
    FLUSH_MEMBER(PrintBool,          SupportsTearing)
    FLUSH_MEMBER(PrintBool,          WaitForFlipEvent)
    FLUSH_MEMBER(PrintBool,          WaitForMPOFlipEvent)
    FLUSH_MEMBER(PrintBool,          SeenDxgkPresent)
    FLUSH_MEMBER(PrintBool,          SeenWin32KEvents)
    FLUSH_MEMBER(PrintBool,          DwmNotified)
    FLUSH_MEMBER(PrintBool,          IsCompleted)
    FLUSH_MEMBER(PrintBool,          IsLost)
    FLUSH_MEMBER(PrintU32,           DeferredCompletionWaitCount)
    FLUSH_MEMBER(PrintTime,          INTC_ProducerPresentTime)
    FLUSH_MEMBER(PrintTime,          INTC_ConsumerPresentTime)
    FLUSH_MEMBER(PrintTimeDelta,     INTC_Timer[INTC_TIMER_WAIT_IF_FULL].mAccumulatedTime)
    FLUSH_MEMBER(PrintTimeDelta,     INTC_Timer[INTC_TIMER_WAIT_IF_EMPTY].mAccumulatedTime)
    FLUSH_MEMBER(PrintTimeDelta,     INTC_Timer[INTC_TIMER_WAIT_UNTIL_EMPTY_SYNC].mAccumulatedTime)
    FLUSH_MEMBER(PrintTimeDelta,     INTC_Timer[INTC_TIMER_WAIT_UNTIL_EMPTY_DRAIN].mAccumulatedTime)
    FLUSH_MEMBER(PrintTimeDelta,     INTC_Timer[INTC_TIMER_WAIT_FOR_FENCE].mAccumulatedTime)
    FLUSH_MEMBER(PrintTimeDelta,     INTC_Timer[INTC_TIMER_WAIT_UNTIL_FENCE_SUBMITTED].mAccumulatedTime)
    FLUSH_MEMBER(PrintTimeDelta,     INTC_Timers[INTC_GPU_TIMER_SYNC_TYPE_WAIT_SYNC_OBJECT_CPU])
    FLUSH_MEMBER(PrintTimeDelta,     INTC_Timers[INTC_GPU_TIMER_SYNC_TYPE_POLL_ON_QUERY_GET_DATA])
    FLUSH_MEMBER(PrintU64,           INTC_FrameID)
#undef FLUSH_MEMBER

    if (changedCount > 0) {
        printf("\n");
    }

    gModifiedPresent = nullptr;
}

uint64_t LookupPresentId(
    PMTraceConsumer* pmConsumer,
    uint64_t CompositionSurfaceLuid,
    uint64_t PresentCount,
    uint64_t BindId)
{
#ifdef NDEBUG
    (void) pmConsumer, CompositionSurfaceLuid, PresentCount, BindId;
#else
    // pmConsumer can complete presents before they've seen all of
    // their TokenStateChanged_Info events, so we keep a copy of the
    // token->present id map here simply so we can print what present
    // the event refers to.
    static std::unordered_map<
        PMTraceConsumer::Win32KPresentHistoryToken,
        uint64_t,
        PMTraceConsumer::Win32KPresentHistoryTokenHash> tokenToIdMap;

    PMTraceConsumer::Win32KPresentHistoryToken key(CompositionSurfaceLuid, PresentCount, BindId);
    auto ii = pmConsumer->mPresentByWin32KPresentHistoryToken.find(key);
    if (ii != pmConsumer->mPresentByWin32KPresentHistoryToken.end()) {
        tokenToIdMap[key] = ii->second->Id;
        return ii->second->Id;
    }

    auto jj = tokenToIdMap.find(key);
    if (jj != tokenToIdMap.end()) {
        return jj->second;
    }
#endif

    return 0;
}

}

int PrintTime(uint64_t value)
{
    return value == 0
        ? printf("0")
        : value < (uint64_t) gFirstTimestamp->QuadPart
            ? printf("-%s", AddCommas(ConvertTimestampDeltaToNs((uint64_t) gFirstTimestamp->QuadPart - value)))
            : printf("%s", AddCommas(ConvertTimestampToNs(value)));
}

int PrintTimeDelta(uint64_t value)
{
    return printf("%s", value == 0 ? "0" : AddCommas(ConvertTimestampDeltaToNs(value)));
}

#ifndef NDEBUG
void EnableVerboseTrace(bool enable)
{
    gVerboseTraceEnabled = enable;
}

bool IsVerboseTraceEnabled()
{
    return gVerboseTraceEnabled;
}
#endif

void DebugAssertImpl(wchar_t const* msg, wchar_t const* file, int line)
{
    if (IsVerboseTraceEnabled()) {
        printf("ASSERTION FAILED: %ls(%d): %ls\n", file, line, msg);
    } else {
        #ifndef NDEBUG
        _wassert(msg, file, line);
        #endif
    }
}

void InitializeVerboseTrace(LARGE_INTEGER* firstTimestamp, LARGE_INTEGER const& timestampFrequency)
{
    gFirstTimestamp = firstTimestamp;
    gTimestampFrequency = timestampFrequency;

    printf("       Time (ns)   PID   TID EVENT\n");
}

void VerboseTraceEvent(PMTraceConsumer* pmConsumer, EVENT_RECORD* eventRecord, EventMetadata* metadata)
{
    auto const& hdr = eventRecord->EventHeader;
    auto id = hdr.EventDescriptor.Id;

    FlushModifiedPresent();

    if (hdr.ProviderId == Microsoft_Windows_D3D9::GUID) {
        using namespace Microsoft_Windows_D3D9;
        switch (id) {
        case Present_Start::Id: PrintEventHeader(hdr, "D3D9PresentStart"); break;
        case Present_Stop::Id:  PrintEventHeader(hdr, "D3D9PresentStop"); break;
        }
        return;
    }

    if (hdr.ProviderId == Microsoft_Windows_DXGI::GUID) {
        using namespace Microsoft_Windows_DXGI;
        switch (id) {
        case Present_Start::Id:                  PrintEventHeader(eventRecord, metadata, "DXGIPresent_Start",    { L"Flags", PrintPresentFlags, }); break;
        case PresentMultiplaneOverlay_Start::Id: PrintEventHeader(eventRecord, metadata, "DXGIPresentMPO_Start", { L"Flags", PrintPresentFlags, }); break;
        case Present_Stop::Id:                   PrintEventHeader(hdr, "DXGIPresent_Stop"); break;
        case PresentMultiplaneOverlay_Stop::Id:  PrintEventHeader(hdr, "DXGIPresentMPO_Stop"); break;
        }
        return;
    }

    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::BLT_GUID)            { PrintEventHeader(hdr, "Win7::BLT"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::FLIP_GUID)           { PrintEventHeader(hdr, "Win7::FLIP"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::PRESENTHISTORY_GUID) { PrintEventHeader(hdr, "Win7::PRESENTHISTORY"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::QUEUEPACKET_GUID)    { PrintEventHeader(hdr, "Win7::QUEUEPACKET"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::VSYNCDPC_GUID)       { PrintEventHeader(hdr, "Win7::VSYNCDPC"); return; }
    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::Win7::MMIOFLIP_GUID)       { PrintEventHeader(hdr, "Win7::MMIOFLIP"); return; }

    if (hdr.ProviderId == Microsoft_Windows_DxgKrnl::GUID) {
        using namespace Microsoft_Windows_DxgKrnl;
        switch (id) {
        case Blit_Info::Id:                     PrintEventHeader(hdr, "Blit_Info"); break;
        case BlitCancel_Info::Id:               PrintEventHeader(hdr, "BlitCancel_Info"); break;
        case FlipMultiPlaneOverlay_Info::Id:    PrintEventHeader(hdr, "FlipMultiPlaneOverlay_Info"); break;
        case Present_Info::Id:                  PrintEventHeader(hdr, "DxgKrnl_Present_Info"); break;
        case MakeResident_Start::Id:            PrintEventHeader(hdr, "MakeResident_Start"); break;
        case MakeResident_Stop::Id:             PrintEventHeader(hdr, "MakeResident_Stop"); break;

        case MMIOFlip_Info::Id:                 PrintEventHeader(eventRecord, metadata, "MMIOFlip_Info",                { L"FlipSubmitSequence", PrintU64, }); break;
        case Flip_Info::Id:                     PrintEventHeader(eventRecord, metadata, "Flip_Info",                    { L"FlipInterval",   PrintU32,
                                                                                                                          L"MMIOFlip",       PrintBool, }); break;
        case IndependentFlip_Info::Id:          PrintEventHeader(eventRecord, metadata, "IndependentFlip_Info",         { L"SubmitSequence", PrintU32,
                                                                                                                          L"FlipInterval",   PrintU32, }); break;
        case PresentHistory_Start::Id:          PrintEventHeader(eventRecord, metadata, "PresentHistory_Start",         { L"Token",          PrintU64x,
                                                                                                                          L"Model",          PrintPresentHistoryModel, }); break;
        case PresentHistory_Info::Id:           PrintEventHeader(eventRecord, metadata, "PresentHistory_Info",          { L"Token",          PrintU64x,
                                                                                                                          L"Model",          PrintPresentHistoryModel, }); break;
        case PresentHistoryDetailed_Start::Id:  PrintEventHeader(eventRecord, metadata, "PresentHistoryDetailed_Start", { L"Token",          PrintU64x,
                                                                                                                          L"Model",          PrintPresentHistoryModel, }); break;
        case QueuePacket_Start::Id:             PrintEventHeader(eventRecord, metadata, "QueuePacket_Start",            { L"hContext",       PrintU64x,
                                                                                                                          L"SubmitSequence", PrintU32,
                                                                                                                          L"PacketType",     PrintQueuePacketType,
                                                                                                                          L"bPresent",       PrintU32, }); break;
        case QueuePacket_Start_2::Id:           PrintEventHeader(eventRecord, metadata, "QueuePacket_Start WAIT",       { L"hContext",       PrintU64x,
                                                                                                                          L"SubmitSequence", PrintU32, }); break;
        case QueuePacket_Stop::Id:              PrintEventHeader(eventRecord, metadata, "QueuePacket_Stop",             { L"hContext",       PrintU64x,
                                                                                                                          L"SubmitSequence", PrintU32, }); break;
        case Context_DCStart::Id:
        case Context_Start::Id:                 PrintEventHeader(eventRecord, metadata, "Context_Start",                { L"hContext",       PrintU64x,
                                                                                                                          L"hDevice",        PrintU64x,
                                                                                                                          L"NodeOrdinal",    PrintU32, }); break;
        case Context_Stop::Id:                  PrintEventHeader(eventRecord, metadata, "Context_Stop",                 { L"hContext",       PrintU64x, }); break;
        case Device_DCStart::Id:
        case Device_Start::Id:                  PrintEventHeader(eventRecord, metadata, "Device_Start",                 { L"hDevice",        PrintU64x,
                                                                                                                          L"pDxgAdapter",    PrintU64x, }); break;
        case Device_Stop::Id:                   PrintEventHeader(eventRecord, metadata, "Device_Stop",                  { L"hDevice",        PrintU64x, }); break;
        case HwQueue_DCStart::Id:
        case HwQueue_Start::Id:                 PrintEventHeader(eventRecord, metadata, "HwQueue_Start",                { L"hContext", PrintU64x,
                                                                                                                          L"hHwQueue", PrintU64x,
                                                                                                                          L"ParentDxgHwQueue", PrintU64x, }); break;
        case DmaPacket_Info::Id:                PrintEventHeader(eventRecord, metadata, "DmaPacket_Info",               { L"hContext",       PrintU64x,
                                                                                                                          L"ulQueueSubmitSequence", PrintU32,
                                                                                                                          L"PacketType",     PrintDmaPacketType, }); break;
        case DmaPacket_Start::Id:               PrintEventHeader(eventRecord, metadata, "DmaPacket_Start",              { L"hContext",       PrintU64x,
                                                                                                                          L"ulQueueSubmitSequence", PrintU32, }); break;
        case PagingQueuePacket_Start::Id:       PrintEventHeader(eventRecord, metadata, "PagingQueuePacket_Start",      { L"SequenceId",     PrintU64 }); break;
        case PagingQueuePacket_Info::Id:        PrintEventHeader(eventRecord, metadata, "PagingQueuePacket_Info",       { L"SequenceId",     PrintU64 }); break;
        case PagingQueuePacket_Stop::Id:        PrintEventHeader(eventRecord, metadata, "PagingQueuePacket_Stop",       { L"SequenceId",     PrintU64 }); break;

        case VSyncDPC_Info::Id: {
            auto FlipFenceId = metadata->GetEventData<uint64_t>(eventRecord, L"FlipFenceId");
            PrintEventHeader(hdr);
            printf("VSyncDPC_Info SubmitSequence=%llu FlipId=0x%llx\n",
                FlipFenceId >> 32,
                FlipFenceId & 0xffffffffll);
            break;
        }
        case HSyncDPCMultiPlane_Info::Id:
        case VSyncDPCMultiPlane_Info::Id: {
            EventDataDesc desc[] = {
                { L"FlipEntryCount" },
                { L"FlipSubmitSequence" },
            };

            metadata->GetEventData(eventRecord, desc, _countof(desc));
            auto FlipEntryCount     = desc[0].GetData<uint32_t>();
            auto FlipSubmitSequence = desc[1].GetArray<uint64_t>(FlipEntryCount);

            PrintEventHeader(hdr);
            printf("%s", id == HSyncDPCMultiPlane_Info::Id ? "HSyncDPCMultiPlane_Info" : "VSyncDPCMultiPlane_Info");
            for (uint32_t i = 0; i < FlipEntryCount; ++i) {
                if (i > 0) printf("\n                                                    ");
                printf(" SubmitSequence[%u]=%llu FlipId[%u]=0x%llx",
                    i, FlipSubmitSequence[i] >> 32,
                    i, FlipSubmitSequence[i] & 0xffffffffll);
            }
            printf("\n");
            break;
        }
        case MMIOFlipMultiPlaneOverlay_Info::Id: {
            auto FlipSubmitSequence = metadata->GetEventData<uint64_t>(eventRecord, L"FlipSubmitSequence");
            PrintEventHeader(hdr);
            printf("DXGKrnl_MMIOFlipMultiPlaneOverlay_Info SubmitSequence=%llu FlipId=0x%llx",
                FlipSubmitSequence >> 32,
                FlipSubmitSequence & 0xffffffffll);
            if (hdr.EventDescriptor.Version >= 2) {
                switch (metadata->GetEventData<uint32_t>(eventRecord, L"FlipEntryStatusAfterFlip")) {
                case FlipEntryStatus::FlipWaitVSync:    printf(" FlipWaitVSync"); break;
                case FlipEntryStatus::FlipWaitComplete: printf(" FlipWaitComplete"); break;
                case FlipEntryStatus::FlipWaitHSync:    printf(" FlipWaitHSync"); break;
                }
            }
            printf("\n");
            break;
        }
        }
        return;
    }

    if (hdr.ProviderId == Microsoft_Windows_Dwm_Core::GUID ||
        hdr.ProviderId == Microsoft_Windows_Dwm_Core::Win7::GUID) {
        using namespace Microsoft_Windows_Dwm_Core;
        switch (id) {
        case MILEVENT_MEDIA_UCE_PROCESSPRESENTHISTORY_GetPresentHistory_Info::Id:
                                              PrintEventHeader(hdr, "DWM_GetPresentHistory"); break;
        case SCHEDULE_PRESENT_Start::Id:      PrintEventHeader(hdr, "DWM_SCHEDULE_PRESENT_Start"); break;
        case FlipChain_Pending::Id:           PrintEventHeader(hdr, "DWM_FlipChain_Pending"); break;
        case FlipChain_Complete::Id:          PrintEventHeader(hdr, "DWM_FlipChain_Complete"); break;
        case FlipChain_Dirty::Id:             PrintEventHeader(hdr, "DWM_FlipChain_Dirty"); break;
        case SCHEDULE_SURFACEUPDATE_Info::Id: PrintEventHeader(hdr, "DWM_Schedule_SurfaceUpdate"); break;
        }
        return;
    }

    if (hdr.ProviderId == Microsoft_Windows_Win32k::GUID) {
        using namespace Microsoft_Windows_Win32k;
        switch (id) {
        case TokenCompositionSurfaceObject_Info::Id: PrintEventHeader(hdr, "Win32k_TokenCompositionSurfaceObject"); break;
        case InputDeviceRead_Stop::Id:               PrintEventHeader(eventRecord, metadata, "Win32k_InputDeviceRead_Stop", { L"DeviceType", PrintU32, }); break;
        case RetrieveInputMessage_Info::Id:          PrintEventHeader(eventRecord, metadata, "Win32k_RetrieveInputMessage", { L"flags",      PrintU32, }); break;
        case TokenStateChanged_Info::Id: {
            EventDataDesc desc[] = {
                { L"CompositionSurfaceLuid" },
                { L"PresentCount" },
                { L"BindId" },
                { L"NewState" },
            };
            metadata->GetEventData(eventRecord, desc, _countof(desc));
            auto CompositionSurfaceLuid = desc[0].GetData<uint64_t>();
            auto PresentCount           = desc[1].GetData<uint32_t>();
            auto BindId                 = desc[2].GetData<uint64_t>();
            auto NewState               = desc[3].GetData<uint32_t>();

            PrintEventHeader(hdr);
            printf("Win32K_TokenStateChanged");

            auto presentId = LookupPresentId(pmConsumer, CompositionSurfaceLuid, PresentCount, BindId);
            if (presentId == 0) {
                printf(" (unknown present)");
            } else {
                printf(" p%llu", presentId);
            }

            switch (NewState) {
            case TokenState::Completed: printf(" NewState=Completed"); break;
            case TokenState::InFrame:   printf(" NewState=InFrame");   break;
            case TokenState::Confirmed: printf(" NewState=Confirmed"); break;
            case TokenState::Retired:   printf(" NewState=Retired");   break;
            case TokenState::Discarded: printf(" NewState=Discarded"); break;
            default:                    printf(" NewState=Unknown (%u)", NewState); assert(false); break;
            }

            printf("\n");
        }   break;
        }
        return;
    }

    if (hdr.ProviderId == Intel_Graphics_D3D10::GUID) {
        using namespace Intel_Graphics_D3D10;
        switch (id) {
        case QueueTimers_Info::Id:  PrintEventHeader(eventRecord, metadata, "INTC_QueueTimers_Info",  { L"value", PrintU32 }); break;
        case QueueTimers_Start::Id: PrintEventHeader(eventRecord, metadata, "INTC_QueueTimers_Start", { L"value", PrintU32 }); break;
        case QueueTimers_Stop::Id:  PrintEventHeader(eventRecord, metadata, "INTC_QueueTimers_Stop",  { L"value", PrintU32 }); break;
        case CpuGpuSync_Start::Id:  PrintEventHeader(eventRecord, metadata, "INTC_CpuGpuSync_Start",  { L"value", PrintU32 }); break;
        case CpuGpuSync_Stop::Id:   PrintEventHeader(eventRecord, metadata, "INTC_CpuGpuSync_Stop",   { L"value", PrintU32 }); break;

        case ShaderCompilationTrackingEvents_Start_3::Id: PrintEventHeader(hdr, "INTC_ShaderCompilationTrackingEvents_Start_3"); break;
        case ShaderCompilationTrackingEvents_Stop_3::Id:  PrintEventHeader(hdr, "INTC_ShaderCompilationTrackingEvents_Stop_3"); break;
        case ShaderCompilationTrackingEvents_Start_4::Id: PrintEventHeader(hdr, "INTC_ShaderCompilationTrackingEvents_Start_4"); break;
        case ShaderCompilationTrackingEvents_Stop_4::Id:  PrintEventHeader(hdr, "INTC_ShaderCompilationTrackingEvents_Stop_4"); break;

        case task_DdiPresentDXGI_Info::Id:
            /* BEGIN WORKAROUND: if the manifest isn't installed nor embedded
             * into the ETL this won't be able to lookup properties
            PrintEventHeader(eventRecord, metadata, "INTC_DdiPresentDXGI_Info", {
                L"FrameID", PrintU64,
            });
            */
            {
                PrintEventHeader(eventRecord->EventHeader);
                printf("INTC_DdiPresentDXGI_Info");

                EventDataDesc desc = { L"FrameID" };
                metadata->GetEventData(eventRecord, &desc, 1, 1);
                if (desc.status_ & PROP_STATUS_FOUND) {
                    PrintU32(desc.GetData<uint32_t>());
                }
                printf("\n");
            }
            /* END WORKAROUND */
            break;
        case task_FramePacer_Info::Id:
            /* BEGIN WORKAROUND: if the manifest isn't installed nor embedded
             * into the ETL this won't be able to lookup properties
            PrintEventHeader(eventRecord, metadata, "INTC_FramePacer_Info", {
                L"FrameID", PrintU64,
            });
            */
            {
                PrintEventHeader(eventRecord->EventHeader);
                printf("INTC_FramePacer_Info");

                EventDataDesc desc = { L"FrameID" };
                metadata->GetEventData(eventRecord, &desc, 1, 1);
                if (desc.status_ & PROP_STATUS_FOUND) {
                    PrintU32(desc.GetData<uint32_t>());
                }
            }
            /* END WORKAROUND */
#if 0
            printf(  "%*sAppWorkStart            = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"AppWorkStart"));
            printf("\n%*sAppSimulationTime       = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"AppSimulationTime"));
            printf("\n%*sDriverWorkStart         = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"DriverWorkStart"));
            printf("\n%*sDriverWorkEnd           = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"DriverWorkEnd"));
            printf("\n%*sKernelDriverSubmitStart = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"KernelDriverSubmitStart"));
            printf("\n%*sKernelDriverSubmitEnd   = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"KernelDriverSubmitEnd"));
            printf("\n%*sGPUStart                = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"GPUStart"));
            printf("\n%*sGPUEnd                  = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"GPUEnd"));
            printf("\n%*sKernelDriverFenceReport = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"KernelDriverFenceReport"));
            printf("\n%*sPresentAPICall          = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"PresentAPICall"));
            printf("\n%*sTargetFrameTime         = ", 29, ""); PrintTimeDelta(metadata->GetEventData<uint64_t>(eventRecord, L"TargetFrameTime"));
            printf("\n%*sFlipReceivedTime        = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"FlipReceivedTime"));
            printf("\n%*sFlipReportTime          = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"FlipReportTime"));
            printf("\n%*sFlipProgrammingTime     = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"FlipProgrammingTime"));
            printf("\n%*sActualFlipTime          = ", 29, ""); PrintTime(metadata->GetEventData<uint64_t>(eventRecord, L"ActualFlipTime"));
#endif
            printf("\n");
            break;
        }
        return;
    }

    if (hdr.ProviderId == Intel_PCAT_Metrics::GUID) {
        switch (id) {
        case Intel_PCAT_Metrics::Task_0_Opcode_0::Id: PrintEventHeader(hdr, "INTC_PCAT"); break;
        }
        return;
    }
}

void VerboseTraceBeforeModifyingPresentImpl(PresentEvent const* p)
{
    if (gModifiedPresent != p) {
        FlushModifiedPresent();
        gModifiedPresent = p;
        if (p != nullptr) {
            gOriginalPresentValues = *p;
        }
    }
}
