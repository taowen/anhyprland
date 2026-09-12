#pragma once

#include "CommitThread.hpp"
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/backend/drm/Atomic.hpp>

#include <atomic>
#include <memory>

namespace Aquamarine {
    class CDRMCursorPositionMailbox {
      public:
        void                      store(const Hyprutils::Math::Vector2D& position);
        Hyprutils::Math::Vector2D load() const;

      private:
        std::atomic<uint64_t> m_sequence = 0;
        std::atomic<int64_t>  m_x        = 0;
        std::atomic<int64_t>  m_y        = 0;
    };

    class CDRMAsyncCommitData : public IDRMCommitThreadData {
      public:
        CDRMAsyncCommitData(COutputState::CSnapshot&& snapshot_);
        virtual ~CDRMAsyncCommitData();

        void                                                               pinBuffers();
        void                                                               releasePins();
        void                                                               rollbackMgpu();
        IDRMCommitSubmitter::SResult                                       submit(int drmFD, uint64_t commitID) const noexcept;

        Hyprutils::Memory::CUniquePointer<COutputState::CSnapshot>         snapshot;
        SDRMConnectorCommitData                                            commitData;
        Hyprutils::Memory::CUniquePointer<CDRMAtomicRequest>               request;
        Hyprutils::Memory::CSharedPointer<CDRMOutput>                      output;
        Hyprutils::Memory::CSharedPointer<SDRMConnector>                   connector;
        Hyprutils::Memory::CSharedPointer<IBuffer>                         mainBuffer;
        Hyprutils::Memory::CSharedPointer<IBuffer>                         cursorBuffer;
        Hyprutils::Memory::CSharedPointer<CSwapchain>                      mgpuSwapchain;
        Hyprutils::OS::CFileDescriptor                                     mgpuFence;
        Hyprutils::Memory::CAtomicSharedPointer<CDRMCursorPositionMailbox> cursorMailbox;
        uint32_t                                                           flags         = 0;
        uintptr_t                                                          flipID        = 0;
        uint32_t                                                           cursorPlaneID = 0;
        uint32_t                                                           cursorXProp   = 0;
        uint32_t                                                           cursorYProp   = 0;
        Hyprutils::Math::Vector2D                                          cursorHotspot;
        bool                                                               lateCursor   = false;
        bool                                                               tearing      = false;
        bool                                                               mainPinned   = false;
        bool                                                               cursorPinned = false;
        bool                                                               mgpuAcquired = false;
    };

    Hyprutils::Memory::CUniquePointer<IDRMCommitSubmitter> makeDRMCommitSubmitter(int drmFD);
}
