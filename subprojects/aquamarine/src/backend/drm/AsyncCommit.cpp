#include "AsyncCommit.hpp"

#include <cerrno>
#include <thread>

using namespace Aquamarine;
using namespace Hyprutils::Memory;

void Aquamarine::CDRMCursorPositionMailbox::store(const Hyprutils::Math::Vector2D& position) {
    m_sequence.fetch_add(1, std::memory_order_acq_rel);
    m_x.store(sc<int64_t>(position.x), std::memory_order_relaxed);
    m_y.store(sc<int64_t>(position.y), std::memory_order_relaxed);
    m_sequence.fetch_add(1, std::memory_order_release);
}

Hyprutils::Math::Vector2D Aquamarine::CDRMCursorPositionMailbox::load() const {
    while (true) {
        const auto BEFORE = m_sequence.load(std::memory_order_acquire);
        if (BEFORE & 1) {
            std::this_thread::yield();
            continue;
        }

        const auto X     = m_x.load(std::memory_order_relaxed);
        const auto Y     = m_y.load(std::memory_order_relaxed);
        const auto AFTER = m_sequence.load(std::memory_order_acquire);
        if (BEFORE == AFTER)
            return {sc<double>(X), sc<double>(Y)};
    }
}

Aquamarine::CDRMAsyncCommitData::CDRMAsyncCommitData(COutputState::CSnapshot&& snapshot_) : snapshot(makeUnique<COutputState::CSnapshot>(std::move(snapshot_))) {
    ;
}

Aquamarine::CDRMAsyncCommitData::~CDRMAsyncCommitData() {
    rollbackMgpu();
    releasePins();
}

void Aquamarine::CDRMAsyncCommitData::pinBuffers() {
    if (mainBuffer && !mainPinned) {
        mainBuffer->backendPin();
        mainPinned = true;
    }

    if (cursorBuffer && cursorBuffer != mainBuffer && !cursorPinned) {
        cursorBuffer->backendPin();
        cursorPinned = true;
    }
}

void Aquamarine::CDRMAsyncCommitData::releasePins() {
    if (mainPinned && mainBuffer) {
        mainBuffer->backendUnpin();
        mainPinned = false;
    }

    if (cursorPinned && cursorBuffer) {
        cursorBuffer->backendUnpin();
        cursorPinned = false;
    }
}

void Aquamarine::CDRMAsyncCommitData::rollbackMgpu() {
    if (!mgpuAcquired || !mgpuSwapchain)
        return;

    mgpuSwapchain->rollback();
    mgpuAcquired = false;
}

IDRMCommitSubmitter::SResult Aquamarine::CDRMAsyncCommitData::submit(int drmFD, uint64_t commitID) const noexcept {
    if (!request || !commitID || !flipID)
        return {.submitted = false, .error = EINVAL};

    if (lateCursor && cursorMailbox && cursorPlaneID && cursorXProp && cursorYProp) {
        const auto POSITION = cursorMailbox->load() - cursorHotspot;
        if (!request->addRaw(cursorPlaneID, cursorXProp, sc<uint64_t>(sc<int64_t>(POSITION.x))) ||
            !request->addRaw(cursorPlaneID, cursorYProp, sc<uint64_t>(sc<int64_t>(POSITION.y))))
            return {.submitted = false, .error = EINVAL};
    }

    const auto RESULT = request->submitAsync(drmFD, flags, flipID);
    return {.submitted = RESULT.submitted, .error = RESULT.error};
}

class CDRMCommitSubmitter final : public IDRMCommitSubmitter {
  public:
    explicit CDRMCommitSubmitter(int drmFD_) : drmFD(drmFD_) {
        ;
    }

    virtual SResult submit(uint64_t commitID, const IDRMCommitThreadData& data) noexcept {
        const auto* ASYNC_DATA = dynamic_cast<const CDRMAsyncCommitData*>(&data);
        if (!ASYNC_DATA)
            return {.submitted = false, .error = EINVAL};

        return ASYNC_DATA->submit(drmFD, commitID);
    }

  private:
    int drmFD = -1;
};

CUniquePointer<IDRMCommitSubmitter> Aquamarine::makeDRMCommitSubmitter(int drmFD) {
    if (drmFD < 0)
        return nullptr;

    return makeUnique<CDRMCommitSubmitter>(drmFD);
}
