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
#include <cctype>
#include <format>
#include <ranges>
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
    if (m_controlsIdleTimer)
        wl_event_source_remove(m_controlsIdleTimer);
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
    used(target);
    recalculate();
}
void CYibuAlgorithm::used(SP<ITarget> target) {
    std::erase_if(m_recent, [&](auto& t) { return !t || t.lock() == target; });
    m_recent.push_back(target);
}
void CYibuAlgorithm::newTarget(SP<ITarget> target) {
    m_tasks.push_back(target);
    m_main = target;
    used(target);
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
    std::erase_if(m_recent, [&](auto& t) { return !t || t.lock() == target; });
    // Automatic replacement must not consume a pinned slot. In particular,
    // Apps closes before the launched application's window is ready.
    if (m_main.lock() == target)
        m_main = getNextCandidate(target);
    recalculate();
}
// The most recently used task that is neither leaving the main area nor pinned.
SP<ITarget> CYibuAlgorithm::getNextCandidate(SP<ITarget> old) {
    for (auto& t : m_recent | std::views::reverse)
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
// Toolbar, slot header and control sizes are in toolbar units (1/100 of the
// host-provided toolbar reference), so the chrome follows the display density.
static constexpr double BAR = 56., HEADER = 34.;
static const CHyprColor COLOR_BAR{0.075F, 0.086F, 0.106F, 1.F}, COLOR_LINE{1.F, 1.F, 1.F, 0.07F}, COLOR_BUTTON{0.16F, 0.18F, 0.22F, 1.F}, COLOR_HOVER{0.23F, 0.26F, 0.31F, 1.F},
    COLOR_PRIMARY{0.43F, 0.87F, 0.75F, 1.F}, COLOR_TEXT{0.92F, 0.95F, 0.98F, 1.F}, COLOR_MUTED{0.62F, 0.68F, 0.75F, 1.F}, COLOR_INK{0.03F, 0.06F, 0.09F, 1.F},
    COLOR_ACTIVE{0.96F, 0.97F, 0.98F, 1.F}, COLOR_SLOT{0.105F, 0.12F, 0.145F, 1.F}, COLOR_EMPTY_LINE{0.43F, 0.87F, 0.75F, 0.35F}, COLOR_MENU{0.09F, 0.1F, 0.125F, 0.98F};

double CYibuAlgorithm::unit() const {
    auto monitor = m_parent ? m_parent->space()->workspace()->m_monitor.lock() : nullptr;
    return m_toolbarPixels / (monitor ? monitor->m_scale : 1.) / 100.;
}

void CYibuAlgorithm::recalculate(eRecalculateReason) {
    if (m_arranging || !m_parent || !m_parent->space())
        return;
    m_arranging    = true;
    auto   area    = m_parent->space()->workArea();
    auto   monitor = m_parent->space()->workspace()->m_monitor.lock();
    double u = unit(), gap = std::max(4., area.w / 200.), rail = area.w / 4.;
    double toolbar = m_enabled ? BAR * u : 0.;
    m_mainBox      = {area.x + (m_enabled && m_left ? rail + gap : gap), area.y + toolbar + gap, area.w - (m_enabled ? rail : 0.) - 3. * gap, area.h - toolbar - 2. * gap};
    if (!m_enabled)
        m_mainBox = area;
    double cell = (area.h - toolbar - 4. * gap) / 3.;
    for (int i = 0; i < 3; ++i)
        m_boxes[i] = {area.x + (m_left ? gap : area.w - rail), area.y + toolbar + gap + i * (cell + gap), rail - gap, cell};
    for (auto& weak : m_tasks) {
        auto target = weak.lock();
        if (!target || !target->window())
            continue;
        auto window  = target->window();
        int  index   = slot(target);
        bool main    = target == m_main.lock();
        bool visible = main || (m_enabled && index >= 0);
        window->setInputBlocked(Desktop::View::FOCUS_BLOCK_YIBU_INACTIVE, !main);
        *window->presentation().alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = visible ? 1.F : 0.F;
        CBox box                                                          = m_mainBox;
        if (!main && index >= 0) {
            // Live task below the card header, scaled to fit without reflowing the client.
            CBox     content{m_boxes[index].x, m_boxes[index].y + HEADER * u, m_boxes[index].w, m_boxes[index].h - HEADER * u};
            double   fit    = std::min(content.w / box.w, content.h / box.h);
            Vector2D size   = box.size() * fit;
            box             = {content.pos() + (content.size() - size) / 2., size};
            m_fitted[index] = box;
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
    focusMain();
}

// Blocking a focused task drops keyboard focus, and unblocking it later does not
// restore it (e.g. OpenCode becomes main again after its first window closes).
// Give the main task focus whenever nothing else that may hold it is focused.
void CYibuAlgorithm::focusMain() {
    auto main = m_main.lock();
    if (!main || !main->window() || !m_parent || !m_parent->space()->workspace()->visible())
        return;
    auto focused = Desktop::focusState()->window();
    if (focused == main->window())
        return;
    if (focused && !std::ranges::any_of(m_tasks, [&](const auto& t) { return t.lock() && t.lock()->window() == focused; }))
        return;         // a popup, dialog or other window outside the task slots keeps focus
    m_arranging = true; // the focus listener must not re-enter select()/recalculate()
    Desktop::focusState()->fullWindowFocus(main->window(), Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_SOFT);
    m_arranging = false;
}

void CYibuAlgorithm::controls() {
    m_controls.clear();
    m_menuBox   = {};
    auto   area = m_parent->space()->workArea();
    double u    = unit();
    auto   add  = [&](CBox box, std::string label, int action, int target, eControlStyle style, bool active = false, std::string app = "") {
        m_controls.push_back({box, std::move(label), std::move(app), action, target, style, active});
    };
    auto main     = m_main.lock();
    bool canClose = main && main->window();
    if (!m_enabled) {
        double right = area.x + area.w - 10 * u;
        if (m_fullscreenControlsVisible)
            add({right - 40 * u, area.y + 10 * u, 40 * u, 40 * u}, "", 1, 0, CONTROL_FLOAT);
        return;
    }
    std::vector<SP<ITarget>> live;
    for (auto& weak : m_tasks)
        if (auto task = weak.lock(); task && task->window())
            live.push_back(task);
    double y = area.y + 8 * u, h = 40 * u, right = area.x + area.w - 10 * u;
    if (canClose) {
        add({right - 40 * u, y, 40 * u, h}, "", 8, main->window()->metadata().stableID(), CONTROL_CLOSE);
        right -= 48 * u;
        add({right - 40 * u, y, 40 * u, h}, "", 0, 0, CONTROL_BUTTON);
        right -= 48 * u;
    }
    CBox tasks{right - 98 * u, y, 98 * u, h};
    CBox apps{tasks.x - 104 * u, y, 94 * u, h};
    add({area.x + 10 * u, y, 112 * u, h}, m_left ? "Sidebar ›" : "‹ Sidebar", 5, 0, CONTROL_BUTTON);
    add(apps, "+ Apps", 6, 0, CONTROL_PRIMARY);
    add(tasks, "Tasks " + std::to_string(live.size()), 7, 0, CONTROL_BUTTON, m_taskMenu);
    // Running tasks as a strip of chips; those that do not fit stay in the Tasks panel.
    double x0 = area.x + 134 * u, x1 = apps.x - 14 * u;
    double w = live.empty() ? 0. : std::clamp((x1 - x0) / live.size(), 120. * u, 210. * u);
    for (size_t i = 0; i < live.size() && x0 + (i + 1) * w <= x1 + 1; ++i)
        add({x0 + i * w, y, w - 8 * u, h}, live[i]->window()->metadata().title(), 4, live[i]->window()->metadata().stableID(), CONTROL_TASK, live[i] == m_main.lock(),
            live[i]->window()->metadata().appID());
    for (int i = 0; i < 3; ++i) {
        if (m_slots[i].expired())
            continue;
        auto box = m_boxes[i];
        add({box.x + box.w - 64 * u, box.y + 4 * u, 60 * u, 26 * u}, "Unpin", 3, i, CONTROL_BUTTON);
    }
    if (!m_taskMenu)
        return;
    int rows  = std::min<int>(live.size(), std::floor((area.h - BAR * u - 40 * u) / (48 * u)));
    m_menuBox = {tasks.x + tasks.w - 400 * u, area.y + BAR * u + 6 * u, 400 * u, 16 * u + std::max(1, rows) * 48 * u};
    for (int i = 0; i < rows; ++i)
        add({m_menuBox.x + 8 * u, m_menuBox.y + 8 * u + i * 48 * u, m_menuBox.w - 16 * u, 44 * u}, live[i]->window()->metadata().title(), 4,
            live[i]->window()->metadata().stableID(), CONTROL_ROW, live[i] == m_main.lock(), live[i]->window()->metadata().appID());
}

SP<Render::ITexture> CYibuAlgorithm::text(const std::string& label, const CHyprColor& color, int pixels, int weight, int maxWidth) {
    std::string key = std::format("{}|{}|{}|{}|{:x}", label, pixels, weight, maxWidth, color.getAsHex());
    if (auto it = m_textures.find(key); it != m_textures.end())
        return it->second;
    if (m_textures.size() > 96)
        m_textures.clear();
    return m_textures[key] = g_pHyprRenderer->renderText(label, color, pixels, false, "sans", std::max(1, maxWidth), weight);
}

// A coloured initial of the application name stands in for its icon in compositor
// chrome. The name comes from the app ID (org.xfce.mousepad, io.taowen.arlinux.app.
// com.android.calendar), falling back to the window title.
static std::string appName(const std::string& appID, const std::string& title) {
    std::string name;
    for (size_t end = appID.size(); end > 0 && name.empty();) {
        size_t start = appID.rfind('.', end - 1);
        start        = start == std::string::npos ? 0 : start + 1;
        auto part    = appID.substr(start, end - start);
        if (part != "desktop" && part != "app" && part != "android" && part != "main")
            name = part;
        end = start ? start - 1 : 0;
    }
    return name.empty() ? title : name;
}

static std::string initial(const std::string& name) {
    if (name.empty())
        return "?";
    auto   lead   = sc<unsigned char>(name[0]);
    size_t length = lead < 0x80 ? 1 : lead >> 5 == 6 ? 2 : lead >> 4 == 14 ? 3 : 4;
    auto   first  = name.substr(0, length);
    if (length == 1)
        first[0] = sc<char>(std::toupper(lead));
    return first;
}

static CHyprColor badgeColor(const std::string& title) {
    static const std::array<CHyprColor, 6> palette = {
        CHyprColor{0.36F, 0.55F, 0.98F, 1.F}, CHyprColor{0.96F, 0.55F, 0.25F, 1.F}, CHyprColor{0.43F, 0.87F, 0.75F, 1.F},
        CHyprColor{0.86F, 0.38F, 0.55F, 1.F}, CHyprColor{0.62F, 0.48F, 0.95F, 1.F}, CHyprColor{0.95F, 0.78F, 0.3F, 1.F},
    };
    return palette[std::hash<std::string>{}(title) % palette.size()];
}

void CYibuAlgorithm::render(PHLMONITOR monitor, bool background) {
    if (!m_parent || m_parent->space()->workspace()->m_monitor.lock() != monitor)
        return;
    controls();
    double scale = monitor->m_scale, u = unit();
    auto   pixels = [&](CBox box) {
        box.translate(-monitor->m_position);
        box.scale(scale);
        return box;
    };
    auto rect = [&](CBox box, const CHyprColor& color, double round = 0.) {
        if (box.w <= 0 || box.h <= 0)
            return;
        CRectPassElement::SRectData data;
        data.box   = pixels(box);
        data.color = color;
        data.round = sc<int>(round * scale);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    };
    // centred labels, or left-aligned with an inset (both in toolbar units)
    auto label = [&](const std::string& value, CBox box, const CHyprColor& color, double size, int weight, bool centred, double inset = 12.) {
        if (value.empty())
            return;
        auto bounds  = pixels(box);
        auto texture = text(value, color, std::max(10, sc<int>(size * u * scale)), weight, sc<int>(bounds.w - (centred ? 8. : inset * 2.) * u * scale));
        if (!texture)
            return;
        CTexPassElement::SRenderData data;
        data.tex = texture;
        double x = centred ? bounds.x + (bounds.w - texture->m_size.x) / 2. : bounds.x + inset * u * scale;
        data.box = {std::round(x), std::round(bounds.y + (bounds.h - texture->m_size.y) / 2.), texture->m_size.x, texture->m_size.y};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    };
    auto badge = [&](const std::string& appID, const std::string& title, CBox box) {
        auto name = appName(appID, title);
        rect(box, badgeColor(name), box.h / 2.);
        label(initial(name), box, COLOR_INK, 15, 700, true);
    };
    // Simple compositor-native icons: no icon font or theme dependency.
    auto icon = [&](int action, CBox box) {
        if (action == 8) {
            label("×", box, COLOR_TEXT, 26, 400, true);
            return;
        }
        double x = box.x + (box.w - 20 * u) / 2., y = box.y + (box.h - 20 * u) / 2.;
        double size = 20 * u, stroke = 2 * u;
        if (action == 0) {
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 2; ++col) {
                    rect({x + col * (size - 7 * u), y + row * (size - stroke), 7 * u, stroke}, COLOR_TEXT);
                    rect({x + col * (size - stroke), y + row * (size - 7 * u), stroke, 7 * u}, COLOR_TEXT);
                }
        } else {
            rect({x, y, size, stroke}, COLOR_TEXT);
            rect({x, y + size - stroke, size, stroke}, COLOR_TEXT);
            rect({x, y, stroke, size}, COLOR_TEXT);
            rect({x + size - stroke, y, stroke, size}, COLOR_TEXT);
            rect({x + 12 * u, y, stroke, size}, COLOR_TEXT);
            for (int row = 1; row < 3; ++row)
                rect({x + 12 * u, y + row * size / 3., 8 * u, stroke}, COLOR_TEXT);
        }
    };
    auto area = m_parent->space()->workArea();
    // The empty desktop is wallpaper, not an overlay over unmanaged X11
    // windows (splash screens, menus and native presentation surfaces).
    if (background) {
        if (m_enabled && !m_main.lock()) {
            auto box = m_mainBox;
            rect(box, COLOR_SLOT, 16 * u);
            label("No task in the main area", {box.x, box.y + box.h / 2. - 44 * u, box.w, 40 * u}, COLOR_TEXT, 22, 600, true);
            label("Open one with + Apps, or click a pinned task to bring it here", {box.x, box.y + box.h / 2. + 4 * u, box.w, 30 * u}, COLOR_MUTED, 15, 500, true);
        }
        return;
    }
    if (m_enabled) {
        rect({area.x, area.y, area.w, BAR * u}, COLOR_BAR);
        rect({area.x, area.y + BAR * u - 1. / scale, area.w, 1. / scale}, COLOR_LINE);
        for (int i = 0; i < 3; ++i) {
            auto box  = m_boxes[i];
            auto task = m_slots[i].lock();
            if (!task || !task->window()) {
                // Empty slot: a quiet card that explains what a click does.
                rect(box, COLOR_EMPTY_LINE, 14 * u);
                rect({box.x + 2. / scale, box.y + 2. / scale, box.w - 4. / scale, box.h - 4. / scale}, COLOR_SLOT, 13 * u);
                label("+", {box.x, box.y + box.h / 2. - 40 * u, box.w, 44 * u}, COLOR_PRIMARY, 34, 400, true);
                label("Pin the current task", {box.x, box.y + box.h / 2. + 8 * u, box.w, 26 * u}, COLOR_MUTED, 14, 500, true);
                continue;
            }
            // Occupied slot: header with the task name; letterbox around the live preview.
            CBox content{box.x, box.y + HEADER * u, box.w, box.h - HEADER * u}, fit = m_fitted[i];
            rect({content.x, content.y, fit.x - content.x, content.h}, COLOR_SLOT);
            rect({fit.x + fit.w, content.y, content.x + content.w - fit.x - fit.w, content.h}, COLOR_SLOT);
            rect({content.x, content.y, content.w, fit.y - content.y}, COLOR_SLOT);
            rect({content.x, fit.y + fit.h, content.w, content.y + content.h - fit.y - fit.h}, COLOR_SLOT);
            rect({box.x, box.y, box.w, HEADER * u}, COLOR_BUTTON, 10 * u);
            auto title = task->window()->metadata().title();
            badge(task->window()->metadata().appID(), title, {box.x + 6 * u, box.y + 5 * u, 24 * u, 24 * u});
            label(title, {box.x + 30 * u, box.y, box.w - 98 * u, HEADER * u}, COLOR_TEXT, 13, 500, false, 8);
        }
        if (m_taskMenu) {
            rect({m_menuBox.x - 1, m_menuBox.y - 1, m_menuBox.w + 2, m_menuBox.h + 2}, COLOR_LINE, 17 * u);
            rect(m_menuBox, COLOR_MENU, 16 * u);
        }
    }
    for (size_t i = 0; i < m_controls.size(); ++i) {
        const auto& control = m_controls[i];
        bool        hover   = sc<int>(i) == m_hoverControl;
        bool        pressed = m_pressedControl && m_pressedControl->box == control.box;
        auto        box     = control.box;
        switch (control.style) {
            case CONTROL_PRIMARY:
                rect(box, pressed ? CHyprColor{0.33F, 0.72F, 0.62F, 1.F} : hover ? CHyprColor{0.55F, 0.93F, 0.82F, 1.F} : COLOR_PRIMARY, box.h / 2.);
                label(control.label, box, COLOR_INK, 15, 700, true);
                break;
            case CONTROL_TASK:
            case CONTROL_ROW: {
                auto fill = control.active ? COLOR_ACTIVE : hover ? COLOR_HOVER : control.style == CONTROL_ROW ? CHyprColor{0.F, 0.F, 0.F, 0.F} : COLOR_BUTTON;
                rect(box, fill, control.style == CONTROL_TASK ? box.h / 2. : 10 * u);
                badge(control.app, control.label, {box.x + 6 * u, box.y + (box.h - 28 * u) / 2., 28 * u, 28 * u});
                label(control.label, {box.x + 30 * u, box.y, box.w - 34 * u, box.h}, control.active ? COLOR_INK : COLOR_TEXT, 14, control.active ? 700 : 500, false, 8);
                break;
            }
            case CONTROL_CLOSE:
                rect(box, hover || pressed ? CHyprColor{0.85F, 0.33F, 0.33F, 1.F} : COLOR_HOVER, box.h / 2.);
                icon(control.action, box);
                break;
            case CONTROL_FLOAT:
                rect(box, hover ? CHyprColor{0.F, 0.F, 0.F, 0.85F} : CHyprColor{0.F, 0.F, 0.F, 0.6F}, box.h / 2.);
                icon(control.action, box);
                break;
            case CONTROL_BUTTON:
                rect(box, control.active || pressed ? COLOR_HOVER : hover ? CHyprColor{0.2F, 0.22F, 0.27F, 1.F} : COLOR_BUTTON, box.h / 2.);
                if (control.action == 0)
                    icon(control.action, box);
                else
                    label(control.label, box, COLOR_TEXT, 14, 600, true);
                break;
        }
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
    if (action == 8) {
        // Address the window captured on pointer-down, not whichever window
        // happens to have focus on release. Never force-kill an application.
        for (auto& weak : m_tasks)
            if (auto task = weak.lock(); task && task->window() && task->window()->metadata().stableID() == static_cast<uint64_t>(target)) {
                task->window()->sendClose();
                break;
            }
        return;
    }
    m_taskMenu = false;
    if (action == 0 || action == 1) {
        m_enabled = action == 1;
        if (!m_enabled)
            showFullscreenControls();
        else if (m_controlsIdleTimer)
            wl_event_source_timer_update(m_controlsIdleTimer, 0);
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
void CYibuAlgorithm::showFullscreenControls() {
    if (m_enabled)
        return;
    if (!m_fullscreenControlsVisible) {
        m_fullscreenControlsVisible = true;
        controls();
        if (auto monitor = m_parent->space()->workspace()->m_monitor.lock())
            g_pHyprRenderer->damageMonitor(monitor);
    }
    if (!m_controlsIdleTimer)
        m_controlsIdleTimer = wl_event_loop_add_timer(
            g_pCompositor->m_wlEventLoop,
            [](void* data) {
                auto self = static_cast<CYibuAlgorithm*>(data);
                if (self->m_enabled || self->m_pointerButtons || self->m_pressedControl || (PROTO::data && PROTO::data->dndActive()))
                    return 0;
                self->m_fullscreenControlsVisible = false;
                self->m_hoverControl              = -1;
                self->controls();
                if (auto monitor = self->m_parent->space()->workspace()->m_monitor.lock())
                    g_pHyprRenderer->damageMonitor(monitor);
                return 0;
            },
            this);
    if (m_controlsIdleTimer)
        wl_event_source_timer_update(m_controlsIdleTimer, 3000);
}

bool CYibuAlgorithm::pointer(double x, double y, uint32_t button, bool pressed) {
    if (!m_parent)
        return false;
    auto monitor = m_parent->space()->workspace()->m_monitor.lock();
    if (!monitor)
        return false;
    if (button >= 0x110 && button < 0x130) {
        uint32_t bit = 1U << (button - 0x110);
        if (pressed)
            m_pointerButtons |= bit;
        else
            m_pointerButtons &= ~bit;
    }
    showFullscreenControls();
    Vector2D point = Vector2D{x, y} / monitor->m_scale + monitor->m_position;
    if (!(PROTO::data && PROTO::data->dndActive())) {
        auto control = std::ranges::find_if(m_controls, [&](const auto& item) { return item.box.containsPoint(point); });
        int  hover   = control == m_controls.end() ? -1 : sc<int>(control - m_controls.begin());
        if (hover != m_hoverControl) {
            m_hoverControl = hover;
            g_pHyprRenderer->damageMonitor(monitor);
        }
        if (button == 0x110 && pressed && control != m_controls.end()) {
            m_pressedControl = *control;
            return true;
        }
        if (button == 0x110 && !pressed && m_pressedControl) {
            auto previous = *m_pressedControl;
            m_pressedControl.reset();
            if (control != m_controls.end() && previous.box.containsPoint(point) && previous.action == control->action && previous.target == control->target)
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
