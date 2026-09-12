#include <aquamarine/output/Output.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include "OutputTiming.hpp"
#include "shared.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;

class CTestOutput : public IOutput {
  public:
    virtual bool commit() {
        return false;
    }

    virtual bool test() {
        return true;
    }

    virtual CSharedPointer<IBackendImplementation> getBackend() {
        return nullptr;
    }

    virtual std::vector<SDRMFormat> getRenderFormats() {
        return {};
    }

    virtual bool pendingPageFlip() {
        return false;
    }

    virtual bool pendingIdleFrame() {
        return false;
    }
};

class CTestBuffer : public IBuffer {
  public:
    virtual eBufferCapability caps() {
        return BUFFER_CAPABILITY_NONE;
    }

    virtual eBufferType type() {
        return BUFFER_TYPE_MISC;
    }

    virtual void update(const Hyprutils::Math::CRegion& damage) {
        ;
    }

    virtual bool isSynchronous() {
        return true;
    }

    virtual bool good() {
        return true;
    }
};

int main() {
    int         ret = 0;
    CTestOutput output;

    CTestBuffer buffer;
    int         backendReleases = 0;
    auto        releaseListener = buffer.events.backendRelease.listen([&] { backendReleases++; });

    buffer.backendPin();
    buffer.backendPin();
    EXPECT(buffer.backendPinCount(), 2U);
    EXPECT(buffer.lockedByBackend, true);
    buffer.backendUnpin();
    EXPECT(buffer.backendPinCount(), 1U);
    EXPECT(buffer.lockedByBackend, true);
    EXPECT(backendReleases, 0);
    buffer.backendUnpin();
    EXPECT(buffer.backendPinCount(), 0U);
    EXPECT(buffer.lockedByBackend, false);
    EXPECT(backendReleases, 1);
    buffer.backendPin();
    buffer.backendUnpin();
    EXPECT(backendReleases, 2);

    COutputState outputState;
    outputState.setEnabled(true);
    outputState.addDamage(Hyprutils::Math::CRegion{0, 0, 10, 10});
    const auto FIRST_SNAPSHOT = outputState.snapshot();

    outputState.setEnabled(false);
    outputState.addDamage(Hyprutils::Math::CRegion{20, 20, 10, 10});
    EXPECT(FIRST_SNAPSHOT.state().enabled, true);
    EXPECT(FIRST_SNAPSHOT.state().damage.empty(), false);

    outputState.consume(FIRST_SNAPSHOT);
    EXPECT(outputState.state().enabled, false);
    EXPECT(!!(outputState.state().committed & COutputState::AQ_OUTPUT_STATE_ENABLED), true);
    EXPECT(!!(outputState.state().committed & COutputState::AQ_OUTPUT_STATE_DAMAGE), true);
    EXPECT(outputState.state().damage.empty(), false);

    const auto SECOND_SNAPSHOT = outputState.snapshot();
    outputState.consume(SECOND_SNAPSHOT);
    EXPECT(outputState.state().committed, 0U);
    EXPECT(outputState.state().damage.empty(), true);

    COutputState retryState;
    retryState.setFormat(DRM_FORMAT_XRGB8888);
    retryState.addDamage(Hyprutils::Math::CRegion{0, 0, 20, 20});
    const auto RETRY_SNAPSHOT = retryState.snapshot();
    retryState.consume(RETRY_SNAPSHOT);
    EXPECT(retryState.state().committed, 0U);
    retryState.rearm(RETRY_SNAPSHOT);
    EXPECT(!!(retryState.state().committed & COutputState::AQ_OUTPUT_STATE_FORMAT), true);
    EXPECT(!!(retryState.state().committed & COutputState::AQ_OUTPUT_STATE_DAMAGE), true);
    EXPECT(retryState.state().damage.empty(), false);

    const auto STALE_RETRY_SNAPSHOT = retryState.snapshot();
    retryState.consume(STALE_RETRY_SNAPSHOT);
    retryState.setFormat(DRM_FORMAT_ARGB8888);
    retryState.rearm(STALE_RETRY_SNAPSHOT);
    EXPECT(retryState.state().drmFormat, DRM_FORMAT_ARGB8888);

    outputState.setFormat(DRM_FORMAT_XRGB8888);
    outputState.setAdaptiveSync(true);
    const auto PARTIAL_SNAPSHOT = outputState.snapshot();
    outputState.setAdaptiveSync(false);
    outputState.consume(PARTIAL_SNAPSHOT);
    EXPECT(!!(outputState.state().committed & COutputState::AQ_OUTPUT_STATE_FORMAT), false);
    EXPECT(!!(outputState.state().committed & COutputState::AQ_OUTPUT_STATE_ADAPTIVE_SYNC), true);

    int pipeFDs[2] = {-1, -1};
    EXPECT(pipe(pipeFDs), 0);
    if (pipeFDs[0] >= 0) {
        COutputState fenceState;
        fenceState.setExplicitInFence(pipeFDs[0]);
        const auto FENCE_SNAPSHOT = fenceState.snapshot();
        EXPECT(FENCE_SNAPSHOT.error(), 0);
        EXPECT(FENCE_SNAPSHOT.state().explicitInFence == pipeFDs[0], false);
        close(pipeFDs[0]);
        close(pipeFDs[1]);
        EXPECT(fcntl(FENCE_SNAPSHOT.state().explicitInFence, F_GETFD) >= 0, true);
    }

    output.state->setEnabled(true);
    EXPECT(output.test(), true);
    EXPECT(!!(output.state->state().committed & COutputState::AQ_OUTPUT_STATE_ENABLED), true);

    EXPECT(output.hasCursorPlane(), false);
    EXPECT(output.nextVBlank().has_value(), false);
    EXPECT(output.commitCapabilities(), 0U);

    const IOutput::SCommitOptions DEFAULT_OPTIONS;
    EXPECT(DEFAULT_OPTIONS.targetPresentation.has_value(), false);
    EXPECT(DEFAULT_OPTIONS.lateCursor, false);

    bool                         resultFired        = false;
    uint64_t                     resultID           = 0;
    IOutput::eOutputCommitStatus resultStatus       = IOutput::AQ_OUTPUT_COMMIT_FAILED;
    int                          resultError        = 0;
    bool                         resultMissedTarget = false;
    auto                         resultListener     = output.events.commitResult.listen([&](const IOutput::SCommitResult& event) {
        resultFired        = true;
        resultID           = event.id;
        resultStatus       = event.status;
        resultError        = event.error;
        resultMissedTarget = event.missedTarget;
    });

    const auto                   SUBMISSION = output.commitAsync({});
    EXPECT(SUBMISSION.id, 0U);
    EXPECT(SUBMISSION.error, ENOTSUP);
    EXPECT(resultFired, false);

    output.events.commitResult.emit({
        .id           = 42,
        .status       = IOutput::AQ_OUTPUT_COMMIT_SUBMITTED,
        .missedTarget = true,
    });

    EXPECT(resultFired, true);
    EXPECT(resultID, 42U);
    EXPECT(resultStatus, IOutput::AQ_OUTPUT_COMMIT_SUBMITTED);
    EXPECT(resultError, 0);
    EXPECT(resultMissedTarget, true);

    const IOutput::SPresentEvent PRESENT_EVENT{
        .commitID = 42,
    };
    EXPECT(PRESENT_EVENT.commitID, 42U);

    using namespace std::chrono_literals;

    const auto NEXT = OutputTiming::predictNextVBlank(100ns, 150ns, 100ns);
    EXPECT(NEXT.has_value(), true);
    EXPECT(NEXT->count(), 200);

    EXPECT(OutputTiming::predictNextVBlank(100ns, 200ns, 100ns).has_value(), false);
    EXPECT(OutputTiming::predictNextVBlank(201ns, 200ns, 100ns).has_value(), false);
    EXPECT(OutputTiming::predictNextVBlank(100ns, 150ns, 0ns).has_value(), false);
    EXPECT(OutputTiming::predictNextVBlank(std::chrono::nanoseconds::max() - 50ns, std::chrono::nanoseconds::max() - 25ns, 100ns).has_value(), false);

    const auto STEADY_EPOCH = std::chrono::steady_clock::time_point{};
    const auto CONVERTED    = OutputTiming::toSteadyClock(200ns, 150ns, STEADY_EPOCH + 100ns, STEADY_EPOCH + 110ns);
    EXPECT(CONVERTED.has_value(), true);
    EXPECT(CONVERTED->time_since_epoch().count(), 155);
    EXPECT(OutputTiming::toSteadyClock(160ns, 150ns, STEADY_EPOCH + 100ns, STEADY_EPOCH + 120ns).has_value(), false);

    return ret;
}
