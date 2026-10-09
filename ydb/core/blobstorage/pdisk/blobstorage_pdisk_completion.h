#pragma once

#include <ydb/library/pdisk_io/aio.h>

#include <util/system/hp_timer.h>
#include <util/generic/string.h>
#include <library/cpp/lwtrace/shuttle.h>
#include <ydb/library/actors/wilson/wilson_span.h>

#include <optional>

namespace NKikimr::NPDisk {

struct TCompletionAction {
    ui64 OperationIdx;
    NHPTimer::STime SubmitTime;
    NHPTimer::STime GetTime;
    NHPTimer::STime CompletionEnqueueTime = 0;
    ui32 CompletionQueueDepthAtEnqueue = 0;
    NHPTimer::STime SubmitQueueTime = 0;
    NHPTimer::STime DeviceSlotWaitStartTime = 0;
    TCompletionAction *FlushAction = nullptr;
    ui64 CostNs = 0;
    NWilson::TTraceId TraceId;
    EIoResult Result = EIoResult::Unknown;
    TString ErrorReason;
    // Only reads should be executed in a separate thread since their completions consist of
    // time-consuming deciphering of read data. But currently some completion actions can write
    // to BlockDevice from Exec() and it's more safe to use WhiteList to allow only
    // LogWrite and ChunkWrite to be executed from GetThread
    bool ShouldBeExecutedInCompletionThread = true;
    bool CanBeExecutedInAdditionalCompletionThread = false;

    mutable NLWTrace::TOrbit Orbit;
protected:
    TVector<ui64> BadOffsets;

public:
    void SetResult(const EIoResult result) {
        Result = result;
        if (FlushAction) {
            FlushAction->SetResult(result);
        }
    }

    void SetErrorReason(const TString& errorReason) {
        ErrorReason = errorReason;
        if (FlushAction) {
            FlushAction->SetErrorReason(errorReason);
        }
    }

    void RegisterBadOffset(ui64 offset) {
        BadOffsets.push_back(offset);
    }

    virtual bool CanHandleResult() const {
        return Result == EIoResult::Ok;
    }

    virtual std::optional<ui8> GetSchedulerGate() const {
        return std::nullopt;
    }

    virtual void Exec(TActorSystem *actorSystem) = 0;
    virtual void Release(TActorSystem *) = 0;
    virtual ~TCompletionAction() {}
};

}
