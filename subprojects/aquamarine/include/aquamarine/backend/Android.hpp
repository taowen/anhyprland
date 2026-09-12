#pragma once

#include "Backend.hpp"
#include "../output/Output.hpp"
#include <hyprutils/os/FileDescriptor.hpp>
#include <android/native_window.h>

namespace Aquamarine {
    class CAndroidBackend;

    class CAndroidOutput : public IOutput {
      public:
        explicit CAndroidOutput(Hyprutils::Memory::CWeakPointer<CAndroidBackend> backend);
        bool commit() override;
        bool test() override;
        Hyprutils::Memory::CSharedPointer<IBackendImplementation> getBackend() override;
        std::vector<SDRMFormat> getRenderFormats() override;
        void scheduleFrame(scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN) override;
        bool pendingPageFlip() override;
        bool pendingIdleFrame() override;

      private:
        Hyprutils::Memory::CWeakPointer<CAndroidBackend> m_backend;
    };

    class CAndroidKeyboard : public IKeyboard {
      public:
        const std::string& getName() override;
      private:
        const std::string m_name = "Android keyboard";
    };

    class CAndroidPointer : public IPointer {
      public:
        const std::string& getName() override;
      private:
        const std::string m_name = "Android pointer";
    };

    // Every method runs on the compositor thread. The embedding API queues
    // Android UI events onto that thread before calling this backend.
    class CAndroidBackend : public IBackendImplementation {
      public:
        CAndroidBackend(Hyprutils::Memory::CWeakPointer<CBackend> backend, ANativeWindow* window, int width, int height);
        ~CAndroidBackend() override;
        eBackendType type() override;
        bool start() override;
        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> pollFDs() override;
        int drmFD() override;
        int drmRenderNodeFD() override;
        bool dispatchEvents() override;
        uint32_t capabilities() override;
        void onReady() override;
        std::vector<SDRMFormat> getRenderFormats() override;
        std::vector<SDRMFormat> getCursorFormats() override;
        bool createOutput(const std::string& name = "") override;
        Hyprutils::Memory::CSharedPointer<IAllocator> preferredAllocator() override;
        std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>> getAllocators() override;
        Hyprutils::Memory::CWeakPointer<IBackendImplementation> getPrimary() override;

        bool setWindow(ANativeWindow* window, int width, int height);
        ANativeWindow* window() const;
        void scheduleFrame();
        bool frameScheduled() const;
        void pointer(float x, float y, uint32_t button, bool pressed);
        void axis(float dx, float dy);
        void key(uint32_t evdev, bool pressed);

        Hyprutils::Memory::CWeakPointer<CAndroidBackend> self;
        std::function<bool(ANativeWindow*)> attachWindow;
        std::function<bool(Hyprutils::Memory::CSharedPointer<IBuffer>)> present;

      private:
        Hyprutils::Memory::CWeakPointer<CBackend> m_backend;
        Hyprutils::Memory::CSharedPointer<CAndroidOutput> m_output;
        Hyprutils::Memory::CSharedPointer<CAndroidKeyboard> m_keyboard;
        Hyprutils::Memory::CSharedPointer<CAndroidPointer> m_pointer;
        Hyprutils::Memory::CSharedPointer<IAllocator> m_allocator;
        Hyprutils::OS::CFileDescriptor m_timer;
        ANativeWindow* m_window = nullptr;
        int m_width = 0, m_height = 0;
        bool m_frameScheduled = false;
        std::chrono::steady_clock::time_point m_lastFrame;
    };
}
