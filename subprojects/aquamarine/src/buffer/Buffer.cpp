#include <aquamarine/buffer/Buffer.hpp>
#include "Shared.hpp"

using namespace Aquamarine;

SDMABUFAttrs Aquamarine::IBuffer::dmabuf() {
    return SDMABUFAttrs{};
}

SSHMAttrs Aquamarine::IBuffer::shm() {
    return SSHMAttrs{};
}

std::tuple<uint8_t*, uint32_t, size_t> Aquamarine::IBuffer::beginDataPtr(uint32_t flags) {
    return {nullptr, 0, 0};
}

void Aquamarine::IBuffer::endDataPtr() {
    ; // empty
}

void Aquamarine::IBuffer::sendRelease() {
    ;
}

void Aquamarine::IBuffer::lock() {
    locks++;
}

void Aquamarine::IBuffer::unlock() {
    locks--;

    ASSERT(locks >= 0);

    if (locks <= 0)
        sendRelease();
}

bool Aquamarine::IBuffer::locked() {
    return locks;
}

void Aquamarine::IBuffer::backendPin() {
    backendPins++;
    lockedByBackend = true;
}

void Aquamarine::IBuffer::backendUnpin() {
    ASSERT(backendPins > 0);

    if (backendPins == 0)
        return;

    backendPins--;
    if (backendPins > 0)
        return;

    lockedByBackend = false;
    events.backendRelease.emit();
}

uint32_t Aquamarine::IBuffer::backendPinCount() const {
    return backendPins;
}
