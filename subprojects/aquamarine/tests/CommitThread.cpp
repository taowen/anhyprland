#include "CommitThread.hpp"
#include "shared.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <poll.h>
#include <vector>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace std::chrono_literals;
#define UP CUniquePointer

class CTestCommitData : public IDRMCommitThreadData {
  public:
    explicit CTestCommitData(int value_) : value(value_) {
        ;
    }

    const int value = 0;
};

class CFakeCommitSubmitter : public IDRMCommitSubmitter {
  public:
    SResult submit(uint64_t commitID, const IDRMCommitThreadData& data) noexcept override {
        std::unique_lock<std::mutex> lock(m_mutex);

        const auto&                  COMMIT_DATA = *sc<const CTestCommitData*>(&data);

        m_submittedIDs.emplace_back(commitID);
        m_submittedValues.emplace_back(COMMIT_DATA.value);

        SResult result{.submitted = true};
        if (!m_results.empty()) {
            result = m_results.front();
            m_results.erase(m_results.begin());
        }

        m_condition.notify_all();
        m_condition.wait(lock, [this] { return !m_blocked; });
        return result;
    }

    void addResult(const SResult& result) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_results.emplace_back(result);
    }

    bool waitForSubmissions(size_t count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock, timeout, [this, count] { return m_submittedIDs.size() >= count; });
    }

    void block() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_blocked = true;
    }

    void unblock() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_blocked = false;
        }
        m_condition.notify_all();
    }

    std::vector<uint64_t> submittedIDs() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_submittedIDs;
    }

    std::vector<int> submittedValues() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_submittedValues;
    }

  private:
    std::mutex              m_mutex;
    std::condition_variable m_condition;
    std::vector<SResult>    m_results;
    std::vector<uint64_t>   m_submittedIDs;
    std::vector<int>        m_submittedValues;
    bool                    m_blocked = false;
};

static UP<SDRMCommitThreadRequest> makeRequest(uint64_t ownerID, uint64_t queueKey, int value) {
    auto request      = makeUnique<SDRMCommitThreadRequest>();
    request->ownerID  = ownerID;
    request->queueKey = queueKey;
    request->data     = makeUnique<CTestCommitData>(value);
    return request;
}

static uint64_t enqueue(CDRMCommitThread& thread, UP<SDRMCommitThreadRequest> request) {
    return thread.enqueue(std::move(request)).value_or(0);
}

static bool waitForResults(int fd) {
    pollfd descriptor{
        .fd     = fd,
        .events = POLLIN,
    };

    return poll(&descriptor, 1, 2000) == 1 && descriptor.revents & POLLIN;
}

static const CDRMCommitThread::SResult* resultFor(const std::vector<CDRMCommitThread::SResult>& results, uint64_t id) {
    const auto IT = std::ranges::find_if(results, [id](const auto& result) { return result.request->id == id; });
    return IT == results.end() ? nullptr : &*IT;
}

int main() {
    int  ret = 0;

    auto submitter    = makeUnique<CFakeCommitSubmitter>();
    auto submitterPtr = submitter.get();
    auto thread       = CDRMCommitThread::create(std::move(submitter));

    EXPECT(!!thread, true);
    if (!thread)
        return 1;

    EXPECT(thread->completionFD() >= 0, true);
    EXPECT(thread->accepting(), true);

    const auto FIRST  = enqueue(*thread, makeRequest(1, 10, 100));
    const auto SECOND = enqueue(*thread, makeRequest(2, 10, 200));

    EXPECT(FIRST != 0, true);
    EXPECT(SECOND > FIRST, true);
    EXPECT(submitterPtr->waitForSubmissions(1), true);
    EXPECT(waitForResults(thread->completionFD()), true);

    auto results = thread->drainResults();
    EXPECT(results.size(), 1U);
    EXPECT(resultFor(results, FIRST) != nullptr, true);
    EXPECT(resultFor(results, FIRST)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_SUBMITTED);
    EXPECT(thread->cancelOwner(2), 1U);
    EXPECT(waitForResults(thread->completionFD()), true);
    results = thread->drainResults();
    EXPECT(results.size(), 1U);
    EXPECT(resultFor(results, SECOND) != nullptr, true);
    EXPECT(resultFor(results, SECOND)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_CANCELLED);
    EXPECT(resultFor(results, SECOND)->error, ECANCELED);

    const auto INDEPENDENT = enqueue(*thread, makeRequest(3, 20, 300));
    EXPECT(INDEPENDENT != 0, true);
    EXPECT(submitterPtr->waitForSubmissions(2), true);
    EXPECT(waitForResults(thread->completionFD()), true);
    results = thread->drainResults();
    EXPECT(resultFor(results, INDEPENDENT) != nullptr, true);

    const auto BLOCKED = enqueue(*thread, makeRequest(4, 10, 400));
    EXPECT(BLOCKED != 0, true);
    EXPECT(thread->cancelOwner(4), 1U);
    EXPECT(waitForResults(thread->completionFD()), true);
    results = thread->drainResults();
    EXPECT(resultFor(results, BLOCKED) != nullptr, true);
    EXPECT(resultFor(results, BLOCKED)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_CANCELLED);

    EXPECT(thread->releaseQueue(10, FIRST), true);
    EXPECT(thread->releaseQueue(10, FIRST), false);
    const auto RELEASED = enqueue(*thread, makeRequest(5, 10, 500));
    EXPECT(RELEASED != 0, true);
    EXPECT(submitterPtr->waitForSubmissions(3), true);
    EXPECT(waitForResults(thread->completionFD()), true);
    results = thread->drainResults();
    EXPECT(resultFor(results, RELEASED) != nullptr, true);

    submitterPtr->addResult({.submitted = false, .error = EINVAL});
    EXPECT(thread->releaseQueue(20, INDEPENDENT), true);
    const auto FAILING = enqueue(*thread, makeRequest(6, 20, 600));
    EXPECT(FAILING != 0, true);
    EXPECT(submitterPtr->waitForSubmissions(4), true);
    EXPECT(waitForResults(thread->completionFD()), true);
    results = thread->drainResults();
    EXPECT(resultFor(results, FAILING) != nullptr, true);
    EXPECT(resultFor(results, FAILING)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_FAILED);
    EXPECT(resultFor(results, FAILING)->error, EINVAL);

    auto late                = makeRequest(7, 30, 700);
    late->targetPresentation = std::chrono::steady_clock::now() - 1ms;
    late->blocksQueue        = false;
    const auto LATE          = enqueue(*thread, std::move(late));
    EXPECT(LATE != 0, true);
    EXPECT(submitterPtr->waitForSubmissions(5), true);
    EXPECT(waitForResults(thread->completionFD()), true);
    results = thread->drainResults();
    EXPECT(resultFor(results, LATE) != nullptr, true);
    EXPECT(resultFor(results, LATE)->missedTarget, true);

    submitterPtr->block();
    auto active         = makeRequest(9, 50, 900);
    active->blocksQueue = false;
    const auto ACTIVE   = enqueue(*thread, std::move(active));
    EXPECT(ACTIVE != 0, true);
    EXPECT(submitterPtr->waitForSubmissions(6), true);

    auto activeFollower         = makeRequest(9, 50, 901);
    activeFollower->blocksQueue = false;
    const auto ACTIVE_FOLLOWER  = enqueue(*thread, std::move(activeFollower));
    EXPECT(ACTIVE_FOLLOWER != 0, true);

    auto cancellation = std::async(std::launch::async, [&thread] { return thread->cancelOwner(9); });
    EXPECT(cancellation.wait_for(20ms) == std::future_status::timeout, true);
    submitterPtr->unblock();
    EXPECT(cancellation.wait_for(2s) == std::future_status::ready, true);
    EXPECT(cancellation.get(), 1U);
    EXPECT(waitForResults(thread->completionFD()), true);
    results = thread->drainResults();
    EXPECT(results.size(), 2U);
    EXPECT(results.at(0).request->id, ACTIVE);
    EXPECT(results.at(1).request->id, ACTIVE_FOLLOWER);
    EXPECT(resultFor(results, ACTIVE) != nullptr, true);
    EXPECT(resultFor(results, ACTIVE)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_SUBMITTED);
    EXPECT(resultFor(results, ACTIVE_FOLLOWER)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_CANCELLED);

    auto cancelledOwner = makeRequest(9, 50, 901);
    EXPECT(thread->enqueue(std::move(cancelledOwner)).has_value(), false);
    EXPECT(!!cancelledOwner, true);

    auto future       = makeRequest(8, 40, 800);
    future->submitAt  = std::chrono::steady_clock::now() + 1h;
    const auto FUTURE = enqueue(*thread, std::move(future));
    EXPECT(FUTURE != 0, true);
    results = thread->stopAndDrain();
    EXPECT(thread->accepting(), false);
    EXPECT(resultFor(results, FUTURE) != nullptr, true);
    EXPECT(resultFor(results, FUTURE)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_CANCELLED);

    const auto IDS    = submitterPtr->submittedIDs();
    const auto VALUES = submitterPtr->submittedValues();
    EXPECT(IDS.size(), 6U);
    EXPECT(VALUES.size(), 6U);
    EXPECT(VALUES.at(0), 100);
    EXPECT(VALUES.at(1), 300);
    EXPECT(VALUES.at(2), 500);
    EXPECT(VALUES.at(3), 600);
    EXPECT(VALUES.at(4), 700);
    EXPECT(VALUES.at(5), 900);

    auto rejected = makeRequest(10, 50, 1000);
    EXPECT(thread->enqueue(std::move(rejected)).has_value(), false);
    EXPECT(!!rejected, true);

    auto deadlineSubmitter    = makeUnique<CFakeCommitSubmitter>();
    auto deadlineSubmitterPtr = deadlineSubmitter.get();
    auto deadlineThread       = CDRMCommitThread::create(std::move(deadlineSubmitter));
    EXPECT(!!deadlineThread, true);

    deadlineSubmitterPtr->block();
    auto gate         = makeRequest(11, 60, 1100);
    gate->blocksQueue = false;
    EXPECT(enqueue(*deadlineThread, std::move(gate)) != 0, true);
    EXPECT(deadlineSubmitterPtr->waitForSubmissions(1), true);

    auto laterDeadline                  = makeRequest(12, 70, 1200);
    laterDeadline->blocksQueue          = false;
    laterDeadline->targetPresentation   = std::chrono::steady_clock::now() + 2s;
    auto earlierDeadline                = makeRequest(13, 80, 1300);
    earlierDeadline->blocksQueue        = false;
    earlierDeadline->targetPresentation = std::chrono::steady_clock::now() + 1s;
    EXPECT(enqueue(*deadlineThread, std::move(laterDeadline)) != 0, true);
    EXPECT(enqueue(*deadlineThread, std::move(earlierDeadline)) != 0, true);

    deadlineSubmitterPtr->unblock();
    EXPECT(deadlineSubmitterPtr->waitForSubmissions(3), true);
    auto deadlineValues = deadlineSubmitterPtr->submittedValues();
    EXPECT(deadlineValues.at(0), 1100);
    EXPECT(deadlineValues.at(1), 1300);
    EXPECT(deadlineValues.at(2), 1200);
    EXPECT(waitForResults(deadlineThread->completionFD()), true);
    deadlineThread->drainResults();

    auto sleeping         = makeRequest(14, 90, 1400);
    sleeping->blocksQueue = false;
    sleeping->submitAt    = std::chrono::steady_clock::now() + 1h;
    const auto SLEEPING   = enqueue(*deadlineThread, std::move(sleeping));
    EXPECT(SLEEPING != 0, true);
    EXPECT(deadlineSubmitterPtr->waitForSubmissions(4, 20ms), false);

    auto wake         = makeRequest(15, 100, 1500);
    wake->blocksQueue = false;
    EXPECT(enqueue(*deadlineThread, std::move(wake)) != 0, true);
    EXPECT(deadlineSubmitterPtr->waitForSubmissions(4), true);
    deadlineValues = deadlineSubmitterPtr->submittedValues();
    EXPECT(deadlineValues.at(3), 1500);

    const auto DEADLINE_RESULTS = deadlineThread->stopAndDrain();
    EXPECT(resultFor(DEADLINE_RESULTS, SLEEPING)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_CANCELLED);

    auto stopSubmitter    = makeUnique<CFakeCommitSubmitter>();
    auto stopSubmitterPtr = stopSubmitter.get();
    auto stopThread       = CDRMCommitThread::create(std::move(stopSubmitter));
    EXPECT(!!stopThread, true);

    stopSubmitterPtr->block();
    auto activeStop         = makeRequest(16, 110, 1600);
    activeStop->blocksQueue = false;
    const auto ACTIVE_STOP  = enqueue(*stopThread, std::move(activeStop));
    EXPECT(ACTIVE_STOP != 0, true);
    EXPECT(stopSubmitterPtr->waitForSubmissions(1), true);

    auto stopping = std::async(std::launch::async, [&stopThread] { return stopThread->stopAndDrain(); });
    EXPECT(stopping.wait_for(20ms) == std::future_status::timeout, true);
    stopSubmitterPtr->unblock();
    EXPECT(stopping.wait_for(2s) == std::future_status::ready, true);
    const auto STOP_RESULTS = stopping.get();
    EXPECT(resultFor(STOP_RESULTS, ACTIVE_STOP)->status, CDRMCommitThread::AQ_DRM_COMMIT_THREAD_SUBMITTED);

    auto barrierSubmitter    = makeUnique<CFakeCommitSubmitter>();
    auto barrierSubmitterPtr = barrierSubmitter.get();
    auto barrierThread       = CDRMCommitThread::create(std::move(barrierSubmitter));
    EXPECT(!!barrierThread, true);

    EXPECT(barrierThread->pauseQueue(0), false);
    EXPECT(barrierThread->resumeQueue(0), false);
    EXPECT(barrierThread->pauseQueue(120), true);
    EXPECT(barrierThread->pauseQueue(120), true);

    auto paused           = makeRequest(17, 120, 1700);
    paused->blocksQueue   = false;
    const auto PAUSED     = enqueue(*barrierThread, std::move(paused));
    auto       unpaused   = makeRequest(18, 130, 1800);
    unpaused->blocksQueue = false;
    const auto UNPAUSED   = enqueue(*barrierThread, std::move(unpaused));
    EXPECT(PAUSED != 0, true);
    EXPECT(UNPAUSED != 0, true);
    EXPECT(barrierSubmitterPtr->waitForSubmissions(1), true);
    EXPECT(barrierSubmitterPtr->submittedIDs().at(0), UNPAUSED);
    EXPECT(barrierSubmitterPtr->waitForSubmissions(2, 20ms), false);

    EXPECT(barrierThread->resumeQueue(120), true);
    EXPECT(barrierSubmitterPtr->waitForSubmissions(2, 20ms), false);
    EXPECT(barrierThread->resumeQueue(120), true);
    EXPECT(barrierSubmitterPtr->waitForSubmissions(2), true);
    EXPECT(barrierSubmitterPtr->submittedIDs().at(1), PAUSED);
    EXPECT(barrierThread->resumeQueue(120), false);

    barrierSubmitterPtr->block();
    auto activeBarrier         = makeRequest(19, 140, 1900);
    activeBarrier->blocksQueue = false;
    EXPECT(enqueue(*barrierThread, std::move(activeBarrier)) != 0, true);
    EXPECT(barrierSubmitterPtr->waitForSubmissions(3), true);

    auto pausing = std::async(std::launch::async, [&barrierThread] { return barrierThread->pauseQueue(140); });
    EXPECT(pausing.wait_for(20ms) == std::future_status::timeout, true);
    barrierSubmitterPtr->unblock();
    EXPECT(pausing.wait_for(2s) == std::future_status::ready, true);
    EXPECT(pausing.get(), true);
    EXPECT(barrierThread->resumeQueue(140), true);
    barrierThread->stopAndDrain();

    return ret;
}
