#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordLoop.hpp"
#include "prx/libSceAgcDriver/Unified/include/Releases.hpp"
#include <memory>
#include <mutex>

namespace AgcDriver::DriverDetail {

void Driver::handFlipToRecode(const Submission& submission, std::size_t cursor, bool holds, bool working) {
    auto flip = submission.flips.at(cursor);
    const auto handle = submission.flipHandles.at(cursor);
    const auto queueId = submission.queue;
    const auto serial = submission.serial;
    auto work = submission.flipWork;
    Unified::Loop::HandFlip(queueId, serial, static_cast<std::uint32_t>(cursor), [this, flip, handle, queueId, serial, cursor, work, holds, working] {
        if (holds) releaseFlipHold();
        GuestMemory::NoteFrame();
        ++flipsCounted;
        auto frame = std::make_shared<FrameTiming>(++frameSerial);
        const auto now = FrameTiming::Clock::now();
        frame->IncludeSubmission(serial, now, now, now, true);
        frame->SetFlip(serial, cursor, now, now);
        const auto workSerial = submitFlipWork(queueId);
        if (working) finishFlipWork(work, workSerial);
        beginFlipReady(handle);
        bool queued = false;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            const auto localDevice = device.Load();
            queued = localDevice != nullptr && localDevice->AfterRecordedWork([this, flip, frame, handle] {
                Unified::Record::AfterLandings([this, flip, frame, handle] {
                    Unified::Releases::Queue([this, flip, frame, handle] {
                        flip->GpuReady(frame);
                        endFlipReady(handle);
                    });
                });
            }, false);
        }
        if (queued) return;
        Unified::Releases::NoteDirect();
        flip->GpuReady(frame);
        endFlipReady(handle);
    });
}

}
