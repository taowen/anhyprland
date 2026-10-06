#pragma once

#include "../../TiledAlgorithm.hpp"
#include "../../../../helpers/signal/Signal.hpp"
#include <array>
#include <vector>
#include <unordered_map>
namespace Render {
    class ITexture;
}
class CHyprColor;
struct wl_event_source;

namespace Layout::Tiled {
    // Fixed task slots, not a recursive split tree. Auxiliary tasks retain the
    // main client's rendering size and are fitted into their display slots.
    class CYibuAlgorithm : public ITiledAlgorithm {
      public:
        CYibuAlgorithm();
        ~CYibuAlgorithm() override;
        void                    newTarget(SP<ITarget>) override;
        void                    movedTarget(SP<ITarget>, std::optional<Vector2D> = std::nullopt) override;
        void                    removeTarget(SP<ITarget>) override;
        void                    resizeTarget(const Vector2D&, SP<ITarget>, eRectCorner = CORNER_NONE) override;
        void                    recalculate(eRecalculateReason = RECALCULATE_REASON_UNKNOWN) override;
        void                    swapTargets(SP<ITarget>, SP<ITarget>) override;
        void                    moveTargetInDirection(SP<ITarget>, Math::eDirection, bool) override;
        SP<ITarget>             getNextCandidate(SP<ITarget>) override;
        std::optional<Vector2D> predictSizeForNewTarget() override;
        void                    action(int action, int target, int toolbarPixels);
        bool                    pointer(double x, double y, uint32_t button, bool pressed);
        static CYibuAlgorithm*  active();
        void                    render(PHLMONITOR monitor);

      private:
        enum eControlStyle : uint8_t {
            CONTROL_BUTTON,
            CONTROL_PRIMARY,
            CONTROL_TASK,
            CONTROL_CLOSE,
            CONTROL_ROW,
            CONTROL_FLOAT,
        };
        std::vector<WP<ITarget>>   m_tasks, m_recent; // open order (task strip), use order (oldest first)
        WP<ITarget>                m_main;
        std::array<WP<ITarget>, 3> m_slots;
        std::array<CBox, 3>        m_boxes;
        std::array<CBox, 3>        m_fitted;
        CBox                       m_mainBox, m_menuBox;
        bool                       m_enabled = true, m_left = false, m_arranging = false;
        int                        m_toolbarPixels = 200, m_pressedSlot = -1, m_hoverControl = -1;
        struct SControl {
            CBox          box;
            std::string   label, app;
            int           action = 0, target = 0;
            eControlStyle style  = CONTROL_BUTTON;
            bool          active = false;
        };
        std::vector<SControl>                                 m_controls;
        std::optional<SControl>                               m_pressedControl;
        bool                                                  m_taskMenu = false;
        std::unordered_map<std::string, SP<Render::ITexture>> m_textures;
        int                                                   m_hoverSlot = -1, m_promotedSlot = -1;
        wl_event_source*                                      m_hoverTimer = nullptr;
        CHyprSignalListener                                   m_focus;
        void                                                  select(SP<ITarget>);
        void                                                  used(SP<ITarget>);
        int                                                   slot(SP<ITarget>) const;
        void                                                  controls();
        double                                                unit() const;
        SP<Render::ITexture>                                  text(const std::string& label, const CHyprColor& color, int pixels, int weight, int maxWidth);
    };
}
