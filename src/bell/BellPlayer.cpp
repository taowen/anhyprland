#include "BellPlayer.hpp"
#include "impl/Impl.hpp"

#ifndef __ANDROID__
#include "impl/Canberra.hpp"
#endif

using namespace Bell;

UP<CBellPlayer>& Bell::player() {
    static auto p = makeUnique<CBellPlayer>();
    return p;
}

#ifdef __ANDROID__
// Audio bells are unavailable until the embedding host provides a bell backend.
CBellPlayer::CBellPlayer() {
    ;
}
#else
CBellPlayer::CBellPlayer() : m_impl(makeUnique<CCanberraImpl>()) {
    ;
}

#endif

void CBellPlayer::play() const {
    if (m_impl)
        m_impl->play();
}
