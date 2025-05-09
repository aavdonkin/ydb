#pragma once

#include "backend.h"

#include <util/datetime/base.h>
#include <util/generic/fwd.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>

class TSyncPageCacheFileLogBackend final: public TLogBackend {
public:
    TSyncPageCacheFileLogBackend(
        const TString& path,
        size_t maxBufferSize,
        size_t maxPendingCacheSize,
        TMaybe<TDuration> bufferFlushPeriod = Nothing()
    );
    ~TSyncPageCacheFileLogBackend();

    void WriteData(const TLogRecord& rec) override;
    void ReopenLog() override;

private:
    class TImpl;
    THolder<TImpl> Impl_;
};
