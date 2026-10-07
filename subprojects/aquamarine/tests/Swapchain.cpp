#include <aquamarine/allocator/Swapchain.hpp>
#include <aquamarine/backend/Backend.hpp>
#include "shared.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;

class CTestBuffer : public IBuffer {
  public:
    eBufferCapability caps() override {
        return BUFFER_CAPABILITY_NONE;
    }
    eBufferType type() override {
        return BUFFER_TYPE_MISC;
    }
    void update(const Hyprutils::Math::CRegion&) override {
        ;
    }
    bool isSynchronous() override {
        return true;
    }
    bool good() override {
        return true;
    }
};

class CTestAllocator : public IAllocator {
  public:
    explicit CTestAllocator(CSharedPointer<CBackend> backend) : m_backend(backend) {
        ;
    }
    CSharedPointer<IBuffer> acquire(const SAllocatorBufferParams&, CSharedPointer<CSwapchain>) override {
        return makeShared<CTestBuffer>();
    }
    CSharedPointer<CBackend> getBackend() override {
        return m_backend;
    }
    int drmFD() override {
        return -1;
    }
    eAllocatorType type() override {
        return AQ_ALLOCATOR_TYPE_ANDROID;
    }

  private:
    CSharedPointer<CBackend> m_backend;
};

int main() {
    int                           ret = 0;
    SBackendImplementationOptions backendOptions;
#ifdef __ANDROID__
    backendOptions.backendType = AQ_BACKEND_ANDROID;
#else
    backendOptions.backendType = AQ_BACKEND_NULL;
#endif
    auto              backend = CBackend::create({backendOptions}, SBackendOptions{});
    auto              chain   = CSwapchain::create(makeShared<CTestAllocator>(backend), nullptr);
    SSwapchainOptions options{.length = 1, .size = {64, 64}, .format = DRM_FORMAT_ABGR8888};
    EXPECT(chain->reconfigure(options), true);
    int age = -1;
    chain->next(nullptr); // modeset/test must not consume a frame
    chain->rollback();
    auto first = chain->next(&age);
    EXPECT(age, 0);
    chain->rollback(); // failed first frame must still require a full redraw
    EXPECT(chain->next(&age) == first, true);
    EXPECT(age, 0);
    EXPECT(chain->next(&age) == first, true);
    EXPECT(age, 1);
    chain->rollback();
    chain->next(&age);
    EXPECT(age, 1);
    options.size = {128, 64};
    EXPECT(chain->reconfigure(options), true);
    EXPECT(chain->next(&age) != first, true);
    EXPECT(age, 0);
    chain->next(&age);
    EXPECT(age, 1);
    options.length = 3;
    EXPECT(chain->reconfigure(options), true);
    for (int i = 0; i < 6; ++i) {
        chain->next(&age);
        EXPECT(age, (i < 3 ? 0 : 3));
    }
    options.length = 1;
    EXPECT(chain->reconfigure(options), true);
    chain->next(&age);
    EXPECT(age, 0);
    chain->next(&age);
    EXPECT(age, 1);
    options.length = 0;
    EXPECT(chain->reconfigure(options), true);
    EXPECT(chain->next(&age) == nullptr, true);
    options.length = 1;
    EXPECT(chain->reconfigure(options), true);
    chain->next(&age);
    EXPECT(age, 0);
    return ret;
}
