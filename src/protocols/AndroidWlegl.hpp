#pragma once
#ifdef __ANDROID__
#include "WaylandProtocol.hpp"

class CAndroidWleglProtocol : public IWaylandProtocol {
  public:
    CAndroidWleglProtocol();
    ~CAndroidWleglProtocol() override;
    void bindManager(wl_client* client, void* data, uint32_t version, uint32_t id) override;

  private:
    struct SState;
    UP<SState> m_state;
};

namespace PROTO {
    inline UP<CAndroidWleglProtocol> androidWlegl;
}
#endif
