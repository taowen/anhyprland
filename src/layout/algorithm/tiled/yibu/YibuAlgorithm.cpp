#include "YibuAlgorithm.hpp"
#include "../../Algorithm.hpp"
#include "../../../target/Target.hpp"
#include "../../../../desktop/view/window/Window.hpp"
#include "../../../../desktop/view/window/WindowPresentation.hpp"
#include "../../../../desktop/state/FocusState.hpp"
#include "../../../../event/EventBus.hpp"
#include "../../../../output/Monitor.hpp"
#include "../../../../helpers/MiscFunctions.hpp"
#include "../../../../Compositor.hpp"
#include "../../../../protocols/core/DataDevice.hpp"
#include "../../../../render/Renderer.hpp"
#include <algorithm>
#include <cmath>
#include "../../../../render/Texture.hpp"
#include "../../../../render/pass/RectPassElement.hpp"
#include "../../../../render/pass/TexPassElement.hpp"
#include "../../../../config/supplementary/executor/Executor.hpp"

using namespace Layout;
using namespace Layout::Tiled;
static std::vector<CYibuAlgorithm*> layouts;

CYibuAlgorithm::CYibuAlgorithm() {
    layouts.push_back(this);
    m_focus = Event::bus()->m_events.window.active.listen([this](PHLWINDOW window, Desktop::eFocusReason reason) {
        if (!m_arranging && m_parent && window && window->m_workspace == m_parent->space()->workspace()) {
            auto target = window->layoutTarget();
            if (std::ranges::any_of(m_tasks, [&](const auto& t) { return t.lock() == target; }))
                select(target);
        }
    });
}
CYibuAlgorithm::~CYibuAlgorithm() {
    m_focus.reset();
    if (m_hoverTimer)
        wl_event_source_remove(m_hoverTimer);
    for (auto& weak : m_tasks)
        if (auto target = weak.lock(); target && target->window()) {
            target->window()->setInputBlocked(Desktop::View::FOCUS_BLOCK_YIBU_INACTIVE, false);
            *target->window()->presentation().alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = 1.F;
        }
    std::erase(layouts, this);
}
CYibuAlgorithm* CYibuAlgorithm::active() {
    for (auto layout : layouts)
        if (layout->m_parent && layout->m_parent->space()->workspace()->visible())
            return layout;
    return nullptr;
}
int CYibuAlgorithm::slot(SP<ITarget> target) const {
    for (int i = 0; i < 3; ++i)
        if (m_slots[i].lock() == target)
            return i;
    return -1;
}
void CYibuAlgorithm::select(SP<ITarget> target) {
    if (!target || m_main.lock() == target)
        return;
    int index = slot(target);
    if (index >= 0)
        m_slots[index] = m_main;
    m_main = target;
    recalculate();
}
void CYibuAlgorithm::newTarget(SP<ITarget> target) {
    m_tasks.push_back(target);
    m_main = target;
    recalculate();
}
void CYibuAlgorithm::movedTarget(SP<ITarget> target, std::optional<Vector2D>) {
    newTarget(target);
}
void CYibuAlgorithm::removeTarget(SP<ITarget> target) {
    if (auto window = target->window()) {
        window->setInputBlocked(Desktop::View::FOCUS_BLOCK_YIBU_INACTIVE, false);
        *window->presentation().alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = 1.F;
    }
    for (auto& s : m_slots)
        if (s.lock() == target)
            s.reset();
    std::erase_if(m_tasks, [&](auto& t) { return !t || t.lock() == target; });
    if (m_main.lock() == target)
        m_main = getNextCandidate(target);
    recalculate();
}
SP<ITarget> CYibuAlgorithm::getNextCandidate(SP<ITarget> old) {
    for (auto& t : m_tasks)
        if (auto target = t.lock(); target && target != old && slot(target) < 0)
            return target;
    return nullptr;
}
void CYibuAlgorithm::resizeTarget(const Vector2D&, SP<ITarget>, eRectCorner) {}
void CYibuAlgorithm::moveTargetInDirection(SP<ITarget>, Math::eDirection, bool) {}
void CYibuAlgorithm::swapTargets(SP<ITarget> a, SP<ITarget> b) {
    if (m_main.lock() == a)
        select(b);
    else if (m_main.lock() == b)
        select(a);
}
std::optional<Vector2D> CYibuAlgorithm::predictSizeForNewTarget() {
    return m_mainBox.size();
}
void CYibuAlgorithm::recalculate(eRecalculateReason) {
    if (m_arranging || !m_parent || !m_parent->space())
        return;
    m_arranging    = true;
    auto   area    = m_parent->space()->workArea();
    auto   monitor = m_parent->space()->workspace()->m_monitor.lock();
    double scale   = monitor ? monitor->m_scale : 1.;
    double gap = std::max(4., area.w / 200.), rail = area.w / 4.;
    double toolbar = m_enabled ? m_toolbarPixels / scale : 0.;
    m_mainBox      = {area.x + (m_enabled && m_left ? rail + gap : gap), area.y + toolbar + gap, area.w - (m_enabled ? rail : 0.) - 3. * gap, area.h - toolbar - 2. * gap};
    if (!m_enabled)
        m_mainBox = area;
    double cell = (area.h - toolbar - 4. * gap) / 3.;
    for (int i = 0; i < 3; ++i)
        m_boxes[i] = {area.x + (m_left ? gap : area.w - rail), area.y + toolbar + gap + i * (cell + gap), rail - gap, cell};
    for (auto& weak : m_tasks)
        if (auto target = weak.lock(); target && target->window()) {
            auto window  = target->window();
            int  index   = slot(target);
            bool main    = target == m_main.lock();
            bool visible = main || (m_enabled && index >= 0);
            window->setInputBlocked(Desktop::View::FOCUS_BLOCK_YIBU_INACTIVE, !main);
            *window->presentation().alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = visible ? 1.F : 0.F;
            CBox box                                                          = m_mainBox;
            if (!main && index >= 0) {
                auto     cellBox = m_boxes[index];
                double   fit     = std::min(cellBox.w / box.w, (cellBox.h - toolbar * 24. / 78.) / box.h);
                Vector2D size    = box.size() * fit;
                box              = {cellBox.pos() + (Vector2D{cellBox.w, cellBox.h - toolbar * 24. / 78.} - size) / 2., size};
            }
            // Surface rendering follows visual geometry; configure the client at a
            // stable main-task size. Normal layouts leave clientSize unset.
            target->setPositionGlobal(STargetBox{.logicalBox = box, .visualBox = box, .clientSize = m_mainBox.size()});
            target->warpPositionSize();
        }
    m_arranging = false;
    if (monitor)
        g_pHyprRenderer->damageMonitor(monitor);
    controls();
}
void CYibuAlgorithm::controls() {
    m_controls.clear();
    auto   area    = m_parent->space()->workArea();
    auto   monitor = m_parent->space()->workspace()->m_monitor.lock();
    double unit    = m_toolbarPixels / (monitor ? monitor->m_scale : 1.) / 100.;
    auto   add     = [&](CBox box, std::string label, int action, int target = 0) { m_controls.push_back({box, std::move(label), action, target}); };
    if (!m_enabled) {
        add({area.x + area.w - 160 * unit, area.y + 4 * unit, 156 * unit, 38 * unit}, "Back to Yibu", 1);
        return;
    }
    add({area.x + 4 * unit, area.y + 4 * unit, 62 * unit, 38 * unit}, m_left ? "Right >" : "< Left", 5);
    add({area.x + area.w - 112 * unit, area.y + 4 * unit, 108 * unit, 38 * unit}, "Full screen", 0);
    add({area.x + area.w - 194 * unit, area.y + 4 * unit, 78 * unit, 38 * unit}, "Tasks", 7);
    add({area.x + area.w - 276 * unit, area.y + 4 * unit, 78 * unit, 38 * unit}, "Apps", 6);
    int index = 0;
    for (auto& weak : m_tasks) {
        auto task = weak.lock();
        if (!task || !task->window() || index >= 8)
            continue;
        add({area.x + 4 * unit + index++ * 138 * unit, area.y + 52 * unit, 132 * unit, 38 * unit}, task->window()->metadata().title(), 4, task->window()->metadata().stableID());
    }
    for (int i = 0; i < 3; ++i) {
        if (m_slots[i].expired())
            continue;
        auto box = m_boxes[i];
        add({box.x + box.w - 32 * unit, box.y + box.h - 30 * unit, 30 * unit, 28 * unit}, "x", 3, i);
    }
    if (m_taskMenu) {
        index = 0;
        for (auto& weak : m_tasks) {
            auto task = weak.lock();
            if (!task || !task->window())
                continue;
            CBox box{area.x + area.w / 3., area.y + 105 * unit + index++ * 42 * unit, area.w / 3., 38 * unit};
            if (box.y + box.h > area.y + area.h)
                break;
            add(box, task->window()->metadata().title(), 4, task->window()->metadata().stableID());
        }
    }
}

void CYibuAlgorithm::render(PHLMONITOR monitor) {
    if (!m_parent || m_parent->space()->workspace()->m_monitor.lock() != monitor)
        return;
    controls();
    double scale = monitor->m_scale, unit = m_toolbarPixels / scale / 100.;
    auto   pixels = [&](CBox box) {
        box.translate(-monitor->m_position);
        box.scale(scale);
        return box;
    };
    auto rect = [&](CBox box, CHyprColor color) {
        CRectPassElement::SRectData data;
        data.box   = pixels(box);
        data.color = color;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    };
    auto label = [&](const std::string& text, CBox box) {
        if (text.empty())
            return;
        auto        bounds   = pixels(box);
        int         fontSize = std::max(12, static_cast<int>(13 * unit * scale));
        std::string key      = text + std::to_string(static_cast<int>(bounds.w)) + ":" + std::to_string(fontSize);
        if (!m_textures.contains(key)) {
            if (m_textures.size() > 64)
                m_textures.clear();
            m_textures[key] =
                g_pHyprRenderer->renderText(text, CHyprColor{0.9F, 0.95F, 1.F, 1.F}, fontSize, false, "sans", std::max(1, static_cast<int>(bounds.w - 16 * unit * scale)), 500);
        }
        auto texture = m_textures[key];
        if (!texture)
            return;
        CTexPassElement::SRenderData data;
        data.tex = texture;
        data.box = {bounds.x + 8 * unit * scale, bounds.y + (bounds.h - texture->m_size.y) / 2., texture->m_size.x, texture->m_size.y};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    };
    auto area = m_parent->space()->workArea();
    if (m_enabled) {
        rect({area.x, area.y, area.w, m_toolbarPixels / scale}, CHyprColor{0.11F, 0.14F, 0.18F, 1.F});
        if (auto main = m_main.lock())
            label(main->window()->metadata().title(), {area.x + 74 * unit, area.y + 4 * unit, area.w - 360 * unit, 38 * unit});
        for (int i = 0; i < 3; ++i) {
            auto box = m_boxes[i];
            if (auto task = m_slots[i].lock()) {
                box.y += box.h - 30 * unit;
                box.h = 30 * unit;
                box.w -= 32 * unit;
                rect(box, CHyprColor{0.15F, 0.19F, 0.24F, 1.F});
                label(task->window()->metadata().title(), box);
            } else {
                rect(box, CHyprColor{0.15F, 0.19F, 0.24F, 1.F});
                label("+ Pin main task", box);
            }
        }
    }
    for (const auto& control : m_controls) {
        rect(control.box, CHyprColor{0.18F, 0.24F, 0.3F, 1.F});
        label(control.label, control.box);
    }
}

void CYibuAlgorithm::action(int action, int target, int toolbarPixels) {
    if (PROTO::data && PROTO::data->dndActive())
        return;
    if (action == 6) {
        Config::Supplementary::executor()->spawn("python3 /usr/lib/arlinux/guest/yibu-launcher.py");
        return;
    }
    if (action == 7) {
        m_taskMenu = !m_taskMenu;
        recalculate();
        return;
    }
    m_taskMenu = false;
    if (action == 0 || action == 1) {
        m_enabled = action == 1;
        if (toolbarPixels > 0)
            m_toolbarPixels = toolbarPixels;
    } else if (action == 5)
        m_left = !m_left;
    else if (action == 2 && target >= 0 && target < 3) {
        if (auto task = m_slots[target].lock())
            select(task);
        else {
            m_slots[target] = m_main;
            m_main          = getNextCandidate(m_main.lock());
        }
    } else if (action == 3 && target >= 0 && target < 3)
        m_slots[target].reset();
    else if (action == 4) {
        for (auto& weak : m_tasks)
            if (auto task = weak.lock(); task && task->window()->metadata().stableID() == static_cast<uint64_t>(target)) {
                select(task);
                break;
            }
    }
    recalculate();
    if (auto task = m_main.lock())
        Desktop::focusState()->fullWindowFocus(task->window(), Desktop::FOCUS_REASON_CLICK);
}
bool CYibuAlgorithm::pointer(double x, double y, uint32_t button, bool pressed) {
    if (!m_parent)
        return false;
    auto monitor = m_parent->space()->workspace()->m_monitor.lock();
    if (!monitor)
        return false;
    Vector2D point = Vector2D{x, y} / monitor->m_scale + monitor->m_position;
    if (!(PROTO::data && PROTO::data->dndActive())) {
        auto control = std::ranges::find_if(m_controls, [&](const auto& item) { return item.box.containsPoint(point); });
        if (button == 0x110 && pressed && control != m_controls.end()) {
            m_pressedControl = *control;
            return true;
        }
        if (button == 0x110 && !pressed && m_pressedControl) {
            auto previous = *m_pressedControl;
            m_pressedControl.reset();
            if (previous.box.containsPoint(point))
                action(previous.action, previous.target, 0);
            return true;
        }
        if (m_pressedControl || control != m_controls.end())
            return true;
    }
    if (!m_enabled)
        return false;
    int hit = -1;
    for (int i = 0; i < 3; ++i)
        if (m_boxes[i].containsPoint(point))
            hit = i;
    if (PROTO::data && PROTO::data->dndActive()) {
        if (hit == m_promotedSlot)
            return false;
        m_promotedSlot = -1;
        if (hit != m_hoverSlot) {
            m_hoverSlot = hit;
            if (!m_hoverTimer)
                m_hoverTimer = wl_event_loop_add_timer(
                    g_pCompositor->m_wlEventLoop,
                    [](void* data) {
                        auto self = static_cast<CYibuAlgorithm*>(data);
                        if (PROTO::data->dndActive() && self->m_hoverSlot >= 0) {
                            self->select(self->m_slots[self->m_hoverSlot].lock());
                            self->m_promotedSlot = self->m_hoverSlot;
                            self->m_hoverSlot    = -1;
                        }
                        return 0;
                    },
                    this);
            if (m_hoverTimer)
                wl_event_source_timer_update(m_hoverTimer, hit >= 0 ? 600 : 0);
        }
        return false;
    }
    m_hoverSlot = m_promotedSlot = -1;
    if (button == 0x110) {
        if (pressed && hit >= 0) {
            m_pressedSlot = hit;
            return true;
        }
        if (!pressed && m_pressedSlot >= 0) {
            int previous  = m_pressedSlot;
            m_pressedSlot = -1;
            if (previous == hit)
                action(2, hit, 0);
            return true;
        }
    }
    return false;
}
