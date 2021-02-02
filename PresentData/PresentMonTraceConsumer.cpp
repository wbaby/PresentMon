/*
Copyright 2017-2021 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "PresentMonTraceConsumer.hpp"

#include "ETW/Microsoft_Windows_D3D9.h"
#include "ETW/Microsoft_Windows_Dwm_Core.h"
#include "ETW/Microsoft_Windows_DXGI.h"
#include "ETW/Microsoft_Windows_DxgKrnl.h"
#include "ETW/Microsoft_Windows_EventMetadata.h"
#include "ETW/Microsoft_Windows_Win32k.h"

#include <algorithm>
#include <assert.h>
#include <d3d9.h>
#include <dxgi.h>

#ifdef DEBUG
static constexpr int PRESENTEVENT_CIRCULAR_BUFFER_SIZE = 32768;
#else
static constexpr int PRESENTEVENT_CIRCULAR_BUFFER_SIZE = 8192;
#endif

// These macros, when enabled, record what PresentMon analysis below was done
// for each present.  The primary use case is to compute usage statistics and
// ensure test coverage.
//
// Add a TRACK_PRESENT_PATH() calls to every location that represents a unique
// analysis path.  e.g., as a starting point this might be one for every ETW
// event used below, with further instrumentation when there is different
// handling based on event property values.
//
// If the location is in a function that can be called by multiple parents, use
// TRACK_PRESENT_SAVE_PATH_ID() instead and call
// TRACK_PRESENT_GENERATE_PATH_ID() in each parent.
#ifdef TRACK_PRESENT_PATHS
#define TRACK_PRESENT_PATH_SAVE_ID(present, id) present->AnalysisPath |= 1ull << (id % 64)
#define TRACK_PRESENT_PATH(present) do { \
    enum { TRACK_PRESENT_PATH_ID = __COUNTER__ }; \
    TRACK_PRESENT_PATH_SAVE_ID(present, TRACK_PRESENT_PATH_ID); \
} while (0)
#define TRACK_PRESENT_PATH_GENERATE_ID()              mAnalysisPathID = __COUNTER__
#define TRACK_PRESENT_PATH_SAVE_GENERATED_ID(present) TRACK_PRESENT_PATH_SAVE_ID(present, mAnalysisPathID)
#else
#define TRACK_PRESENT_PATH(present)                   (void) present
#define TRACK_PRESENT_PATH_GENERATE_ID()
#define TRACK_PRESENT_PATH_SAVE_GENERATED_ID(present) (void) present
#endif

PresentEvent::PresentEvent(EVENT_HEADER const& hdr, ::Runtime runtime)
    : QpcTime(*(uint64_t*) &hdr.TimeStamp)
    , ProcessId(hdr.ProcessId)
    , ThreadId(hdr.ThreadId)
    , TimeTaken(0)
    , ReadyTime(0)
    , ScreenTime(0)
    , SwapChainAddress(0)
    , SyncInterval(-1)
    , PresentFlags(0)
    , Hwnd(0)
    , TokenPtr(0)
    , CompositionSurfaceLuid(0)
    , GPUDuration(0)
    , GPUVideoDuration(0)
    , QueueSubmitSequence(0)
    , DestWidth(0)
    , DestHeight(0)
    , DriverBatchThreadId(0)
    , Runtime(runtime)
    , PresentMode(PresentMode::Unknown)
    , FinalState(PresentResult::Unknown)
    , SupportsTearing(false)
    , MMIO(false)
    , SeenDxgkPresent(false)
    , SeenWin32KEvents(false)
    , DwmNotified(false)
    , Completed(false)
    , IsLost(false)
    , mAllPresentsTrackingIndex(0)
    , DxgKrnlHContext(0)
    , Win32KPresentCount(0)
    , Win32KBindId(0)
    , LegacyBlitTokenData(0)
    , PresentInDwmWaitingStruct(false)
{
#ifdef TRACK_PRESENT_PATHS
    AnalysisPath = 0ull;
#endif

#if DEBUG_VERBOSE
    static uint64_t presentCount = 0;
    presentCount += 1;
    Id = presentCount;
#endif
}

PMTraceConsumer::PMTraceConsumer()
    : mAllPresents(PRESENTEVENT_CIRCULAR_BUFFER_SIZE)
{
}

void PMTraceConsumer::HandleD3D9Event(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);

    auto const& hdr = pEventRecord->EventHeader;

    if (!IsProcessTrackedForFiltering(hdr.ProcessId)) {
        return;
    }

    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_D3D9::Present_Start::Id:
    {
        EventDataDesc desc[] = {
            { L"pSwapchain" },
            { L"Flags" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto pSwapchain = desc[0].GetData<uint64_t>();
        auto Flags      = desc[1].GetData<uint32_t>();

        auto present = std::make_shared<PresentEvent>(hdr, Runtime::D3D9);
        present->SwapChainAddress = pSwapchain;
        present->PresentFlags =
            ((Flags & D3DPRESENT_DONOTFLIP) ? DXGI_PRESENT_DO_NOT_SEQUENCE : 0) |
            ((Flags & D3DPRESENT_DONOTWAIT) ? DXGI_PRESENT_DO_NOT_WAIT : 0) |
            ((Flags & D3DPRESENT_FLIPRESTART) ? DXGI_PRESENT_RESTART : 0);
        if ((Flags & D3DPRESENT_FORCEIMMEDIATE) != 0) {
            present->SyncInterval = 0;
        }

        TrackPresentOnThread(present);
        TRACK_PRESENT_PATH(present);
        break;
    }
    case Microsoft_Windows_D3D9::Present_Stop::Id:
    {
        auto result = mMetadata.GetEventData<uint32_t>(pEventRecord, L"Result");

        bool AllowBatching =
            SUCCEEDED(result) &&
            result != S_PRESENT_OCCLUDED;

        RuntimePresentStop(hdr, AllowBatching, Runtime::D3D9);
        break;
    }
    default:
        assert(!mFilteredEvents); // Assert that filtering is working if expected
        break;
    }
}

void PMTraceConsumer::HandleDXGIEvent(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);

    auto const& hdr = pEventRecord->EventHeader;

    if (!IsProcessTrackedForFiltering(hdr.ProcessId)) {
        return;
    }

    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_DXGI::Present_Start::Id:
    case Microsoft_Windows_DXGI::PresentMultiplaneOverlay_Start::Id:
    {
        EventDataDesc desc[] = {
            { L"pIDXGISwapChain" },
            { L"Flags" },
            { L"SyncInterval" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto pIDXGISwapChain = desc[0].GetData<uint64_t>();
        auto Flags           = desc[1].GetData<uint32_t>();
        auto SyncInterval    = desc[2].GetData<int32_t>();

        // Ignore PRESENT_TEST: it's just to check if you're still fullscreen
        if ((Flags & DXGI_PRESENT_TEST) != 0) {
            // mPresentByThreadId isn't cleaned up properly when non-runtime
            // presents (e.g. created by Dxgk via FindOrCreatePresent())
            // complete.  So we need to clear mPresentByThreadId here to
            // prevent the corresponding Present_Stop event from modifying
            // anything.
            //
            // TODO: Perhaps the better solution is to not have
            // FindOrCreatePresent() add to the thread tracking?
            mPresentByThreadId.erase(hdr.ThreadId);
            break;
        }

        auto present = std::make_shared<PresentEvent>(hdr, Runtime::DXGI);
        present->SwapChainAddress = pIDXGISwapChain;
        present->PresentFlags     = Flags;
        present->SyncInterval     = SyncInterval;

        TrackPresentOnThread(present);
        TRACK_PRESENT_PATH(present);
        break;
    }
    case Microsoft_Windows_DXGI::Present_Stop::Id:
    case Microsoft_Windows_DXGI::PresentMultiplaneOverlay_Stop::Id:
    {
        auto result = mMetadata.GetEventData<uint32_t>(pEventRecord, L"Result");

        bool AllowBatching =
            SUCCEEDED(result) &&
            result != DXGI_STATUS_OCCLUDED &&
            result != DXGI_STATUS_MODE_CHANGE_IN_PROGRESS &&
            result != DXGI_STATUS_NO_DESKTOP_ACCESS;

        RuntimePresentStop(hdr, AllowBatching, Runtime::DXGI);
        break;
    }
    default:
        assert(!mFilteredEvents); // Assert that filtering is working if expected
        break;
    }
}

void PMTraceConsumer::HandleDxgkBlt(EVENT_HEADER const& hdr, uint64_t hwnd, bool redirectedPresent)
{
    // Lookup the in-progress present.  It should not have a known present mode
    // yet, so PresentMode!=Unknown implies we looked up a 'stuck' present
    // whose tracking was lost for some reason.
    auto presentEvent = FindOrCreatePresent(hdr);
    if (presentEvent == nullptr) {
        return;
    }

    if (presentEvent->PresentMode != PresentMode::Unknown) {
        RemoveLostPresent(presentEvent);
        presentEvent = FindOrCreatePresent(hdr);
        if (presentEvent == nullptr) {
            return;
        }
        assert(presentEvent->PresentMode == PresentMode::Unknown);
    }

    TRACK_PRESENT_PATH_SAVE_GENERATED_ID(presentEvent);

    // This could be one of several types of presents. Further events will clarify.
    // For now, assume that this is a blt straight into a surface which is already on-screen.
    presentEvent->Hwnd = hwnd;
    if (redirectedPresent) {
        TRACK_PRESENT_PATH(presentEvent);
        presentEvent->PresentMode = PresentMode::Composed_Copy_CPU_GDI;
        presentEvent->SupportsTearing = false;
    } else {
        presentEvent->PresentMode = PresentMode::Hardware_Legacy_Copy_To_Front_Buffer;
        presentEvent->SupportsTearing = true;
    }
}

void PMTraceConsumer::HandleDxgkBltCancel(EVENT_HEADER const& hdr)
{
    // There are cases where a present blt can be optimized out in kernel.
    // In such cases, we return success to the caller, but issue no further work
    // for the present. Mark these cases as discarded.
    auto eventIter = mPresentByThreadId.find(hdr.ThreadId);

    if (eventIter != mPresentByThreadId.end()) {
        TRACK_PRESENT_PATH(eventIter->second);
        eventIter->second->FinalState = PresentResult::Discarded;
        CompletePresent(eventIter->second);
    }
}

void PMTraceConsumer::HandleDxgkFlip(EVENT_HEADER const& hdr, int32_t flipInterval, bool mmio)
{
    // A flip event is emitted during fullscreen present submission.
    // Afterwards, expect an MMIOFlip packet on the same thread, used to trace
    // the flip to screen.

    // Lookup the in-progress present.  The only events that we expect before a
    // Flip/FlipMPO are a runtime present start, or a previous FlipMPO.  If
    // that's not the case, we assume that correct tracking has been lost.
    auto presentEvent = FindOrCreatePresent(hdr);
    if (presentEvent == nullptr) {
        return;
    }

    while (presentEvent->QueueSubmitSequence != 0 || presentEvent->SeenDxgkPresent) {
        RemoveLostPresent(presentEvent);
        presentEvent = FindOrCreatePresent(hdr);
        if (presentEvent == nullptr) {
            return;
        }
    }

    TRACK_PRESENT_PATH_SAVE_GENERATED_ID(presentEvent);

    // For MPO, N events may be issued, but we only care about the first
    if (presentEvent->PresentMode != PresentMode::Unknown) {
        return;
    }

    presentEvent->MMIO = mmio;
    presentEvent->PresentMode = PresentMode::Hardware_Legacy_Flip;

    if (presentEvent->SyncInterval == -1) {
        presentEvent->SyncInterval = flipInterval;
    }
    if (!mmio) {
        presentEvent->SupportsTearing = flipInterval == 0;
    }

    // If this is the DWM thread, piggyback these pending presents on our fullscreen present
    if (hdr.ThreadId == DwmPresentThreadId) {
        for (auto iter = mPresentsWaitingForDWM.begin(); iter != mPresentsWaitingForDWM.end(); iter++) {
            iter->get()->PresentInDwmWaitingStruct = false;
        }
        std::swap(presentEvent->DependentPresents, mPresentsWaitingForDWM);
        DwmPresentThreadId = 0;
    }
}

static std::unordered_map<uint32_t, std::string> gNTProcessNames;

static void CreateDmaDurations(
    uint32_t processId,
    uint32_t* cloudStreamingProcessId,
    std::unordered_map<uint32_t, PMTraceConsumer::DmaDurations>* dmaDurations,
    PMTraceConsumer::Context* context)
{
    auto p = dmaDurations->emplace(processId, PMTraceConsumer::DmaDurations());
    context->mDmaDurations = &p.first->second;
    if (p.second) {
        context->mDmaDurations->mVideoEngines.mAccumulatedDmaTime = 0;
        context->mDmaDurations->mVideoEngines.mDmaExecStartTime = 0;
        context->mDmaDurations->mVideoEngines.mDmaExecCount = 0;
        context->mDmaDurations->mOtherEngines.mAccumulatedDmaTime = 0;
        context->mDmaDurations->mOtherEngines.mDmaExecStartTime = 0;
        context->mDmaDurations->mOtherEngines.mDmaExecCount = 0;

        if (*cloudStreamingProcessId == 0) {
            std::string processName;

            auto ii = gNTProcessNames.find(processId);
            if (ii != gNTProcessNames.end()) {
                processName = ii->second;
            } else {
                auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
                if (handle != nullptr) {
                    char path[MAX_PATH] = {};
                    DWORD numChars = sizeof(path);
                    if (QueryFullProcessImageNameA(handle, 0, path, &numChars)) {
                        for (; numChars > 0; --numChars) {
                            if (path[numChars - 1] == '/' ||
                                path[numChars - 1] == '\\') {
                                break;
                            }
                        }
                        processName = path + numChars;
                    }
                    CloseHandle(handle);
                }
            }

            if (_stricmp(processName.c_str(), "parsecd.exe") == 0 ||
                _stricmp(processName.c_str(), "intel-cloud-screen-capture.exe") == 0 ||
                _stricmp(processName.c_str(), "nvEncDXGIOutputDuplicationSample.exe") == 0) {
                *cloudStreamingProcessId = processId;
            }
        }
    }

    if (context->mNode->mIsVideoDecode && processId == *cloudStreamingProcessId) {
        context->mIsCloudStreamingVideoEncoder = true;
    }
}

void PMTraceConsumer::HandleDxgkQueueSubmit(
    EVENT_HEADER const& hdr,
    uint32_t packetType,
    uint32_t submitSequence,
    uint64_t context,
    bool present,
    bool supportsDxgkPresentEvent)
{
    // Create DmaDurations for this context to track DMA execution (for cases
    // where the context was created before the capture was started)
    //
    // mContexts should be empty if mTrackGPU==false.
    auto contextIter = mContexts.find(context);
    if (contextIter != mContexts.end() && contextIter->second.mDmaDurations == nullptr) {
        CreateDmaDurations(hdr.ProcessId, &mCloudStreamingProcessId, &mDmaDurations, &contextIter->second);
    }

    // If we know we're never going to get a DxgkPresent event for a given blt, then let's try to determine if it's a redirected blt or not.
    // If it's redirected, then the SubmitPresentHistory event should've been emitted before submitting anything else to the same context,
    // and therefore we'll know it's a redirected present by this point. If it's still non-redirected, then treat this as if it was a DxgkPresent
    // event - the present will be considered completed once its work is done, or if the work is already done, complete it now.
    if (!supportsDxgkPresentEvent) {
        bool completedPresent = false;
        auto eventIter = mBltsByDxgContext.find(context);
        if (eventIter != mBltsByDxgContext.end()) {
            TRACK_PRESENT_PATH(eventIter->second);
            if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer) {
                DebugModifyPresent(*eventIter->second);
                eventIter->second->SeenDxgkPresent = true;
                if (eventIter->second->ScreenTime != 0) {
                    CompletePresent(eventIter->second);
                    completedPresent = true;
                }
            }

            if (!completedPresent) {
                mBltsByDxgContext.erase(eventIter);
                // If the present event is completed, then this removal would have been done in CompletePresent.
            }
        }
    }

    // This event is emitted after a flip/blt/PHT event, and may be the only way
    // to trace completion of the present.
    if (packetType == DXGKETW_MMIOFLIP_COMMAND_BUFFER ||
        packetType == DXGKETW_SOFTWARE_COMMAND_BUFFER ||
        present) {
        auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
        if (eventIter == mPresentByThreadId.end() || eventIter->second->QueueSubmitSequence != 0) {
            return;
        }

        TRACK_PRESENT_PATH(eventIter->second);
        DebugModifyPresent(*eventIter->second);

        eventIter->second->QueueSubmitSequence = submitSequence;
        mPresentsBySubmitSequence.emplace(submitSequence, eventIter->second);

        if (eventIter->second->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer && !supportsDxgkPresentEvent) {
            mBltsByDxgContext[context] = eventIter->second;
            eventIter->second->DxgKrnlHContext = context;
        }
    }
}

// Assign any tracked accumulated GPU work to the present.
//
// If there are any DMA's executing across the present completion, assign their
// current duration to this present.
//
// mDmaDurations should be empty if mTrackGPU==false.
//
// TODO: there is an assumption here that no subsequent DMA packet can complete
// before we see this QueuePacket_Stop event.  If that happens, that DMA packet
// will mistakenly be assigned to this present.  I'm not sure if that is
// possible... but regardless, it is necessarily a very short duration so
// shouldn't be significant.
void PMTraceConsumer::AssignAccumulatedGPUWork(EVENT_HEADER const& hdr, PresentEvent* present)
{
    auto dmaIter = mDmaDurations.find(present->ProcessId);
    if (dmaIter != mDmaDurations.end()) {
        auto dmaDurations = &dmaIter->second;
        if (dmaDurations->mVideoEngines.mDmaExecCount > 0) {
            dmaDurations->mVideoEngines.mAccumulatedDmaTime += hdr.TimeStamp.QuadPart - dmaDurations->mVideoEngines.mDmaExecStartTime;
            dmaDurations->mVideoEngines.mDmaExecStartTime = hdr.TimeStamp.QuadPart;
        }
        if (dmaDurations->mOtherEngines.mDmaExecCount > 0) {
            dmaDurations->mOtherEngines.mAccumulatedDmaTime += hdr.TimeStamp.QuadPart - dmaDurations->mOtherEngines.mDmaExecStartTime;
            dmaDurations->mOtherEngines.mDmaExecStartTime = hdr.TimeStamp.QuadPart;
        }
        present->GPUDuration = dmaDurations->mOtherEngines.mAccumulatedDmaTime;
        present->GPUVideoDuration = dmaDurations->mVideoEngines.mAccumulatedDmaTime;
        dmaDurations->mOtherEngines.mAccumulatedDmaTime = 0;
        dmaDurations->mVideoEngines.mAccumulatedDmaTime = 0;
    }
}

void PMTraceConsumer::HandleDxgkQueueComplete(EVENT_HEADER const& hdr, uint32_t submitSequence)
{
    // Check if this is a present Packet being tracked, and if so get the
    // relevant PresentEvent.
    auto pEvent = FindBySubmitSequence(submitSequence);
    if (pEvent == nullptr) {
        return;
    }

    TRACK_PRESENT_PATH_SAVE_GENERATED_ID(pEvent);
    DebugModifyPresent(*pEvent);

    // Assign any tracked accumulated GPU work to the present.
    AssignAccumulatedGPUWork(hdr, pEvent.get());

    // Complete the present for present modes for which packet completion
    // implies display.
    if (pEvent->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer ||
        (pEvent->PresentMode == PresentMode::Hardware_Legacy_Flip && !pEvent->MMIO)) {
        pEvent->ReadyTime = hdr.TimeStamp.QuadPart;
        pEvent->ScreenTime = hdr.TimeStamp.QuadPart;
        pEvent->FinalState = PresentResult::Presented;

        // Sometimes, the queue packets associated with a present will complete
        // before the DxgKrnl PresentInfo event is fired.  For blit presents in
        // this case, we have no way to differentiate between fullscreen and
        // windowed blits, so we defer the completion of this present until
        // we've also seen the Dxgk Present_Info event.
        if (!pEvent->SeenDxgkPresent && pEvent->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer) {
            return;
        }

        CompletePresent(pEvent);
    }
}

// An MMIOFlip event is emitted when an MMIOFlip packet is dequeued.  All GPU
// work submitted prior to the flip has been completed.
//
// It also is emitted when an independent flip PHT is dequed, and will tell us
// whether the present is immediate or vsync.
void PMTraceConsumer::HandleDxgkMMIOFlip(EVENT_HEADER const& hdr, uint32_t flipSubmitSequence, uint32_t flags)
{
    auto pEvent = FindBySubmitSequence(flipSubmitSequence);
    if (pEvent == nullptr) {
        return;
    }

    TRACK_PRESENT_PATH_SAVE_GENERATED_ID(pEvent);

    pEvent->ReadyTime = hdr.TimeStamp.QuadPart;

    if (pEvent->PresentMode == PresentMode::Composed_Flip) {
        pEvent->PresentMode = PresentMode::Hardware_Independent_Flip;
    }

    if (flags & (uint32_t) Microsoft_Windows_DxgKrnl::MMIOFlip::Immediate) {
        pEvent->FinalState = PresentResult::Presented;
        pEvent->ScreenTime = hdr.TimeStamp.QuadPart;
        pEvent->SupportsTearing = true;
        if (pEvent->PresentMode == PresentMode::Hardware_Legacy_Flip) {
            CompletePresent(pEvent);
        }
    }
}

void PMTraceConsumer::HandleDxgkMMIOFlipMPO(EVENT_HEADER const& hdr, uint32_t flipSubmitSequence, uint32_t flipEntryStatusAfterFlip, bool flipEntryStatusAfterFlipValid)
{
    auto pEvent = FindBySubmitSequence(flipSubmitSequence);
    if (pEvent == nullptr) {
        return;
    }

    TRACK_PRESENT_PATH(pEvent);

    // Avoid double-marking a single present packet coming from the MPO API
    if (pEvent->ReadyTime == 0) {
        pEvent->ReadyTime = hdr.TimeStamp.QuadPart;
    }

    if (pEvent->PresentMode == PresentMode::Hardware_Independent_Flip ||
        pEvent->PresentMode == PresentMode::Composed_Flip) {
        pEvent->PresentMode = PresentMode::Hardware_Composed_Independent_Flip;
    }

    if (!flipEntryStatusAfterFlipValid) {
        return;
    }

    TRACK_PRESENT_PATH(pEvent);

    // Present could tear if we're not waiting for vsync
    if (flipEntryStatusAfterFlip != (uint32_t) Microsoft_Windows_DxgKrnl::FlipEntryStatus::FlipWaitVSync) {
        pEvent->SupportsTearing = true;
    }

    // For the VSync ahd HSync paths, we'll wait for the corresponding ?SyncDPC
    // event before being considering the present complete to get a more-accurate
    // ScreenTime (see HandleDxgkSyncDPC).
    if (flipEntryStatusAfterFlip == (uint32_t) Microsoft_Windows_DxgKrnl::FlipEntryStatus::FlipWaitVSync ||
        flipEntryStatusAfterFlip == (uint32_t) Microsoft_Windows_DxgKrnl::FlipEntryStatus::FlipWaitHSync) {
        return;
    }

    TRACK_PRESENT_PATH(pEvent);

    pEvent->FinalState = PresentResult::Presented;
    if (flipEntryStatusAfterFlip == (uint32_t) Microsoft_Windows_DxgKrnl::FlipEntryStatus::FlipWaitComplete) {
        pEvent->ScreenTime = hdr.TimeStamp.QuadPart;
    }
    if (pEvent->PresentMode == PresentMode::Hardware_Legacy_Flip) {
        CompletePresent(pEvent);
    }
}

void PMTraceConsumer::HandleDxgkSyncDPC(EVENT_HEADER const& hdr, uint32_t flipSubmitSequence)
{
    // The VSyncDPC/HSyncDPC contains a field telling us what flipped to screen.
    // This is the way to track completion of a fullscreen present.
    auto pEvent = FindBySubmitSequence(flipSubmitSequence);
    if (pEvent == nullptr) {
        return;
    }

    TRACK_PRESENT_PATH_SAVE_GENERATED_ID(pEvent);

    // Prevent double-complete when VSyncDPCMultiPlane and VSyncDPC are both sent for a plane.
    if (pEvent->FinalState != PresentResult::Presented) {
        pEvent->ScreenTime = hdr.TimeStamp.QuadPart;
        pEvent->FinalState = PresentResult::Presented;
        if (pEvent->PresentMode == PresentMode::Hardware_Legacy_Flip) {
            CompletePresent(pEvent);
        }
    }
}

void PMTraceConsumer::HandleDxgkPresentHistory(
    EVENT_HEADER const& hdr,
    uint64_t token,
    uint64_t tokenData,
    PresentMode knownPresentMode)
{
    // These events are emitted during submission of all types of windowed presents while DWM is on.
    // It gives us up to two different types of keys to correlate further.

    // Lookup the in-progress present.  It should not have a known TokenPtr
    // yet, so TokenPtr!=0 implies we looked up a 'stuck' present whose
    // tracking was lost for some reason.
    auto presentEvent = FindOrCreatePresent(hdr);
    if (presentEvent == nullptr) {
        return;
    }

    if (presentEvent->TokenPtr != 0) {
        RemoveLostPresent(presentEvent);
        presentEvent = FindOrCreatePresent(hdr);
        if (presentEvent == nullptr) {
            return;
        }

        assert(presentEvent->TokenPtr == 0);
    }

    TRACK_PRESENT_PATH_SAVE_GENERATED_ID(presentEvent);

    presentEvent->ReadyTime = 0;
    presentEvent->ScreenTime = 0;
    presentEvent->SupportsTearing = false;
    presentEvent->FinalState = PresentResult::Unknown;
    presentEvent->TokenPtr = token;

    assert(mDxgKrnlPresentHistoryTokens.find(token) == mDxgKrnlPresentHistoryTokens.end());
    mDxgKrnlPresentHistoryTokens[token] = presentEvent;

    if (presentEvent->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer) {
        presentEvent->PresentMode = PresentMode::Composed_Copy_GPU_GDI;
        assert(knownPresentMode == PresentMode::Unknown ||
               knownPresentMode == PresentMode::Composed_Copy_GPU_GDI);

    } else if (presentEvent->PresentMode == PresentMode::Unknown) {
        if (knownPresentMode == PresentMode::Composed_Composition_Atlas) {
            presentEvent->PresentMode = PresentMode::Composed_Composition_Atlas;
        } else {
            // When there's no Win32K events, we'll assume PHTs that aren't after a blt, and aren't composition tokens
            // are flip tokens and that they're displayed. There are no Win32K events on Win7, and they might not be
            // present in some traces - don't let presents get stuck/dropped just because we can't track them perfectly.
            assert(!presentEvent->SeenWin32KEvents);
            presentEvent->PresentMode = PresentMode::Composed_Flip;
        }
    } else if (presentEvent->PresentMode == PresentMode::Composed_Copy_CPU_GDI) {
        if (tokenData == 0) {
            // This is the best we can do, we won't be able to tell how many frames are actually displayed.
            mPresentsWaitingForDWM.emplace_back(presentEvent);
            presentEvent->PresentInDwmWaitingStruct = true;
        } else {
            assert(mPresentsByLegacyBlitToken.find(tokenData) == mPresentsByLegacyBlitToken.end());
            mPresentsByLegacyBlitToken[tokenData] = presentEvent;
            presentEvent->LegacyBlitTokenData = tokenData;
        }
    }

    // If we are not tracking further GPU/display-related events, complete the
    // present here.
    if (!mTrackDisplay) {
        CompletePresent(presentEvent);
    }
}

void PMTraceConsumer::HandleDxgkPresentHistoryInfo(EVENT_HEADER const& hdr, uint64_t token)
{
    // This event is emitted when a token is being handed off to DWM, and is a good way to indicate a ready state
    auto eventIter = mDxgKrnlPresentHistoryTokens.find(token);
    if (eventIter == mDxgKrnlPresentHistoryTokens.end()) {
        return;
    }

    DebugModifyPresent(*eventIter->second);
    TRACK_PRESENT_PATH_SAVE_GENERATED_ID(eventIter->second);

    eventIter->second->ReadyTime = eventIter->second->ReadyTime == 0
        ? hdr.TimeStamp.QuadPart
        : std::min(eventIter->second->ReadyTime, (uint64_t) hdr.TimeStamp.QuadPart);

    if (eventIter->second->PresentMode == PresentMode::Composed_Composition_Atlas ||
        (eventIter->second->PresentMode == PresentMode::Composed_Flip && !eventIter->second->SeenWin32KEvents)) {
        mPresentsWaitingForDWM.emplace_back(eventIter->second);
        eventIter->second->PresentInDwmWaitingStruct = true;
    }

    if (eventIter->second->PresentMode == PresentMode::Composed_Copy_GPU_GDI) {
        // Manipulate the map here
        // When DWM is ready to present, we'll query for the most recent blt targeting this window and take it out of the map

        // Ok to overwrite existing presents in this Hwnd.
        mLastWindowPresent[eventIter->second->Hwnd] = eventIter->second;
    }

    mDxgKrnlPresentHistoryTokens.erase(eventIter);
}

void PMTraceConsumer::HandleDXGKEvent(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_DxgKrnl::Flip_Info::Id:
    {
        EventDataDesc desc[] = {
            { L"FlipInterval" },
            { L"MMIOFlip" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto FlipInterval = desc[0].GetData<uint32_t>();
        auto MMIOFlip     = desc[1].GetData<BOOL>() != 0;

        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkFlip(hdr, FlipInterval, MMIOFlip);
        return;
    }
    case Microsoft_Windows_DxgKrnl::FlipMultiPlaneOverlay_Info::Id:
        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkFlip(hdr, -1, true);
        return;
    case Microsoft_Windows_DxgKrnl::QueuePacket_Start::Id:
    {
        EventDataDesc desc[] = {
            { L"PacketType" },
            { L"SubmitSequence" },
            { L"hContext" },
            { L"bPresent" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto PacketType     = desc[0].GetData<uint32_t>();
        auto SubmitSequence = desc[1].GetData<uint32_t>();
        auto hContext       = desc[2].GetData<uint64_t>();
        auto bPresent       = desc[3].GetData<BOOL>() != 0;

        HandleDxgkQueueSubmit(hdr, PacketType, SubmitSequence, hContext, bPresent, true);
        return;
    }
    case Microsoft_Windows_DxgKrnl::QueuePacket_Stop::Id:
    {
        EventDataDesc desc[] = {
            { L"SubmitSequence" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto SubmitSequence = desc[0].GetData<uint32_t>();

        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkQueueComplete(hdr, SubmitSequence);
        return;
    }
    case Microsoft_Windows_DxgKrnl::MMIOFlip_Info::Id:
    {
        EventDataDesc desc[] = {
            { L"FlipSubmitSequence" },
            { L"Flags" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto FlipSubmitSequence = desc[0].GetData<uint32_t>();
        auto Flags              = desc[1].GetData<uint32_t>();

        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkMMIOFlip(hdr, FlipSubmitSequence, Flags);
        return;
    }
    case Microsoft_Windows_DxgKrnl::MMIOFlipMultiPlaneOverlay_Info::Id:
    {
        auto flipEntryStatusAfterFlipValid = hdr.EventDescriptor.Version >= 2;
        EventDataDesc desc[] = {
            { L"FlipSubmitSequence" },
            { L"FlipEntryStatusAfterFlip" }, // optional
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc) - (flipEntryStatusAfterFlipValid ? 0 : 1));
        auto FlipFenceId              = desc[0].GetData<uint64_t>();
        auto FlipEntryStatusAfterFlip = flipEntryStatusAfterFlipValid ? desc[1].GetData<uint32_t>() : 0u;

        auto flipSubmitSequence = (uint32_t) (FlipFenceId >> 32u);

        HandleDxgkMMIOFlipMPO(hdr, flipSubmitSequence, FlipEntryStatusAfterFlip, flipEntryStatusAfterFlipValid);
        return;
    }
    case Microsoft_Windows_DxgKrnl::HSyncDPCMultiPlane_Info::Id:
    {
        // Used for Hardware Independent Flip, and Hardware Composed Flip to signal flipping to the screen 
        // on Windows 10 build numbers 17134 and up where the associated display is connected to 
        // integrated graphics
        // MMIOFlipMPO [EntryStatus:FlipWaitHSync] ->HSync DPC

        TRACK_PRESENT_PATH_GENERATE_ID();

        auto FlipCount = mMetadata.GetEventData<uint32_t>(pEventRecord, L"FlipEntryCount");
        for (uint32_t i = 0; i < FlipCount; i++) {
            // TODO: Combine these into single GetEventData() call?
            auto FlipId = mMetadata.GetEventData<uint64_t>(pEventRecord, L"FlipSubmitSequence", i);
            HandleDxgkSyncDPC(hdr, (uint32_t)(FlipId >> 32u));
        }
        return;
    }
    case Microsoft_Windows_DxgKrnl::VSyncDPCMultiPlane_Info::Id:
    {
        TRACK_PRESENT_PATH_GENERATE_ID();

        auto FlipCount = mMetadata.GetEventData<uint32_t>(pEventRecord, L"FlipEntryCount");
        for (uint32_t i = 0; i < FlipCount; i++) {
            // TODO: Combine these into single GetEventData() call?
            auto FlipId = mMetadata.GetEventData<uint64_t>(pEventRecord, L"FlipSubmitSequence", i);
            HandleDxgkSyncDPC(hdr, (uint32_t)(FlipId >> 32u));
        }
        break;
    }
    case Microsoft_Windows_DxgKrnl::VSyncDPC_Info::Id:
    {
        TRACK_PRESENT_PATH_GENERATE_ID();

        auto FlipFenceId = mMetadata.GetEventData<uint64_t>(pEventRecord, L"FlipFenceId");
        HandleDxgkSyncDPC(hdr, (uint32_t)(FlipFenceId >> 32u));
        return;
    }
    case Microsoft_Windows_DxgKrnl::Present_Info::Id:
    {
        // This event is emitted at the end of the kernel present, before returning.
        auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
        if (eventIter != mPresentByThreadId.end()) {
            auto present = eventIter->second;
            DebugModifyPresent(*present);
            TRACK_PRESENT_PATH(present);

            // Store the fact we've seen this present.  This is used to improve
            // tracking and to defer blt present completion until both Present_Info
            // and present QueuePacket_Stop have been seen.
            present->SeenDxgkPresent = true;

            if (present->Hwnd == 0) {
                present->Hwnd = mMetadata.GetEventData<uint64_t>(pEventRecord, L"hWindow");
            }

            // If we are not expecting an API present end event, then treat this as
            // the end of the present.  This can happen due to batched presents or
            // non-instrumented present APIs (i.e., not DXGI nor D3D9).
            if (present->ThreadId != hdr.ThreadId) {
                present->DriverBatchThreadId = hdr.ThreadId;
                if (present->TimeTaken == 0) {
                    present->TimeTaken = hdr.TimeStamp.QuadPart - present->QpcTime;
                }

                mPresentByThreadId.erase(eventIter);
            } else if (present->Runtime == Runtime::Other) {
                mPresentByThreadId.erase(eventIter);
            }

            // If this is a deferred blit that's already seen QueuePacket_Stop,
            // then complete it now.
            if (present->PresentMode == PresentMode::Hardware_Legacy_Copy_To_Front_Buffer &&
                present->ScreenTime != 0) {
                CompletePresent(present);
            }
        }

        // We use the first observed event to indicate that Dxgk provider is
        // running and able to successfully track/complete presents.
        //
        // There may be numerous presents that were previously started and
        // queued.  However, it's possible that they actually completed but we
        // never got their Dxgk events due to the trace startup process.  When
        // that happens, QpcTime/TimeTaken and ReadyTime/ScreenTime times can
        // become mis-matched, actually coming from different Present() calls.
        //
        // This is especially prevalent in ETLs that start runtime providers
        // before backend providers and/or start capturing while an intensive
        // graphics application is already running.
        //
        // We handle this by throwing away all queued presents up to this
        // point.
        if (mSeenDxgkPresentInfo == false) {
            mSeenDxgkPresentInfo = true;

            for (uint32_t i = 0; i < mAllPresentsNextIndex; ++i) {
                auto& p = mAllPresents[i];
                if (p != nullptr && !p->Completed && !p->IsLost) {
                    RemoveLostPresent(p);
                }
            }
        }
        return;
    }
    case Microsoft_Windows_DxgKrnl::PresentHistoryDetailed_Start::Id:
    case Microsoft_Windows_DxgKrnl::PresentHistory_Start::Id:
    {
        EventDataDesc desc[] = {
            { L"Token" },
            { L"Model" },
            { L"TokenData" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto Token     = desc[0].GetData<uint64_t>();
        auto Model     = desc[1].GetData<uint32_t>();
        auto TokenData = desc[2].GetData<uint64_t>();

        if (Model == D3DKMT_PM_REDIRECTED_GDI) {
            return;
        }

        auto presentMode = PresentMode::Unknown;
        switch (Model) {
        case D3DKMT_PM_REDIRECTED_BLT:         presentMode = PresentMode::Composed_Copy_GPU_GDI; break;
        case D3DKMT_PM_REDIRECTED_VISTABLT:    presentMode = PresentMode::Composed_Copy_CPU_GDI; break;
        case D3DKMT_PM_REDIRECTED_FLIP:        presentMode = PresentMode::Composed_Flip; break;
        case D3DKMT_PM_REDIRECTED_COMPOSITION: presentMode = PresentMode::Composed_Composition_Atlas; break;
        }

        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkPresentHistory(hdr, Token, TokenData, presentMode);
        return;
    }
    case Microsoft_Windows_DxgKrnl::PresentHistory_Info::Id:
        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkPresentHistoryInfo(hdr, mMetadata.GetEventData<uint64_t>(pEventRecord, L"Token"));
        return;
    case Microsoft_Windows_DxgKrnl::Blit_Info::Id:
    {
        EventDataDesc desc[] = {
            { L"hwnd" },
            { L"bRedirectedPresent" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto hwnd               = desc[0].GetData<uint64_t>();
        auto bRedirectedPresent = desc[1].GetData<uint32_t>() != 0;

        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkBlt(hdr, hwnd, bRedirectedPresent);
        return;
    }
    case Microsoft_Windows_DxgKrnl::Blit_Cancel::Id:
        HandleDxgkBltCancel(hdr);
        return;
    }

    if (mTrackGPU) {
        switch (hdr.EventDescriptor.Id) {

        // We need a mapping from hContext to GPU node.
        //
        // There's two ways I've tried to get this. One is to use
        // Microsoft_Windows_DxgKrnl::SelectContext2_Info events which include
        // all the required info (hContext, pDxgAdapter, and NodeOrdinal) but
        // that event fires often leading to significant overhead.
        //
        // The current implementaiton requires a CAPTURE_STATE on start up to
        // get all existing context/device events but after that the event
        // overhead should be minimal.
        case Microsoft_Windows_DxgKrnl::Device_DCStart::Id:
        case Microsoft_Windows_DxgKrnl::Device_Start::Id:
        {
            EventDataDesc desc[] = {
                { L"pDxgAdapter" },
                { L"hDevice" },
            };
            mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
            auto pDxgAdapter = desc[0].GetData<uint64_t>();
            auto hDevice     = desc[1].GetData<uint64_t>();

            // Sometimes there are duplicate start events
            assert(mDevices.find(hDevice) == mDevices.end() || mDevices.find(hDevice)->second == pDxgAdapter);
            mDevices.emplace(hDevice, pDxgAdapter);
            return;
        }
        case Microsoft_Windows_DxgKrnl::Device_Stop::Id:
        {
            auto hDevice = mMetadata.GetEventData<uint64_t>(pEventRecord, L"hDevice");

            // Sometimes there are duplicate stop events so it's ok if it's already removed
            mDevices.erase(hDevice);
            return;
        }
        case Microsoft_Windows_DxgKrnl::Context_DCStart::Id:
        case Microsoft_Windows_DxgKrnl::Context_Start::Id:
        {
            EventDataDesc desc[] = {
                { L"hContext" },
                { L"hDevice" },
                { L"NodeOrdinal" },
            };
            mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
            auto hContext    = desc[0].GetData<uint64_t>();
            auto hDevice     = desc[1].GetData<uint64_t>();
            auto NodeOrdinal = desc[2].GetData<uint32_t>();

            auto deviceIter = mDevices.find(hDevice);
            if (deviceIter == mDevices.end()) {
                assert(false);
                return;
            }
            auto pDxgAdapter = deviceIter->second;
            auto p = mNodes[pDxgAdapter].emplace(NodeOrdinal, Node());
            auto node = &p.first->second;
            if (p.second) {
                node->mQueueIndex = 0;
                node->mQueueCount = 0;
                node->mIsVideo = false;
                node->mIsVideoDecode = false;
            }

            // Sometimes there are duplicate start events, make sure that they say the same thing
            assert(mContexts.find(hDevice) == mContexts.end() || mContexts.find(hDevice)->second.mNode == node);

            auto context = &mContexts.emplace(hContext, PMTraceConsumer::Context()).first->second;
            context->mDmaDurations = nullptr;
            context->mNode = node;
            context->mIsCloudStreamingVideoEncoder = false;

            // Create DmaDurations unless this was a DCStart (in which case
            // it's generated by xperf)
            if (hdr.EventDescriptor.Id == Microsoft_Windows_DxgKrnl::Context_Start::Id) {
                CreateDmaDurations(hdr.ProcessId, &mCloudStreamingProcessId, &mDmaDurations, context);
            }
            return;
        }
        case Microsoft_Windows_DxgKrnl::Context_Stop::Id:
        {
            auto hContext = mMetadata.GetEventData<uint64_t>(pEventRecord, L"hContext");

            // Sometimes there are duplicate stop events so it's ok if it's already removed
            mContexts.erase(hContext);
            return;
        }

        case Microsoft_Windows_DxgKrnl::NodeMetadata_Info::Id:
        {
            EventDataDesc desc[] = {
                { L"pDxgAdapter" },
                { L"NodeOrdinal" },
                { L"EngineType" },
            };
            mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
            auto pDxgAdapter = desc[0].GetData<uint64_t>();
            auto NodeOrdinal = desc[1].GetData<uint32_t>();
            auto EngineType  = desc[2].GetData<uint32_t>();

            // Node should already be created (DxgKrnl::Context_Start comes
            // first) but just to be sure...
            auto p = mNodes[pDxgAdapter].emplace(NodeOrdinal, Node());
            auto node = &p.first->second;
            if (p.second) {
                node->mQueueIndex = 0;
                node->mQueueCount = 0;
                node->mIsVideo = false;
                node->mIsVideoDecode = false;
            }

            if (EngineType == DXGK_ENGINE_TYPE_VIDEO_DECODE ||
                EngineType == DXGK_ENGINE_TYPE_VIDEO_ENCODE ||
                EngineType == DXGK_ENGINE_TYPE_VIDEO_PROCESSING) {
                node->mIsVideo = true;
            }

            if (EngineType == DXGK_ENGINE_TYPE_VIDEO_DECODE) {
                node->mIsVideoDecode = true;
            }
            return;
        }

        // DmaPacket_Start occurs when a packet is enqueued onto a node.
        case Microsoft_Windows_DxgKrnl::DmaPacket_Start::Id:
        {
            EventDataDesc desc[] = {
                { L"hContext" },
                { L"ulQueueSubmitSequence" },
            };
            mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
            auto hContext   = desc[0].GetData<uint64_t>();
            auto SequenceId = desc[1].GetData<uint32_t>();

            // There are certain DMA packets that don't result in GPU work.
            // Examples are preemption packets or notifications for
            // VIDSCH_QUANTUM_EXPIRED.  These will have a sequence id of zero
            // (also DmaBuffer will be null).
            if (SequenceId == 0) {
                return;
            }

            // Lookup the context to figure out which node it's running on;
            // this can fail sometimes e.g. if parsing the beginning of an ETL
            // file where we can get packet events before the context mapping.
            auto ii = mContexts.find(hContext);
            if (ii != mContexts.end()) {
                auto context = &ii->second;
                auto dmaDurations = context->mDmaDurations;
                auto node = context->mNode;

                // A very rare (never observed) race exists where dmaDurations can
                // still be nullptr here.  The context must have been created
                // and this packet must have been submitted to the queue before
                // the capture started.
                //
                // In this case, we have to ignore the DMA packet otherwise the
                // node and dmaDurations tracking will become out of sync.
                if (dmaDurations == nullptr) {
                    return;
                }

                if (node->mQueueCount == _countof(Node::mSequenceId)) {
                    // mSequenceId array is too small (or, DmaPacket_Info
                    // events didn't fire for some reason).  This seems to always
                    // hit when an application closes... no sure why yet.
                    return;
                }

                // Enqueue the packet
                auto queueIndex = (node->mQueueIndex + node->mQueueCount) % _countof(Node::mSequenceId);
                node->mDmaDurations[queueIndex] = dmaDurations;
                node->mSequenceId[queueIndex] = SequenceId;
                node->mQueueCount += 1;

                // If the queue was empty, the packet starts running right
                // away, otherwise it is just enqueued and will start running
                // after all previous packets complete.
                if (node->mQueueCount == 1) {
                    node->mStartTime = hdr.TimeStamp.QuadPart;

                    if (node->mIsVideo) {
                        dmaDurations->mVideoEngines.mDmaExecCount += 1;
                        if (dmaDurations->mVideoEngines.mDmaExecCount == 1) {
                            dmaDurations->mVideoEngines.mDmaExecStartTime = hdr.TimeStamp.QuadPart;
                        }
                    } else {
                        dmaDurations->mOtherEngines.mDmaExecCount += 1;
                        if (dmaDurations->mOtherEngines.mDmaExecCount == 1) {
                            dmaDurations->mOtherEngines.mDmaExecStartTime = hdr.TimeStamp.QuadPart;
                        }
                    }
                }
            }
            return;
        }

        // DmaPacket_Info occurs on packet-related interrupts.  We could use
        // DmaPacket_Stop here, but the DMA_COMPLETED interrupt is a tighter
        // bound.
        case Microsoft_Windows_DxgKrnl::DmaPacket_Info_3::Id:
        {
            EventDataDesc desc[] = {
                { L"hContext" },
                { L"ulQueueSubmitSequence" },
            };
            mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
            auto hContext   = desc[0].GetData<uint64_t>();
            auto SequenceId = desc[1].GetData<uint32_t>();

            // There are certain DMA packets that don't result in GPU work.
            // Examples are preemption packets or notifications for
            // VIDSCH_QUANTUM_EXPIRED.  These will have a sequence id of zero
            // (also DmaBuffer will be null).
            if (SequenceId == 0) {
                return;
            }

            // Lookup the context to figure out which node it's running on;
            // this can fail sometimes e.g. if parsing the beginning of an ETL
            // file where we can get packet events before the context mapping.
            auto ii = mContexts.find(hContext);
            if (ii != mContexts.end()) {
                auto context = &ii->second;
                auto dmaDurations = context->mDmaDurations;
                auto node = context->mNode;

                // It's possible to miss DmaPacket events during realtime
                // analysis, so try to handle it gracefully here.
                //
                // If we get a DmaPacket_Info event for a packet that we didn't
                // get a DmaPacket_Start event for (or that we ignored because
                // we didn't know the process yet) then SequenceId will be
                // smaller than expected.  If this happens, we ignore the
                // DmaPacket_Info event which means that, if there was idle
                // time before the missing DmaPacket_Start event, mGPUTime will
                // be too large.
                //
                // measured: ----------------  -------     ---------------------
                //                                            [---   [--
                // actual:   [-----]  [-----]  [-----]     [-----]-----]-------]
                //           ^     ^  x     ^  ^     ^        x  ^   ^
                //           s1    i1 s2    i2 s3    i3       s2 i1  s3

                auto runningSequenceId = node->mSequenceId[node->mQueueIndex];
                if (dmaDurations == nullptr || node->mQueueCount == 0 || SequenceId < runningSequenceId) {
                    return;
                }

                // If we get a DmaPacket_Start event with no corresponding
                // DmaPacket_Info, then SequenceId will be larger than
                // expected.  If this happens, we seach through the queue for a
                // match and if no match was found then we ignore this event
                // (we missed both the DmaPacket_Start and DmaPacket_Info for
                // the packet).  In this case, both the missing packet's
                // execution time as well as any idle time afterwards will be
                // associated with the previous packet.
                //
                // If a match is found, then we don't know when the pre-match
                // packets ended (nor when the matched packet started).  We
                // treat this case as if the first packet with a missed
                // DmaPacket_Info ran the whole time, and all other packets up
                // to the match executed with zero time.  Any idle time during
                // this range is ignored, and the correct association of gpu
                // work to process will not be correct (unless all these
                // contexts come from the same process).
                //
                // measured: -------  ----------------     ---------------------
                //                                            [---   [--
                // actual:   [-----]  [-----]  [-----]     [-----]-----]-------]
                //           ^     ^  ^     x  ^     ^        ^  ^     x
                //           s1    i1 s2    i2 s3    i3       s2 i1    i2

                if (SequenceId > runningSequenceId) {
                    for (uint32_t missingCount = 1; ; ++missingCount) {
                        if (missingCount == node->mQueueCount) {
                            return;
                        }

                        uint32_t queueIndex = (node->mQueueIndex + missingCount) % _countof(Node::mSequenceId);
                        if (node->mSequenceId[queueIndex] == SequenceId) {
                            // Move current packet into this slot
                            node->mDmaDurations[queueIndex] = node->mDmaDurations[node->mQueueIndex];
                            node->mSequenceId[queueIndex] = node->mSequenceId[node->mQueueIndex];
                            node->mQueueIndex = queueIndex;
                            node->mQueueCount -= missingCount;

                            dmaDurations = node->mDmaDurations[node->mQueueIndex];
                            break;
                        }
                    }
                }

                // Point to the appropriate DmaDuration instance based on Node
                // type.
                assert(dmaDurations == node->mDmaDurations[node->mQueueIndex]);
                auto dmaDuration = node->mIsVideo ? &dmaDurations->mVideoEngines : &dmaDurations->mOtherEngines;

                // Pop the completed packet from the queue
                node->mQueueCount -= 1;
                dmaDuration->mDmaExecCount -= 1;

                // If this was the process' last executing packet, accumulate
                // the execution duration into the process' count.
                if (dmaDuration->mDmaExecCount == 0) {
                    dmaDuration->mAccumulatedDmaTime += hdr.TimeStamp.QuadPart - dmaDuration->mDmaExecStartTime;
                }

                // If there was another queued packet, start it
                if (node->mQueueCount > 0) {
                    node->mStartTime  = hdr.TimeStamp.QuadPart;
                    node->mQueueIndex = (node->mQueueIndex + 1) % _countof(Node::mSequenceId);

                    dmaDurations = node->mDmaDurations[node->mQueueIndex];
                    dmaDuration = node->mIsVideo ? &dmaDurations->mVideoEngines : &dmaDurations->mOtherEngines;

                    dmaDuration->mDmaExecCount += 1;
                    if (dmaDuration->mDmaExecCount == 1) {
                        dmaDuration->mDmaExecStartTime = hdr.TimeStamp.QuadPart;
                    }
                }

                // If this is the end of an identified video encode context
                // used for cloud streaming, treat it like a present
                if (context->mIsCloudStreamingVideoEncoder) {
                    auto videoPresent = std::make_shared<PresentEvent>(hdr, Runtime::CloudStreaming);
                    videoPresent->ProcessId = mCloudStreamingProcessId;
                    AssignAccumulatedGPUWork(hdr, videoPresent.get());
                    videoPresent->Completed = true;
                    mPresentEvents.push_back(videoPresent);
                }
            }
            return;
        }
        }
    }

    assert(!mFilteredEvents); // Assert that filtering is working if expected
}

namespace Win7 {

typedef LARGE_INTEGER PHYSICAL_ADDRESS;

#pragma pack(push)
#pragma pack(1)

typedef struct _DXGKETW_BLTEVENT {
    ULONGLONG                  hwnd;
    ULONGLONG                  pDmaBuffer;
    ULONGLONG                  PresentHistoryToken;
    ULONGLONG                  hSourceAllocation;
    ULONGLONG                  hDestAllocation;
    BOOL                       bSubmit;
    BOOL                       bRedirectedPresent;
    UINT                       Flags; // DXGKETW_PRESENTFLAGS
    RECT                       SourceRect;
    RECT                       DestRect;
    UINT                       SubRectCount; // followed by variable number of ETWGUID_DXGKBLTRECT events
} DXGKETW_BLTEVENT;

typedef struct _DXGKETW_FLIPEVENT {
    ULONGLONG                  pDmaBuffer;
    ULONG                      VidPnSourceId;
    ULONGLONG                  FlipToAllocation;
    UINT                       FlipInterval; // D3DDDI_FLIPINTERVAL_TYPE
    BOOLEAN                    FlipWithNoWait;
    BOOLEAN                    MMIOFlip;
} DXGKETW_FLIPEVENT;

typedef struct _DXGKETW_PRESENTHISTORYEVENT {
    ULONGLONG             hAdapter;
    ULONGLONG             Token;
    D3DKMT_PRESENT_MODEL  Model;     // available only for _STOP event type.
    UINT                  TokenSize; // available only for _STOP event type.
} DXGKETW_PRESENTHISTORYEVENT;

typedef struct _DXGKETW_QUEUESUBMITEVENT {
    ULONGLONG                  hContext;
    ULONG                      PacketType; // DXGKETW_QUEUE_PACKET_TYPE
    ULONG                      SubmitSequence;
    ULONGLONG                  DmaBufferSize;
    UINT                       AllocationListSize;
    UINT                       PatchLocationListSize;
    BOOL                       bPresent;
    ULONGLONG                  hDmaBuffer;
} DXGKETW_QUEUESUBMITEVENT;

typedef struct _DXGKETW_QUEUECOMPLETEEVENT {
    ULONGLONG                  hContext;
    ULONG                      PacketType;
    ULONG                      SubmitSequence;
    union {
        BOOL                   bPreempted;
        BOOL                   bTimeouted; // PacketType is WaitCommandBuffer.
    };
} DXGKETW_QUEUECOMPLETEEVENT;

typedef struct _DXGKETW_SCHEDULER_VSYNC_DPC {
    ULONGLONG                 pDxgAdapter;
    UINT                      VidPnTargetId;
    PHYSICAL_ADDRESS          ScannedPhysicalAddress;
    UINT                      VidPnSourceId;
    UINT                      FrameNumber;
    LONGLONG                  FrameQPCTime;
    ULONGLONG                 hFlipDevice;
    UINT                      FlipType; // DXGKETW_FLIPMODE_TYPE
    union
    {
        ULARGE_INTEGER        FlipFenceId;
        PHYSICAL_ADDRESS      FlipToAddress;
    };
} DXGKETW_SCHEDULER_VSYNC_DPC;

typedef struct _DXGKETW_SCHEDULER_MMIO_FLIP_32 {
    ULONGLONG        pDxgAdapter;
    UINT             VidPnSourceId;
    ULONG            FlipSubmitSequence; // ContextUserSubmissionId
    UINT             FlipToDriverAllocation;
    PHYSICAL_ADDRESS FlipToPhysicalAddress;
    UINT             FlipToSegmentId;
    UINT             FlipPresentId;
    UINT             FlipPhysicalAdapterMask;
    ULONG            Flags;
} DXGKETW_SCHEDULER_MMIO_FLIP_32;

typedef struct _DXGKETW_SCHEDULER_MMIO_FLIP_64 {
    ULONGLONG        pDxgAdapter;
    UINT             VidPnSourceId;
    ULONG            FlipSubmitSequence; // ContextUserSubmissionId
    ULONGLONG        FlipToDriverAllocation;
    PHYSICAL_ADDRESS FlipToPhysicalAddress;
    UINT             FlipToSegmentId;
    UINT             FlipPresentId;
    UINT             FlipPhysicalAdapterMask;
    ULONG            Flags;
} DXGKETW_SCHEDULER_MMIO_FLIP_64;

#pragma pack(pop)

} // namespace Win7

void PMTraceConsumer::HandleWin7DxgkBlt(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);
    TRACK_PRESENT_PATH_GENERATE_ID();

    auto pBltEvent = reinterpret_cast<Win7::DXGKETW_BLTEVENT*>(pEventRecord->UserData);
    HandleDxgkBlt(
        pEventRecord->EventHeader,
        pBltEvent->hwnd,
        pBltEvent->bRedirectedPresent != 0);
}

void PMTraceConsumer::HandleWin7DxgkFlip(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);
    TRACK_PRESENT_PATH_GENERATE_ID();

    auto pFlipEvent = reinterpret_cast<Win7::DXGKETW_FLIPEVENT*>(pEventRecord->UserData);
    HandleDxgkFlip(
        pEventRecord->EventHeader,
        pFlipEvent->FlipInterval,
        pFlipEvent->MMIOFlip != 0);
}

void PMTraceConsumer::HandleWin7DxgkPresentHistory(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);

    auto pPresentHistoryEvent = reinterpret_cast<Win7::DXGKETW_PRESENTHISTORYEVENT*>(pEventRecord->UserData);
    if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_START) {
        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkPresentHistory(
            pEventRecord->EventHeader,
            pPresentHistoryEvent->Token,
            0,
            PresentMode::Unknown);
    } else if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_INFO) {
        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkPresentHistoryInfo(pEventRecord->EventHeader, pPresentHistoryEvent->Token);
    }
}

void PMTraceConsumer::HandleWin7DxgkQueuePacket(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);

    if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_START) {
        auto pSubmitEvent = reinterpret_cast<Win7::DXGKETW_QUEUESUBMITEVENT*>(pEventRecord->UserData);
        HandleDxgkQueueSubmit(
            pEventRecord->EventHeader,
            pSubmitEvent->PacketType,
            pSubmitEvent->SubmitSequence,
            pSubmitEvent->hContext,
            pSubmitEvent->bPresent != 0,
            false);
    } else if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_STOP) {
        auto pCompleteEvent = reinterpret_cast<Win7::DXGKETW_QUEUECOMPLETEEVENT*>(pEventRecord->UserData);
        TRACK_PRESENT_PATH_GENERATE_ID();
        HandleDxgkQueueComplete(pEventRecord->EventHeader, pCompleteEvent->SubmitSequence);
    }
}

void PMTraceConsumer::HandleWin7DxgkVSyncDPC(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);
    TRACK_PRESENT_PATH_GENERATE_ID();

    auto pVSyncDPCEvent = reinterpret_cast<Win7::DXGKETW_SCHEDULER_VSYNC_DPC*>(pEventRecord->UserData);
    HandleDxgkSyncDPC(pEventRecord->EventHeader, (uint32_t)(pVSyncDPCEvent->FlipFenceId.QuadPart >> 32u));
}

void PMTraceConsumer::HandleWin7DxgkMMIOFlip(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);
    TRACK_PRESENT_PATH_GENERATE_ID();

    if (pEventRecord->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER)
    {
        auto pMMIOFlipEvent = reinterpret_cast<Win7::DXGKETW_SCHEDULER_MMIO_FLIP_32*>(pEventRecord->UserData);
        HandleDxgkMMIOFlip(
            pEventRecord->EventHeader,
            pMMIOFlipEvent->FlipSubmitSequence,
            pMMIOFlipEvent->Flags);
    }
    else
    {
        auto pMMIOFlipEvent = reinterpret_cast<Win7::DXGKETW_SCHEDULER_MMIO_FLIP_64*>(pEventRecord->UserData);
        HandleDxgkMMIOFlip(
            pEventRecord->EventHeader,
            pMMIOFlipEvent->FlipSubmitSequence,
            pMMIOFlipEvent->Flags);
    }
}

void PMTraceConsumer::HandleWin32kEvent(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_Win32k::TokenCompositionSurfaceObject_Info::Id:
    {
        EventDataDesc desc[] = {
            { L"CompositionSurfaceLuid" },
            { L"PresentCount" },
            { L"BindId" },
            { L"DestWidth" },  // version >= 1
            { L"DestHeight" }, // version >= 1
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc) - (hdr.EventDescriptor.Version == 0 ? 2 : 0));
        auto CompositionSurfaceLuid = desc[0].GetData<uint64_t>();
        auto PresentCount           = desc[1].GetData<uint64_t>();
        auto BindId                 = desc[2].GetData<uint64_t>();

        // Lookup the in-progress present.  It should not have seen any Win32K
        // events yet, so SeenWin32KEvents==true implies we looked up a 'stuck'
        // present whose tracking was lost for some reason.
        auto PresentEvent = FindOrCreatePresent(hdr);
        if (PresentEvent == nullptr) {
            return;
        }

        if (PresentEvent->SeenWin32KEvents) {
            RemoveLostPresent(PresentEvent);
            PresentEvent = FindOrCreatePresent(hdr);
            if (PresentEvent == nullptr) {
                return;
            }

            assert(!PresentEvent->SeenWin32KEvents);
        }

        TRACK_PRESENT_PATH(PresentEvent);

        PresentEvent->PresentMode = PresentMode::Composed_Flip;
        PresentEvent->SeenWin32KEvents = true;

        if (hdr.EventDescriptor.Version >= 1) {
            PresentEvent->DestWidth  = desc[3].GetData<uint32_t>();
            PresentEvent->DestHeight = desc[4].GetData<uint32_t>();
        }

        PMTraceConsumer::Win32KPresentHistoryTokenKey key(CompositionSurfaceLuid, PresentCount, BindId);
        assert(mWin32KPresentHistoryTokens.find(key) == mWin32KPresentHistoryTokens.end());
        mWin32KPresentHistoryTokens[key] = PresentEvent;
        PresentEvent->CompositionSurfaceLuid = CompositionSurfaceLuid;
        PresentEvent->Win32KPresentCount = PresentCount;
        PresentEvent->Win32KBindId = BindId;
        break;
    }
    case Microsoft_Windows_Win32k::TokenStateChanged_Info::Id:
    {
        EventDataDesc desc[] = {
            { L"CompositionSurfaceLuid" },
            { L"PresentCount" },
            { L"BindId" },
            { L"NewState" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto CompositionSurfaceLuid = desc[0].GetData<uint64_t>();
        auto PresentCount           = desc[1].GetData<uint32_t>();
        auto BindId                 = desc[2].GetData<uint64_t>();
        auto NewState               = desc[3].GetData<uint32_t>();

        PMTraceConsumer::Win32KPresentHistoryTokenKey key(CompositionSurfaceLuid, PresentCount, BindId);
        auto eventIter = mWin32KPresentHistoryTokens.find(key);
        if (eventIter == mWin32KPresentHistoryTokens.end()) {
            return;
        }

        auto &event = *eventIter->second;

        DebugModifyPresent(event);

        switch (NewState) {
        case (uint32_t) Microsoft_Windows_Win32k::TokenState::InFrame: // Composition is starting
        {
            TRACK_PRESENT_PATH(eventIter->second);

            // If we're compositing a newer present than the last known window
            // present, then the last known one was discarded.  We won't
            // necessarily see a transition to Discarded for it.
            if (event.Hwnd) {
                auto hWndIter = mLastWindowPresent.find(event.Hwnd);
                if (hWndIter == mLastWindowPresent.end()) {
                    mLastWindowPresent.emplace(event.Hwnd, eventIter->second);
                } else if (hWndIter->second != eventIter->second) {
                    DebugModifyPresent(*hWndIter->second);
                    hWndIter->second->FinalState = PresentResult::Discarded;
                    hWndIter->second = eventIter->second;
                    DebugModifyPresent(event);
                }
            }

            bool iFlip = mMetadata.GetEventData<BOOL>(pEventRecord, L"IndependentFlip") != 0;
            if (iFlip && event.PresentMode == PresentMode::Composed_Flip) {
                event.PresentMode = PresentMode::Hardware_Independent_Flip;
            }
            break;
        }

        case (uint32_t) Microsoft_Windows_Win32k::TokenState::Confirmed: // Present has been submitted
            TRACK_PRESENT_PATH(eventIter->second);

            // Handle DO_NOT_SEQUENCE presents, which may get marked as confirmed,
            // if a frame was composed when this token was completed
            if (event.FinalState == PresentResult::Unknown &&
                (event.PresentFlags & DXGI_PRESENT_DO_NOT_SEQUENCE) != 0) {
                event.FinalState = PresentResult::Discarded;
            }
            if (event.Hwnd) {
                mLastWindowPresent.erase(event.Hwnd);
            }
            break;

        case (uint32_t) Microsoft_Windows_Win32k::TokenState::Retired: // Present has been completed
            TRACK_PRESENT_PATH(eventIter->second);

            if (event.FinalState == PresentResult::Unknown) {
                event.ScreenTime = hdr.TimeStamp.QuadPart;
                event.FinalState = PresentResult::Presented;
            }
            break;

        case (uint32_t) Microsoft_Windows_Win32k::TokenState::Discarded: // Present has been discarded
        {
            TRACK_PRESENT_PATH(eventIter->second);

            auto sharedPtr = eventIter->second;
            mWin32KPresentHistoryTokens.erase(eventIter);

            if (event.FinalState == PresentResult::Unknown || event.ScreenTime == 0) {
                event.FinalState = PresentResult::Discarded;
            }

            CompletePresent(sharedPtr);
            break;
        }
        }
        break;
    }
    default:
        assert(!mFilteredEvents); // Assert that filtering is working if expected
        break;
    }
}

void PMTraceConsumer::HandleDWMEvent(EVENT_RECORD* pEventRecord)
{
    DebugEvent(pEventRecord, &mMetadata);

    auto const& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) {
    case Microsoft_Windows_Dwm_Core::MILEVENT_MEDIA_UCE_PROCESSPRESENTHISTORY_GetPresentHistory_Info::Id:
        for (auto& hWndPair : mLastWindowPresent) {
            auto& present = hWndPair.second;
            // Pickup the most recent present from a given window
            if (present->PresentMode != PresentMode::Composed_Copy_GPU_GDI &&
                present->PresentMode != PresentMode::Composed_Copy_CPU_GDI) {
                continue;
            }
            TRACK_PRESENT_PATH(present);
            DebugModifyPresent(*present);
            present->DwmNotified = true;
            mPresentsWaitingForDWM.emplace_back(present);
            present->PresentInDwmWaitingStruct = true;
        }
        mLastWindowPresent.clear();
        break;

    case Microsoft_Windows_Dwm_Core::SCHEDULE_PRESENT_Start::Id:
        DwmProcessId = hdr.ProcessId;
        DwmPresentThreadId = hdr.ThreadId;
        break;

    case Microsoft_Windows_Dwm_Core::FlipChain_Pending::Id:
    case Microsoft_Windows_Dwm_Core::FlipChain_Complete::Id:
    case Microsoft_Windows_Dwm_Core::FlipChain_Dirty::Id:
    {
        if (InlineIsEqualGUID(hdr.ProviderId, Microsoft_Windows_Dwm_Core::Win7::GUID)) {
            return;
        }

        EventDataDesc desc[] = {
            { L"ulFlipChain" },
            { L"ulSerialNumber" },
            { L"hwnd" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto ulFlipChain    = desc[0].GetData<uint32_t>();
        auto ulSerialNumber = desc[1].GetData<uint32_t>();
        auto hwnd           = desc[2].GetData<uint64_t>();

        // The 64-bit token data from the PHT submission is actually two 32-bit
        // data chunks, corresponding to a "flip chain" id and present id
        auto token = ((uint64_t) ulFlipChain << 32ull) | ulSerialNumber;
        auto flipIter = mPresentsByLegacyBlitToken.find(token);
        if (flipIter == mPresentsByLegacyBlitToken.end()) {
            return;
        }

        TRACK_PRESENT_PATH(flipIter->second);
        DebugModifyPresent(*flipIter->second);

        // Watch for multiple legacy blits completing against the same window		
        mLastWindowPresent[hwnd] = flipIter->second;
        flipIter->second->DwmNotified = true;
        mPresentsByLegacyBlitToken.erase(flipIter);
        break;
    }
    case Microsoft_Windows_Dwm_Core::SCHEDULE_SURFACEUPDATE_Info::Id:
    {
        EventDataDesc desc[] = {
            { L"luidSurface" },
            { L"PresentCount" },
            { L"bindId" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));
        auto luidSurface  = desc[0].GetData<uint64_t>();
        auto PresentCount = desc[1].GetData<uint64_t>();
        auto bindId       = desc[2].GetData<uint64_t>();

        PMTraceConsumer::Win32KPresentHistoryTokenKey key(luidSurface, PresentCount, bindId);
        auto eventIter = mWin32KPresentHistoryTokens.find(key);
        if (eventIter != mWin32KPresentHistoryTokens.end()) {
            TRACK_PRESENT_PATH(eventIter->second);
            DebugModifyPresent(*eventIter->second);
            eventIter->second->DwmNotified = true;
        }
        break;
    }
    default:
        assert(!mFilteredEvents || // Assert that filtering is working if expected
               hdr.ProviderId == Microsoft_Windows_Dwm_Core::Win7::GUID);
        break;
    }
}

void PMTraceConsumer::RemovePresentFromTemporaryTrackingCollections(std::shared_ptr<PresentEvent> p)
{
    // Remove the present from any struct that would only host the event temporarily.
    // Currently defined as all structures except for mPresentsByProcess, 
    // mPresentsByProcessAndSwapChain, and mAllPresents.

    // mPresentByThreadId
    auto threadEventIter = mPresentByThreadId.find(p->ThreadId);
    if (threadEventIter != mPresentByThreadId.end() && threadEventIter->second == p) {
        mPresentByThreadId.erase(threadEventIter);
    }

    if (p->DriverBatchThreadId != 0) {
        auto batchThreadEventIter = mPresentByThreadId.find(p->DriverBatchThreadId);
        if (batchThreadEventIter != mPresentByThreadId.end() && batchThreadEventIter->second == p) {
            mPresentByThreadId.erase(batchThreadEventIter);
        }
    }

    // mPresentsBySubmitSequence
    if (p->QueueSubmitSequence != 0) {
        auto eventIter = mPresentsBySubmitSequence.find(p->QueueSubmitSequence);
        if (eventIter != mPresentsBySubmitSequence.end() && (eventIter->second == p)) {
            mPresentsBySubmitSequence.erase(eventIter);
        }
    }

    // mWin32KPresentHistoryTokens
    if (p->CompositionSurfaceLuid != 0) {
        PMTraceConsumer::Win32KPresentHistoryTokenKey key(
            p->CompositionSurfaceLuid,
            p->Win32KPresentCount,
            p->Win32KBindId
        );

        auto eventIter = mWin32KPresentHistoryTokens.find(key);
        if (eventIter != mWin32KPresentHistoryTokens.end() && (eventIter->second == p)) {
            mWin32KPresentHistoryTokens.erase(eventIter);
        }
    }

    // mDxgKrnlPresentHistoryTokens
    if (p->TokenPtr != 0) {
        auto eventIter = mDxgKrnlPresentHistoryTokens.find(p->TokenPtr);
        if (eventIter != mDxgKrnlPresentHistoryTokens.end() && eventIter->second == p) {
            mDxgKrnlPresentHistoryTokens.erase(eventIter);
        }
    }

    // mBltsByDxgContext
    if (p->DxgKrnlHContext != 0) {
        auto eventIter = mBltsByDxgContext.find(p->DxgKrnlHContext);
        if (eventIter != mBltsByDxgContext.end() && eventIter->second == p) {
            mBltsByDxgContext.erase(eventIter);
        }
    }

    // mLastWindowPresent
    // 0 is a invalid hwnd
    if (p->Hwnd != 0) {
        auto eventIter = mLastWindowPresent.find(p->Hwnd);
        if (eventIter != mLastWindowPresent.end() && eventIter->second == p) {
            mLastWindowPresent.erase(eventIter);
        }
    }

    // mPresentsWaitingForDWM
    if (p->PresentInDwmWaitingStruct) {
        for (auto presentIter = mPresentsWaitingForDWM.begin(); presentIter != mPresentsWaitingForDWM.end(); presentIter++) {
            // This loop should in theory be short because the present is old.
            // If we are in this loop for dozens of times, something is likely wrong.
            if (p == *presentIter) {
                mPresentsWaitingForDWM.erase(presentIter);
                p->PresentInDwmWaitingStruct = false;
                break;
            }
        }
    }

    // mPresentsByLegacyBlitToken
    // LegacyTokenData cannot be 0 if it's in mPresentsByLegacyBlitToken list.
    if (p->LegacyBlitTokenData != 0) {
        auto eventIter = mPresentsByLegacyBlitToken.find(p->LegacyBlitTokenData);
        if (eventIter != mPresentsByLegacyBlitToken.end() && eventIter->second == p) {
            mPresentsByLegacyBlitToken.erase(eventIter);
        }
    }
}

void PMTraceConsumer::RemoveLostPresent(std::shared_ptr<PresentEvent> p)
{
    // This present has been timed out. Remove all references to it from all tracking structures.
    // mPresentsByProcessAndSwapChain and mPresentsByProcess should always track the present's lifetime,
    // so these also have an assert to validate this assumption.

    DebugLostPresent(*p);

    p->IsLost = true;

    // Presents dependent on this event can no longer be trakced.
    for (auto& dependentPresent : p->DependentPresents) {
        if (!dependentPresent->IsLost) {
            RemoveLostPresent(dependentPresent);
        }
        // The only place a lost present could still exist outside of mLostPresentEvents is the dependents list.
        // A lost present has already been added to mLostPresentEvents, we should never modify it.
    }
    p->DependentPresents.clear();

    // Completed Presented presents should not make it here.
    assert(!(p->Completed && p->FinalState == PresentResult::Presented));

    // Remove the present from any struct that would only host the event temporarily.
    // Should we loop through and remove the dependent presents?
    RemovePresentFromTemporaryTrackingCollections(p);

    // mPresentsByProcess
    auto& presentsByThisProcess = mPresentsByProcess[p->ProcessId];
    presentsByThisProcess.erase(p->QpcTime);

    // mPresentsByProcessAndSwapChain
    auto& presentDeque = mPresentsByProcessAndSwapChain[std::make_tuple(p->ProcessId, p->SwapChainAddress)];

    bool hasRemovedElement = false;
    for (auto presentIter = presentDeque.begin(); presentIter != presentDeque.end(); presentIter++) {
        // This loop should in theory be short because the present is old.
        // If we are in this loop for dozens of times, something is likely wrong.
        if (p == *presentIter) {
            hasRemovedElement = true;
            presentDeque.erase(presentIter);
            break;
        }
    }
    // We expect an element to be removed here.
    assert(hasRemovedElement);

    // Update the list of lost presents.
    {
        std::lock_guard<std::mutex> lock(mLostPresentEventMutex);
        mLostPresentEvents.push_back(mAllPresents[p->mAllPresentsTrackingIndex]);
    }

    // mAllPresents
    mAllPresents[p->mAllPresentsTrackingIndex] = nullptr;
}

void PMTraceConsumer::CompletePresent(std::shared_ptr<PresentEvent> p)
{
    if (p->Completed && p->FinalState != PresentResult::Presented) {
        DebugModifyPresent(*p);
        p->FinalState = PresentResult::Error;
    }

    // Throw away events until we've seen at least one Dxgk PresentInfo event
    // (unless we're not tracking display in which case provider start order
    // is not an issue)
    if (mTrackDisplay && !mSeenDxgkPresentInfo) {
        RemoveLostPresent(p);
        return;
    }

    // Complete all other presents that were riding along with this one (i.e. this one came from DWM)
    for (auto& p2 : p->DependentPresents) {
        if (!p2->IsLost) {
            DebugModifyPresent(*p2);
            p2->ScreenTime = p->ScreenTime;
            p2->FinalState = p->FinalState;
            CompletePresent(p2);
        }
        // The only place a lost present could still exist outside of mLostPresentEvents is the dependents list.
        // A lost present has already been added to mLostPresentEvents, we should never modify it.
    }
    p->DependentPresents.clear();

    // Remove it from any tracking maps that it may have been inserted into
    RemovePresentFromTemporaryTrackingCollections(p);

    // TODO: Only way to CompletePresent() a present without
    // FindOrCreatePresent() finding it first is the while loop below, in which
    // case we should remove it there instead.  Or, when created by
    // FindOrCreatePresent() (which itself is a separate TODO).
    auto& presentsByThisProcess = mPresentsByProcess[p->ProcessId];
    presentsByThisProcess.erase(p->QpcTime);

    auto& presentDeque = mPresentsByProcessAndSwapChain[std::make_tuple(p->ProcessId, p->SwapChainAddress)];

    // If presented, remove all previous presents up till this one.
    if (p->FinalState == PresentResult::Presented) {
        auto presentIter = presentDeque.begin();
        while (presentIter != presentDeque.end() && *presentIter != p) {
            CompletePresent(*presentIter);
            presentIter = presentDeque.begin();
        }
    }

    DebugModifyPresent(*p);
    p->Completed = true;

    // Move presents to ready list.
    {
        std::lock_guard<std::mutex> lock(mPresentEventMutex);
        while (!presentDeque.empty() && presentDeque.front()->Completed) {
            mAllPresents[presentDeque.front()->mAllPresentsTrackingIndex] = nullptr;

            mPresentEvents.push_back(presentDeque.front());
            presentDeque.pop_front();
        }
    }
}

std::shared_ptr<PresentEvent> PMTraceConsumer::FindBySubmitSequence(uint32_t submitSequence)
{
    auto eventIter = mPresentsBySubmitSequence.find(submitSequence);
    if (eventIter == mPresentsBySubmitSequence.end()) {
        return nullptr;
    }
    DebugModifyPresent(*eventIter->second);
    return eventIter->second;
}

std::shared_ptr<PresentEvent> PMTraceConsumer::FindOrCreatePresent(EVENT_HEADER const& hdr)
{
    // Check if there is an in-progress present that this thread is already
    // working on and, if so, continue working on that.
    auto threadEventIter = mPresentByThreadId.find(hdr.ThreadId);
    if (threadEventIter != mPresentByThreadId.end()) {
        return threadEventIter->second;
    }

    // If not, check if this event is from a process that is filtered out and,
    // if so, ignore it.
    if (!IsProcessTrackedForFiltering(hdr.ProcessId)) {
        return nullptr;
    }

    // Search for an in-progress present created by this process that still
    // doesn't have a known PresentMode.
    //
    // This can be the case for DXGI/D3D presents created on a different
    // thread, which are batched and then handled later during a DXGK/Win32K
    // event. We want the oldest such present, based on the assumption that
    // batched presents are popped off the front of the driver queue by process
    // in order.
    auto& presentsByThisProcess = mPresentsByProcess[hdr.ProcessId];
    auto processIter = std::find_if(presentsByThisProcess.begin(), presentsByThisProcess.end(), [](auto processIter) {
        return processIter.second->PresentMode == PresentMode::Unknown;
    });
    if (processIter != presentsByThisProcess.end()) {
        auto presentEvent = processIter->second;

        // TODO: Do we need to move it to mPresentByThreadId anymore?
        presentsByThisProcess.erase(processIter);
        mPresentByThreadId.emplace(hdr.ThreadId, presentEvent);

        return presentEvent;
    }

    // Because we couldn't find a present above, the calling event is for an
    // unknown, in-progress present.  This can happen if the present didn't
    // originate from a runtime whose events we're tracking (i.e., DXGI or
    // D3D9) in which case a DXGKRNL event will be the first present-related
    // event we ever see.  So, we create the PresentEvent and start tracking it
    // from here.
    //
    // TODO: Why do we add it to presentsByThisProcess?  We're already past the
    // stage where we need to look it up by that mechanism...
    // mPresentByThreadId should be good enough at this point right?
    auto presentEvent = std::make_shared<PresentEvent>(hdr, Runtime::Other);
    TrackPresent(presentEvent, presentsByThisProcess);
    return presentEvent;
}

void PMTraceConsumer::TrackPresent(
    std::shared_ptr<PresentEvent> present,
    OrderedPresents& presentsByThisProcess)
{
    DebugCreatePresent(*present);

    // If there is an existing present that hasn't completed by the time the
    // circular buffer has come around, consider it lost.
    if (mAllPresents[mAllPresentsNextIndex] != nullptr) {
        RemoveLostPresent(mAllPresents[mAllPresentsNextIndex]);
    }

    present->mAllPresentsTrackingIndex = mAllPresentsNextIndex;
    mAllPresents[mAllPresentsNextIndex] = present;
    mAllPresentsNextIndex = (mAllPresentsNextIndex + 1) % PRESENTEVENT_CIRCULAR_BUFFER_SIZE;

    presentsByThisProcess.emplace(present->QpcTime, present);
    mPresentsByProcessAndSwapChain[std::make_tuple(present->ProcessId, present->SwapChainAddress)].emplace_back(present);
    mPresentByThreadId.emplace(present->ThreadId, present);
}

void PMTraceConsumer::TrackPresentOnThread(std::shared_ptr<PresentEvent> present)
{
    // If there is an in-flight present on this thread already, then something
    // has gone wrong with it's tracking so consider it lost.
    auto iter = mPresentByThreadId.find(present->ThreadId);
    if (iter != mPresentByThreadId.end()) {
        RemoveLostPresent(iter->second);
    }

    TrackPresent(present, mPresentsByProcess[present->ProcessId]);
}

// No TRACK_PRESENT instrumentation here because each runtime Present::Start
// event is instrumented and we assume we'll see the corresponding Stop event
// for any completed present.
void PMTraceConsumer::RuntimePresentStop(EVENT_HEADER const& hdr, bool AllowPresentBatching, Runtime runtime)
{
    auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
    if (eventIter == mPresentByThreadId.end()) {
        return;
    }
    auto &event = *eventIter->second;

    DebugModifyPresent(event);

    // eventIter should be equal to the PresentEvent created by the
    // corresponding ???::Present_Start event with event.Runtime==runtime.
    // However, sometimes this is not the case due to the corresponding Start
    // event happened before capture started, or missed events.
    assert(event.Runtime == Runtime::Other || event.Runtime == runtime);
    assert(event.QpcTime <= *(uint64_t*) &hdr.TimeStamp);
    event.Runtime   = runtime;
    event.TimeTaken = *(uint64_t*) &hdr.TimeStamp - event.QpcTime;

    if (!AllowPresentBatching || !mTrackDisplay) {
        event.FinalState = AllowPresentBatching ? PresentResult::Presented : PresentResult::Discarded;
        CompletePresent(eventIter->second);
        // CompletePresent removes the entry in mPresentByThreadId.
    } else {
        // We now remove this present from mPresentByThreadId because any future
        // event related to it (e.g., from DXGK/Win32K/etc.) is not expected to
        // come from this thread.
        mPresentByThreadId.erase(eventIter);
    }
}

void PMTraceConsumer::HandleNTProcessEvent(EVENT_RECORD* pEventRecord)
{
    if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_START ||
        pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_DC_START ||
        pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_END||
        pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_DC_END) {
        EventDataDesc desc[] = {
            { L"ProcessId" },
            { L"ImageFileName" },
        };
        mMetadata.GetEventData(pEventRecord, desc, _countof(desc));

        ProcessEvent event;
        event.QpcTime       = pEventRecord->EventHeader.TimeStamp.QuadPart;
        event.ProcessId     = desc[0].GetData<uint32_t>();
        event.ImageFileName = desc[1].GetData<std::string>();
        event.IsStartEvent  = pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_START ||
                              pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_DC_START;

        if (event.IsStartEvent) {
            gNTProcessNames[event.ProcessId] = event.ImageFileName;
        } else {
            gNTProcessNames.erase(event.ProcessId);
        }

        std::lock_guard<std::mutex> lock(mProcessEventMutex);
        mProcessEvents.emplace_back(event);
        return;
    }
}

void PMTraceConsumer::HandleMetadataEvent(EVENT_RECORD* pEventRecord)
{
    mMetadata.AddMetadata(pEventRecord);
}

void PMTraceConsumer::AddTrackedProcessForFiltering(uint32_t processID)
{
    std::unique_lock<std::shared_mutex> lock(mTrackedProcessFilterMutex);
    mTrackedProcessFilter.insert(processID);
}

void PMTraceConsumer::RemoveTrackedProcessForFiltering(uint32_t processID)
{
    std::unique_lock<std::shared_mutex> lock(mTrackedProcessFilterMutex);
    auto iterator = mTrackedProcessFilter.find(processID);
    
    if (iterator != mTrackedProcessFilter.end()) {
        mTrackedProcessFilter.erase(processID);
    }
    else {
        assert(false);
    }

    // Completion events will remove any currently tracked events for this process
    // from data structures, so we don't need to proactively remove them now.
}

bool PMTraceConsumer::IsProcessTrackedForFiltering(uint32_t processID)
{
    if (!mFilteredProcessIds || processID == DwmProcessId) {
        return true;
    }

    std::shared_lock<std::shared_mutex> lock(mTrackedProcessFilterMutex);
    auto iterator = mTrackedProcessFilter.find(processID);
    return (iterator != mTrackedProcessFilter.end());
}

#ifdef TRACK_PRESENT_PATHS
static_assert(__COUNTER__ <= 64, "Too many TRACK_PRESENT ids to store in PresentEvent::AnalysisPath");
#endif
