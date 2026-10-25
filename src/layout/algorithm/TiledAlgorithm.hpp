#pragma once

#include "../../helpers/math/Math.hpp"
#include "../../helpers/memory/Memory.hpp"

#include "ModeAlgorithm.hpp"

#include <optional>
#include <string>

namespace Layout {

    class ITarget;
    class CAlgorithm;

    class ITiledAlgorithm : public IModeAlgorithm {
      public:
        virtual ~ITiledAlgorithm() = default;

        virtual SP<ITarget> getNextCandidate(SP<ITarget> old) = 0;

        // Tabbed layouts may require explicit activation, including for floating
        // windows above the selected tile. Pointer motion must not select a tab.
        virtual bool allowsFocusOnPointerMove() const {
            return true;
        }

        // Slot layouts fit constrained clients rather than floating them.
        virtual bool fitsConstrainedClients() const {
            return false;
        }

        // A desktop shell can keep its fullscreen presentation while the client
        // returns to windowed rendering. Only an explicit shell restore ends it.
        virtual bool ownsFullscreenPresentation() const {
            return false;
        }

        // Optional runtime layout name. Useful for generic adapter classes where
        // typeid alone cannot identify the selected layout instance.
        virtual std::optional<std::string> layoutName() const {
            return std::nullopt;
        }

      protected:
        ITiledAlgorithm() = default;

        friend class Layout::CAlgorithm;
    };
}
