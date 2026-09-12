#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

namespace Aquamarine {
    class IDRMCommitThreadData {
      public:
        virtual ~IDRMCommitThreadData();
    };

    struct SDRMCommitThreadRequest {
        SDRMCommitThreadRequest();
        SDRMCommitThreadRequest(SDRMCommitThreadRequest&&);
        SDRMCommitThreadRequest& operator=(SDRMCommitThreadRequest&&);
        ~SDRMCommitThreadRequest();

        uint64_t                                                id       = 0;
        uint64_t                                                ownerID  = 0;
        uint64_t                                                queueKey = 0;
        std::optional<std::chrono::steady_clock::time_point>    submitAt;
        std::optional<std::chrono::steady_clock::time_point>    targetPresentation;
        bool                                                    blocksQueue = true;
        Hyprutils::Memory::CUniquePointer<IDRMCommitThreadData> data;
    };

    class IDRMCommitSubmitter {
      public:
        struct SResult {
            bool submitted = false;
            int  error     = 0;
        };

        virtual ~IDRMCommitSubmitter();

        // Data must own everything used by submit() and must not reference mutable backend state.
        // submit() must not wait on user-space synchronization and must eventually return.
        virtual SResult submit(uint64_t commitID, const IDRMCommitThreadData& data) noexcept = 0;
    };

    class CDRMCommitThread {
      public:
        enum eResultStatus : uint32_t {
            AQ_DRM_COMMIT_THREAD_SUBMITTED = 0,
            AQ_DRM_COMMIT_THREAD_FAILED,
            AQ_DRM_COMMIT_THREAD_CANCELLED,
        };

        struct SResult {
            SResult();
            SResult(SResult&&);
            SResult& operator=(SResult&&);
            ~SResult();

            Hyprutils::Memory::CUniquePointer<SDRMCommitThreadRequest> request;
            eResultStatus                                              status       = AQ_DRM_COMMIT_THREAD_FAILED;
            int                                                        error        = 0;
            bool                                                       missedTarget = false;
            std::chrono::steady_clock::time_point                      completedAt;
        };

        static Hyprutils::Memory::CUniquePointer<CDRMCommitThread> create(Hyprutils::Memory::CUniquePointer<IDRMCommitSubmitter> submitter);

        ~CDRMCommitThread();

        std::optional<uint64_t> enqueue(Hyprutils::Memory::CUniquePointer<SDRMCommitThreadRequest>&& request);
        size_t                  cancelOwner(uint64_t ownerID);
        bool                    pauseQueue(uint64_t queueKey);
        bool                    resumeQueue(uint64_t queueKey);
        bool                    releaseQueue(uint64_t queueKey, uint64_t commitID);
        std::vector<SResult>    stopAndDrain();

        int                     completionFD() const;
        std::vector<SResult>    drainResults();
        bool                    accepting() const;
        size_t                  queued() const;
        bool                    submitting() const;

      private:
        struct SQueuedRequest {
            uint64_t                                                   sequence = 0;
            Hyprutils::Memory::CUniquePointer<SDRMCommitThreadRequest> request;
        };

        struct SBlocker {
            uint64_t commitID = 0;
            uint64_t ownerID  = 0;
        };

        struct SActiveRequest {
            uint64_t commitID = 0;
            uint64_t ownerID  = 0;
            uint64_t queueKey = 0;
        };

        CDRMCommitThread(Hyprutils::Memory::CUniquePointer<IDRMCommitSubmitter> submitter);

        void                                                                    run();
        void                                                                    stop();
        Hyprutils::Memory::CUniquePointer<SDRMCommitThreadRequest>              takeNextRequest(std::unique_lock<std::mutex>& lock);
        void                                                                    appendResult(SResult&& result);
        void                                                                    signalResults();
        std::vector<Hyprutils::Memory::CUniquePointer<SDRMCommitThreadRequest>> takeOwnerRequests(uint64_t ownerID);

        Hyprutils::Memory::CUniquePointer<IDRMCommitSubmitter>                  m_submitter;
        Hyprutils::OS::CFileDescriptor                                          m_completionFD;
        std::thread                                                             m_thread;
        std::mutex                                                              m_stopMutex;

        mutable std::mutex                                                      m_queueMutex;
        std::condition_variable                                                 m_queueCondition;
        std::vector<SQueuedRequest>                                             m_queue;
        std::unordered_map<uint64_t, SBlocker>                                  m_blockedQueues;
        std::unordered_map<uint64_t, size_t>                                    m_pausedQueues;
        std::unordered_set<uint64_t>                                            m_cancellingQueues;
        std::unordered_set<uint64_t>                                            m_cancelledOwners;
        std::optional<SActiveRequest>                                           m_activeRequest;
        uint64_t                                                                m_lastCommitID = 0;
        uint64_t                                                                m_lastSequence = 0;
        bool                                                                    m_stopping     = false;

        std::mutex                                                              m_resultMutex;
        std::vector<SResult>                                                    m_results;
    };
}
