#include "CommitThread.hpp"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <limits>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;

Aquamarine::IDRMCommitThreadData::~IDRMCommitThreadData() {
    ;
}

Aquamarine::SDRMCommitThreadRequest::SDRMCommitThreadRequest()                                                 = default;
Aquamarine::SDRMCommitThreadRequest::SDRMCommitThreadRequest(SDRMCommitThreadRequest&&)                        = default;
Aquamarine::SDRMCommitThreadRequest& Aquamarine::SDRMCommitThreadRequest::operator=(SDRMCommitThreadRequest&&) = default;
Aquamarine::SDRMCommitThreadRequest::~SDRMCommitThreadRequest()                                                = default;

Aquamarine::IDRMCommitSubmitter::~IDRMCommitSubmitter() {
    ;
}

Aquamarine::CDRMCommitThread::SResult::SResult()                                                   = default;
Aquamarine::CDRMCommitThread::SResult::SResult(SResult&&)                                          = default;
Aquamarine::CDRMCommitThread::SResult& Aquamarine::CDRMCommitThread::SResult::operator=(SResult&&) = default;
Aquamarine::CDRMCommitThread::SResult::~SResult()                                                  = default;

CUniquePointer<CDRMCommitThread> Aquamarine::CDRMCommitThread::create(CUniquePointer<IDRMCommitSubmitter> submitter) {
    if (!submitter)
        return nullptr;

    auto thread = CUniquePointer<CDRMCommitThread>{new CDRMCommitThread(std::move(submitter))};
    if (!thread->m_completionFD.isValid())
        return nullptr;

    thread->m_thread = std::thread([threadPtr = thread.get()] { threadPtr->run(); });
    return thread;
}

Aquamarine::CDRMCommitThread::CDRMCommitThread(CUniquePointer<IDRMCommitSubmitter> submitter) :
    m_submitter(std::move(submitter)), m_completionFD(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    ;
}

Aquamarine::CDRMCommitThread::~CDRMCommitThread() {
    stop();
}

std::optional<uint64_t> Aquamarine::CDRMCommitThread::enqueue(CUniquePointer<SDRMCommitThreadRequest>&& request) {
    if (!request || !request->data || request->id != 0 || request->ownerID == 0 || request->queueKey == 0)
        return std::nullopt;

    uint64_t commitID = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        constexpr size_t            MAX_QUEUED_REQUESTS = 64;
        if (m_stopping || m_cancelledOwners.contains(request->ownerID) || m_queue.size() >= MAX_QUEUED_REQUESTS)
            return std::nullopt;

        ++m_lastCommitID;
        if (m_lastCommitID == 0)
            ++m_lastCommitID;

        commitID    = m_lastCommitID;
        request->id = commitID;

        m_queue.emplace_back(SQueuedRequest{
            .sequence = ++m_lastSequence,
            .request  = std::move(request),
        });
    }

    m_queueCondition.notify_one();
    return commitID;
}

size_t Aquamarine::CDRMCommitThread::cancelOwner(uint64_t ownerID) {
    if (ownerID == 0)
        return 0;

    std::vector<CUniquePointer<SDRMCommitThreadRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_cancelledOwners.emplace(ownerID);
    }

    requests = takeOwnerRequests(ownerID);

    std::unordered_set<uint64_t> cancelledQueues;
    for (const auto& request : requests)
        cancelledQueues.emplace(request->queueKey);

    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_queueCondition.wait(
            lock, [this, ownerID, &cancelledQueues] { return !m_activeRequest || (m_activeRequest->ownerID != ownerID && !cancelledQueues.contains(m_activeRequest->queueKey)); });
    }

    for (auto& request : requests) {
        SResult result;
        result.request     = std::move(request);
        result.status      = AQ_DRM_COMMIT_THREAD_CANCELLED;
        result.error       = ECANCELED;
        result.completedAt = std::chrono::steady_clock::now();
        appendResult(std::move(result));
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::erase_if(m_blockedQueues, [ownerID](const auto& entry) { return entry.second.ownerID == ownerID; });
        for (const auto QUEUE : cancelledQueues)
            m_cancellingQueues.erase(QUEUE);
    }

    m_queueCondition.notify_one();

    return requests.size();
}

bool Aquamarine::CDRMCommitThread::pauseQueue(uint64_t queueKey) {
    if (queueKey == 0)
        return false;

    std::unique_lock<std::mutex> lock(m_queueMutex);
    ++m_pausedQueues[queueKey];
    m_queueCondition.wait(lock, [this, queueKey] { return !m_activeRequest || m_activeRequest->queueKey != queueKey; });
    return true;
}

bool Aquamarine::CDRMCommitThread::resumeQueue(uint64_t queueKey) {
    if (queueKey == 0)
        return false;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const auto                  PAUSE = m_pausedQueues.find(queueKey);
        if (PAUSE == m_pausedQueues.end())
            return false;
        if (--PAUSE->second == 0)
            m_pausedQueues.erase(PAUSE);
    }

    m_queueCondition.notify_one();
    return true;
}

bool Aquamarine::CDRMCommitThread::releaseQueue(uint64_t queueKey, uint64_t commitID) {
    if (queueKey == 0 || commitID == 0)
        return false;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const auto                  BLOCKER = m_blockedQueues.find(queueKey);
        if (BLOCKER == m_blockedQueues.end() || BLOCKER->second.commitID != commitID)
            return false;

        m_blockedQueues.erase(BLOCKER);
    }

    m_queueCondition.notify_one();
    return true;
}

void Aquamarine::CDRMCommitThread::stop() {
    std::lock_guard<std::mutex>                          stopLock(m_stopMutex);

    std::vector<CUniquePointer<SDRMCommitThreadRequest>> cancelled;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_stopping) {
            m_stopping = true;
            cancelled.reserve(m_queue.size());
            for (auto& queued : m_queue)
                cancelled.emplace_back(std::move(queued.request));
            m_queue.clear();
        }
    }

    m_queueCondition.notify_one();
    if (m_thread.joinable() && m_thread.get_id() == std::this_thread::get_id())
        std::terminate();

    if (m_thread.joinable())
        m_thread.join();

    for (auto& request : cancelled) {
        SResult result;
        result.request     = std::move(request);
        result.status      = AQ_DRM_COMMIT_THREAD_CANCELLED;
        result.error       = ECANCELED;
        result.completedAt = std::chrono::steady_clock::now();
        appendResult(std::move(result));
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_activeRequest.reset();
        m_blockedQueues.clear();
    }
}

std::vector<CDRMCommitThread::SResult> Aquamarine::CDRMCommitThread::stopAndDrain() {
    stop();
    return drainResults();
}

int Aquamarine::CDRMCommitThread::completionFD() const {
    return m_completionFD.get();
}

std::vector<CDRMCommitThread::SResult> Aquamarine::CDRMCommitThread::drainResults() {
    std::vector<SResult> results;
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);

        uint64_t                    signals = 0;
        while (true) {
            const auto READ = read(m_completionFD.get(), &signals, sizeof(signals));
            if (READ == sizeof(signals))
                continue;
            if (READ < 0 && errno == EINTR)
                continue;
            break;
        }

        results = std::move(m_results);
        m_results.clear();
    }

    return results;
}

bool Aquamarine::CDRMCommitThread::accepting() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return !m_stopping;
}

size_t Aquamarine::CDRMCommitThread::queued() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_queue.size();
}

bool Aquamarine::CDRMCommitThread::submitting() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_activeRequest.has_value();
}

void Aquamarine::CDRMCommitThread::run() {
    while (true) {
        CUniquePointer<SDRMCommitThreadRequest> request;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            request = takeNextRequest(lock);
        }

        if (!request)
            return;

        const auto SUBMIT_RESULT = m_submitter->submit(request->id, *request->data);
        const auto COMPLETED_AT  = std::chrono::steady_clock::now();

        SResult    result;
        result.status       = SUBMIT_RESULT.submitted ? AQ_DRM_COMMIT_THREAD_SUBMITTED : AQ_DRM_COMMIT_THREAD_FAILED;
        result.error        = SUBMIT_RESULT.submitted ? 0 : (SUBMIT_RESULT.error > 0 ? SUBMIT_RESULT.error : EIO);
        result.missedTarget = SUBMIT_RESULT.submitted && request->targetPresentation.has_value() && COMPLETED_AT > *request->targetPresentation;
        result.completedAt  = COMPLETED_AT;
        result.request      = std::move(request);
        appendResult(std::move(result));

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            const auto&                 REQUEST = *m_activeRequest;
            if (!SUBMIT_RESULT.submitted) {
                const auto BLOCKER = m_blockedQueues.find(REQUEST.queueKey);
                if (BLOCKER != m_blockedQueues.end() && BLOCKER->second.commitID == REQUEST.commitID)
                    m_blockedQueues.erase(BLOCKER);
            }

            m_activeRequest.reset();
        }

        m_queueCondition.notify_all();
    }
}

CUniquePointer<SDRMCommitThreadRequest> Aquamarine::CDRMCommitThread::takeNextRequest(std::unique_lock<std::mutex>& lock) {
    while (true) {
        if (m_stopping)
            return nullptr;

        const auto                                           NOW = std::chrono::steady_clock::now();

        std::unordered_set<uint64_t>                         seenQueues;
        std::optional<size_t>                                candidate;
        std::chrono::steady_clock::time_point                candidateDeadline = std::chrono::steady_clock::time_point::max();
        uint64_t                                             candidateSequence = std::numeric_limits<uint64_t>::max();
        std::optional<std::chrono::steady_clock::time_point> earliestWake;

        for (size_t i = 0; i < m_queue.size(); ++i) {
            const auto& QUEUED = m_queue.at(i);
            const auto  KEY    = QUEUED.request->queueKey;

            if (!seenQueues.emplace(KEY).second || m_blockedQueues.contains(KEY) || m_pausedQueues.contains(KEY) || m_cancellingQueues.contains(KEY) ||
                m_cancelledOwners.contains(QUEUED.request->ownerID))
                continue;

            const auto SUBMIT_AT = QUEUED.request->submitAt.value_or(std::chrono::steady_clock::time_point::min());
            if (SUBMIT_AT > NOW) {
                if (!earliestWake || SUBMIT_AT < *earliestWake)
                    earliestWake = SUBMIT_AT;
                continue;
            }

            const auto DEADLINE = QUEUED.request->targetPresentation.value_or(std::chrono::steady_clock::time_point::max());
            if (!candidate || DEADLINE < candidateDeadline || (DEADLINE == candidateDeadline && QUEUED.sequence < candidateSequence)) {
                candidate         = i;
                candidateDeadline = DEADLINE;
                candidateSequence = QUEUED.sequence;
            }
        }

        if (!candidate) {
            if (earliestWake)
                m_queueCondition.wait_until(lock, *earliestWake);
            else
                m_queueCondition.wait(lock);
            continue;
        }

        auto request = std::move(m_queue.at(*candidate).request);
        m_queue.erase(m_queue.begin() + *candidate);
        m_activeRequest = SActiveRequest{
            .commitID = request->id,
            .ownerID  = request->ownerID,
            .queueKey = request->queueKey,
        };
        if (request->blocksQueue) {
            m_blockedQueues[request->queueKey] = SBlocker{
                .commitID = request->id,
                .ownerID  = request->ownerID,
            };
        }
        return request;
    }
}

void Aquamarine::CDRMCommitThread::appendResult(SResult&& result) {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    m_results.emplace_back(std::move(result));
    signalResults();
}

void Aquamarine::CDRMCommitThread::signalResults() {
    const uint64_t SIGNAL = 1;
    while (write(m_completionFD.get(), &SIGNAL, sizeof(SIGNAL)) < 0 && errno == EINTR) {
        ;
    }
}

std::vector<CUniquePointer<SDRMCommitThreadRequest>> Aquamarine::CDRMCommitThread::takeOwnerRequests(uint64_t ownerID) {
    std::vector<CUniquePointer<SDRMCommitThreadRequest>> requests;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (auto it = m_queue.begin(); it != m_queue.end();) {
            if (it->request->ownerID != ownerID) {
                ++it;
                continue;
            }

            m_cancellingQueues.emplace(it->request->queueKey);
            requests.emplace_back(std::move(it->request));
            it = m_queue.erase(it);
        }
    }

    return requests;
}
