#include "WindowState.hpp"
#include "../../event/EventBus.hpp"
#include "../../render/Renderer.hpp"
#include "../view/window/Window.hpp"
#include "../view/window/WindowFullscreenPolicy.hpp"
#include "../view/window/WindowPresentation.hpp"

#include <algorithm>
#include <ranges>

using namespace Desktop;

CWindowState::CWindowState() {
    m_listeners.viewCreate = Event::bus()->m_events.view.create.listen([this](PHLVIEW view) {
        const auto WINDOW = View::CWindow::fromView(view);
        if (!WINDOW)
            return;

        m_windows.emplace_back(WINDOW);
    });
}

void CWindowState::removeSafe(PHLWINDOW w) {
    std::erase_if(m_windows, [&w](auto& el) { return !el || el == w; });
}

const std::vector<PHLWINDOW>& CWindowState::windows() const {
    return m_windows;
}

CWindowQuery CWindowState::query() const {
    return CWindowQuery{*this};
}

bool CWindowState::groupsLocked() const {
    return m_groupsLocked;
}

void CWindowState::setGroupsLocked(bool locked) {
    m_groupsLocked = locked;
}

void CWindowState::raise(PHLWINDOW w) {
    moveToZ(w, true);
}

void CWindowState::lower(PHLWINDOW w) {
    moveToZ(w, false);
}

void CWindowState::moveToZ(PHLWINDOW w, bool top) {
    if (!View::validMapped(w) || m_windows.empty())
        return;

    if (top && w->backend().traits().belowParent) {
        if (const auto parent = w->backend().parent())
            w = parent;
    }

    w->fullscreenPolicy().setAllowedOverFullscreen(top);
    w->updateFullscreenInputState();
    *w->presentation().alpha(View::WINDOW_ALPHA_FULLSCREEN) = w->isBlockedByFullscreen() ? 0.F : 1.F;

    auto moveSingleToZ = [&](PHLWINDOW pw) -> void {
        if (top)
            moveToTop(pw);
        else
            moveToBottom(pw);

        if (pw->mapped())
            g_pHyprRenderer->damageMonitor(pw->m_monitor.lock());
    };

    // Both X11 transients and xdg_toplevel parents define a stacking family.
    // Raising an owner must not cover the dialog that has disabled it.
    std::vector<PHLWINDOW> toMove;

    std::vector<PHLWINDOW> visited;
    auto collectStack = [&](PHLWINDOW pw, auto&& collectStack) -> void {
        if (std::ranges::find(visited, pw) != visited.end())
            return;
        visited.emplace_back(pw);
        auto collectChildren = [&](bool below) {
            for (auto const& other : m_windows) {
                if (other->mapped() && !other->isHidden() && other->backend().parent() == pw && other != pw &&
                    other->backend().traits().belowParent == below)
                    collectStack(other, collectStack);
            }
        };
        collectChildren(true);
        toMove.emplace_back(pw);
        collectChildren(false);
    };

    collectStack(w, collectStack);
    if (!top)
        std::ranges::reverse(toMove);
    for (auto const& it : toMove) {
        moveSingleToZ(it);
    }
}

void CWindowState::moveToTop(PHLWINDOW w) {
    if (!w || m_windows.empty() || m_windows.back() == w)
        return;

    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (*it != w)
            continue;

        std::rotate(it, it + 1, m_windows.end());
        return;
    }
}

void CWindowState::moveToBottom(PHLWINDOW w) {
    if (!w || m_windows.empty() || m_windows.front() == w)
        return;

    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
        if (*it != w)
            continue;

        std::rotate(it, it + 1, m_windows.rend());
        return;
    }
}

void CWindowState::clear() {
    m_windows.clear();
}

UP<CWindowState>& Desktop::windowState() {
    static UP<CWindowState> state = makeUnique<CWindowState>();
    return state;
}
