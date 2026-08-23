/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3tiler.h"
#include "ki3_logging.h"

#include "main.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KGlobalAccel>
#include <KLocalizedString>
#include <KSharedConfig>

#include <kwineffects_interface.h>

#include <QAction>
#include <QDBusConnection>
#include <QProcess>
#include <QStringList>

#include <functional>

namespace KWin
{

Ki3Tiler::Ki3Tiler()
    : m_sessionGuard(std::make_unique<Ki3SessionGuard>())
    , m_tileTree(std::make_unique<TileTreeController>())
    , m_decoration(std::make_unique<DecorationController>(m_tileTree.get()))
{
    qCInfo(KWIN_KI3) << "ki3 tiling plugin loaded";

    // Snapshot whatever ki3 is about to overwrite in the real kwinrc/
    // kglobalshortcutsrc, before anything below touches either -- see
    // ki3sessionguard.h/ki3session.cpp and ~Ki3Tiler().
    m_sessionGuard->backupIfNeeded();

    loadWorkspaceOutputPreferences();

    Workspace *ws = Workspace::self();
    connect(ws, &Workspace::windowAdded, this, &Ki3Tiler::handleWindowAdded);
    connect(ws, &Workspace::windowRemoved, this, &Ki3Tiler::handleWindowRemoved);
    connect(ws, &Workspace::windowActivated, this, &Ki3Tiler::handleWindowActivated);

    // Adapt to monitors being plugged/unplugged (keep the one-desktop-per-screen
    // invariant and re-tile windows KWin re-homes across outputs).
    connect(ws, &Workspace::outputAdded, this, &Ki3Tiler::scheduleReconcile);
    connect(ws, &Workspace::outputRemoved, this, &Ki3Tiler::scheduleReconcile);
    // Runs synchronously, before scheduleReconcile()'s queued reconcileOutputs()
    // -- and before KWin itself destroys this output's tile tree, which is the
    // point. See teardownGroupsOnOutput()'s doc comment.
    connect(ws, &Workspace::outputRemoved, this, &Ki3Tiler::teardownGroupsOnOutput);

    // KWin re-syncs its active output to the active window's output on every
    // activation (activation.cpp), and to the pointer/touch position otherwise
    // (e.g. clicking empty desktop background) -- so this alone tracks every
    // way focusedOutput()'s result can change, without hooking windowActivated
    // too. Forwarded to the pager over D-Bus so it can mute the highlight on
    // every screen except the focused one.
    connect(ws, &Workspace::activeOutputChanged, this, &Ki3Tiler::focusedOutputChanged);

    // i3/sway model: each output independently shows one workspace. Unlike
    // Plasma's default (one desktop spanning all outputs), a desktop number here
    // exists on exactly one screen, and only desktops that are either shown or
    // occupied by windows exist at all — empty desktops are removed on the fly.
    VirtualDesktopManager::self()->setPerOutputVirtualDesktops(true);
    assignInitialDesktops();

    registerShortcuts();
    registerResizeModeShortcuts();
    disableOverviewHotCorner();

    // Adopt any windows that already exist when the plugin loads.
    for (Window *window : ws->windows()) {
        handleWindowAdded(window);
    }
    m_decoration->updateSplitIndicator();

    // Export /Ki3 on KWin's own "org.kde.KWin" service (already owned by this
    // process via its DBusInterface, alongside /Effects, /Compositor, ...) so
    // ki3-pager can show/drive per-output desktops without linking KWin internals.
    QDBusConnection::sessionBus().registerObject(
        QStringLiteral("/Ki3"), this,
        QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals);
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::currentChanged,
            this, &Ki3Tiler::desktopsChanged);
    // Notify the pager whenever the set of desktops changes (dynamic add/remove).
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::desktopAdded,
            this, [this](VirtualDesktop *) {
        Q_EMIT desktopsChanged();
    });
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::desktopRemoved,
            this, [this](VirtualDesktop *) {
        Q_EMIT desktopsChanged();
    });
}

void Ki3Tiler::registerShortcuts()
{
    const auto add = [this](const QString &name, const QString &text,
                            const QList<QKeySequence> &keys, std::function<void()> callback,
                            bool activeDuringResizeMode = false) {
        QAction *action = new QAction(this);
        action->setObjectName(name);
        action->setText(text);
        // Take the keys away from any existing owner (e.g. Meta+L = Lock Session)
        // so ki3's tiling bindings win, like a real tiling WM grabbing the keys.
        // Captured first (if this session took a fresh SessionBackup) so
        // Ki3SessionGuard::restoreOnCleanExit() can give it back later.
        for (const QKeySequence &key : keys) {
            m_sessionGuard->noteShortcutGrab(key);
            KGlobalAccel::stealShortcutSystemwide(key);
        }
        KGlobalAccel::self()->setShortcut(action, keys, KGlobalAccel::NoAutoloading);
        // i3/sway's resize mode grabs the keyboard, so nothing but the mode's
        // own bare-key resize/exit shortcuts (registerResizeModeShortcuts())
        // and the Meta+R toggle itself (activeDuringResizeMode) fire while
        // it's active. ki3 has no real grab, so every *other* global shortcut
        // approximates it by no-opping here instead.
        connect(action, &QAction::triggered, this, [this, callback = std::move(callback), activeDuringResizeMode]() {
            if (m_resizeMode && !activeDuringResizeMode) {
                return;
            }
            callback();
        });
        const QList<QKeySequence> active = KGlobalAccel::self()->shortcut(action);
        for (const QKeySequence &key : keys) {
            if (!active.contains(key)) {
                qCWarning(KWIN_KI3) << "could not take over" << key.toString()
                                    << "for" << name << "- got" << active;
            }
        }
    };

    // Focus (Meta + h/j/k/l, also arrow keys). Inert while resize mode is
    // active -- see the `add` lambda above and registerResizeModeShortcuts().
    add(QStringLiteral("ki3_focus_left"), i18n("ki3: Focus Left"),
        {QKeySequence(Qt::META | Qt::Key_H), QKeySequence(Qt::META | Qt::Key_Left)},
        [this]() {
        handleDirectional(Qt::LeftEdge);
    });
    add(QStringLiteral("ki3_focus_down"), i18n("ki3: Focus Down"),
        {QKeySequence(Qt::META | Qt::Key_J), QKeySequence(Qt::META | Qt::Key_Down)},
        [this]() {
        handleDirectional(Qt::BottomEdge);
    });
    add(QStringLiteral("ki3_focus_up"), i18n("ki3: Focus Up"),
        {QKeySequence(Qt::META | Qt::Key_K), QKeySequence(Qt::META | Qt::Key_Up)},
        [this]() {
        handleDirectional(Qt::TopEdge);
    });
    add(QStringLiteral("ki3_focus_right"), i18n("ki3: Focus Right"),
        {QKeySequence(Qt::META | Qt::Key_L), QKeySequence(Qt::META | Qt::Key_Right)},
        [this]() {
        handleDirectional(Qt::RightEdge);
    });

    // Resize mode (i3/sway "mode resize"): always live (activeDuringResizeMode)
    // so Meta+R can toggle the mode back off from inside it too.
    add(QStringLiteral("ki3_toggle_resize_mode"), i18n("ki3: Toggle Resize Mode"),
        {QKeySequence(Qt::META | Qt::Key_R)}, [this]() {
        toggleResizeMode();
    },
        /*activeDuringResizeMode=*/true);

    // Move/swap (Meta + Shift + h/j/k/l, also arrow keys)
    add(QStringLiteral("ki3_move_left"), i18n("ki3: Move Left"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_H), QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Left)},
        [this]() {
        m_tileTree->moveWindow(Qt::LeftEdge);
    });
    add(QStringLiteral("ki3_move_down"), i18n("ki3: Move Down"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_J), QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Down)},
        [this]() {
        m_tileTree->moveWindow(Qt::BottomEdge);
    });
    add(QStringLiteral("ki3_move_up"), i18n("ki3: Move Up"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_K), QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Up)},
        [this]() {
        m_tileTree->moveWindow(Qt::TopEdge);
    });
    add(QStringLiteral("ki3_move_right"), i18n("ki3: Move Right"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_L), QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Right)},
        [this]() {
        m_tileTree->moveWindow(Qt::RightEdge);
    });

    // Resize (Meta + Ctrl + h/j/k/l, also arrow keys), 50px steps
    add(QStringLiteral("ki3_resize_shrink_h"), i18n("ki3: Shrink Width"),
        {QKeySequence(Qt::META | Qt::CTRL | Qt::Key_H), QKeySequence(Qt::META | Qt::CTRL | Qt::Key_Left)},
        [this]() {
        m_tileTree->resizeActive(Qt::Horizontal, -50);
    });
    add(QStringLiteral("ki3_resize_grow_h"), i18n("ki3: Grow Width"),
        {QKeySequence(Qt::META | Qt::CTRL | Qt::Key_L), QKeySequence(Qt::META | Qt::CTRL | Qt::Key_Right)},
        [this]() {
        m_tileTree->resizeActive(Qt::Horizontal, 50);
    });
    add(QStringLiteral("ki3_resize_grow_v"), i18n("ki3: Grow Height"),
        {QKeySequence(Qt::META | Qt::CTRL | Qt::Key_J), QKeySequence(Qt::META | Qt::CTRL | Qt::Key_Down)},
        [this]() {
        m_tileTree->resizeActive(Qt::Vertical, 50);
    });
    add(QStringLiteral("ki3_resize_shrink_v"), i18n("ki3: Shrink Height"),
        {QKeySequence(Qt::META | Qt::CTRL | Qt::Key_K), QKeySequence(Qt::META | Qt::CTRL | Qt::Key_Up)},
        [this]() {
        m_tileTree->resizeActive(Qt::Vertical, -50);
    });

    // Launch a terminal (Meta + Return), i3-style.
    add(QStringLiteral("ki3_spawn_terminal"), i18n("ki3: Launch Terminal"),
        {QKeySequence(Qt::META | Qt::Key_Return)}, [this]() {
        spawnTerminal();
    });

    // Split direction for the *next* window (i3/sway "split h"/"split v").
    // Meta+H is already vim-style focus-left, so this uses Meta+G instead.
    add(QStringLiteral("ki3_split_horizontal"), i18n("ki3: Split Horizontal"),
        {QKeySequence(Qt::META | Qt::Key_G)},
        [this]() {
        m_tileTree->setSplitDirection(Tile::LayoutDirection::Horizontal);
    });
    add(QStringLiteral("ki3_split_vertical"), i18n("ki3: Split Vertical"),
        {QKeySequence(Qt::META | Qt::Key_V)},
        [this]() {
        m_tileTree->setSplitDirection(Tile::LayoutDirection::Vertical);
    });

    // Toggle the current container's layout direction (i3/sway "layout toggle
    // split") and float toggle (Meta + Shift + Space)
    add(QStringLiteral("ki3_toggle_layout"), i18n("ki3: Toggle Container Layout"),
        {QKeySequence(Qt::META | Qt::Key_E)}, [this]() {
        m_tileTree->toggleContainerLayout();
    });
    add(QStringLiteral("ki3_toggle_float"), i18n("ki3: Toggle Floating"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Space)}, [this]() {
        toggleFloating();
    });

    // Container layouts (i3/sway "layout tabbed"/"layout stacked"). Meta+W
    // matches i3/sway's actual default, and takes it over from the Overview
    // effect's default global shortcut (see also disableOverviewHotCorner()).
    add(QStringLiteral("ki3_layout_tabbed"), i18n("ki3: Tabbed Layout"),
        {QKeySequence(Qt::META | Qt::Key_W)}, [this]() {
        m_tileTree->setContainerMode(TileTreeController::ContainerMode::Tabbed);
    });
    add(QStringLiteral("ki3_layout_stacked"), i18n("ki3: Stacked Layout"),
        {QKeySequence(Qt::META | Qt::Key_S)}, [this]() {
        m_tileTree->setContainerMode(TileTreeController::ContainerMode::Stacked);
    });

    // Close the active window (i3/sway "kill").
    add(QStringLiteral("ki3_close_window"), i18n("ki3: Close Window"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Q)}, [this]() {
        closeActiveWindow();
    });

    // Steal KDE's stock Meta+PgUp/PgDown (Window Maximize/Minimize) and no-op
    // them: ki3 has no maximize/minimize handling, so either action would
    // desync a tiled window from the tile tree, same class of problem as the
    // Overview effect's Meta+W above.
    add(QStringLiteral("ki3_noop_maximize"), i18n("ki3: Disabled Window Maximize"),
        {QKeySequence(Qt::META | Qt::Key_PageUp)}, []() { });
    add(QStringLiteral("ki3_noop_minimize"), i18n("ki3: Disabled Window Minimize"),
        {QKeySequence(Qt::META | Qt::Key_PageDown)}, []() { });

    // Workspaces: Meta+1..9,0 switch, Meta+Shift+1..9,0 move window. As in
    // i3/sway, Meta+0 is workspace 10.
    static constexpr Qt::Key digits[10] = {
        Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5,
        Qt::Key_6, Qt::Key_7, Qt::Key_8, Qt::Key_9, Qt::Key_0};
    // On a US layout Shift+digit produces a symbol (!, @, #, ...), and KWin's
    // global-shortcut matching (Xkb::modifiersRelevantForGlobalShortcuts())
    // strips the Shift modifier whenever it was "consumed" to produce that
    // symbol -- it only puts Shift back for letters (see xkb.cpp, BUG 370341),
    // not digits. So a physical Meta+Shift+2 press is delivered as Meta+@, and
    // never matches a registered Meta+Shift+2 sequence. Register the produced
    // symbol as a second binding so the move shortcuts actually fire.
    static constexpr Qt::Key shiftedDigits[10] = {
        Qt::Key_Exclam, Qt::Key_At, Qt::Key_NumberSign, Qt::Key_Dollar, Qt::Key_Percent,
        Qt::Key_AsciiCircum, Qt::Key_Ampersand, Qt::Key_Asterisk, Qt::Key_ParenLeft, Qt::Key_ParenRight};
    for (int i = 0; i < 10; ++i) {
        const int n = i + 1;
        add(QStringLiteral("ki3_workspace_%1").arg(n), i18n("ki3: Switch to Workspace %1", n),
            {QKeySequence(Qt::META | digits[i])}, [this, n]() {
            switchToWorkspace(n);
        });
        add(QStringLiteral("ki3_move_workspace_%1").arg(n), i18n("ki3: Move to Workspace %1", n),
            {QKeySequence(Qt::META | Qt::SHIFT | digits[i]), QKeySequence(Qt::META | shiftedDigits[i])},
            [this, n]() {
            moveActiveToWorkspace(n);
        });
    }
}

void Ki3Tiler::registerResizeModeShortcuts()
{
    const auto add = [this](const QString &name, const QString &text,
                            const QList<QKeySequence> &keys, std::function<void()> callback) {
        QAction *action = new QAction(this);
        action->setObjectName(name);
        action->setText(text);
        // Deliberately left unbound here -- KGlobalAccel::setShortcut() is
        // called with these keys by setResizeMode() only while the mode is
        // active, and cleared again on exit. Binding bare "h" etc. up front
        // (like registerShortcuts()'s `add`) would grab that key globally
        // forever, breaking normal typing in every other application.
        connect(action, &QAction::triggered, this, std::move(callback));
        m_resizeModeActions.append({action, keys});
    };

    static constexpr qreal step = 50;
    add(QStringLiteral("ki3_resize_mode_shrink_h"), i18n("ki3: Resize Mode: Shrink Width"),
        {QKeySequence(Qt::Key_H), QKeySequence(Qt::Key_Left)},
        [this]() {
        m_tileTree->resizeActive(Qt::Horizontal, -step);
    });
    add(QStringLiteral("ki3_resize_mode_grow_h"), i18n("ki3: Resize Mode: Grow Width"),
        {QKeySequence(Qt::Key_L), QKeySequence(Qt::Key_Right)},
        [this]() {
        m_tileTree->resizeActive(Qt::Horizontal, step);
    });
    add(QStringLiteral("ki3_resize_mode_shrink_v"), i18n("ki3: Resize Mode: Shrink Height"),
        {QKeySequence(Qt::Key_K), QKeySequence(Qt::Key_Up)},
        [this]() {
        m_tileTree->resizeActive(Qt::Vertical, -step);
    });
    add(QStringLiteral("ki3_resize_mode_grow_v"), i18n("ki3: Resize Mode: Grow Height"),
        {QKeySequence(Qt::Key_J), QKeySequence(Qt::Key_Down)},
        [this]() {
        m_tileTree->resizeActive(Qt::Vertical, step);
    });
    // i3/sway also leave resize mode on Escape/Return, in addition to
    // re-pressing the mode's own Meta+R toggle.
    add(QStringLiteral("ki3_resize_mode_exit"), i18n("ki3: Resize Mode: Exit"),
        {QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)},
        [this]() {
        setResizeMode(false);
    });
}

void Ki3Tiler::disableOverviewHotCorner()
{
    KSharedConfigPtr kwinConfig = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    KConfigGroup group = kwinConfig->group(QStringLiteral("Effect-overview"));
    if (group.readEntry("BorderActivate", QList<int>{}).isEmpty()
        && group.hasKey("BorderActivate")) {
        return;
    }
    group.writeEntry("BorderActivate", QList<int>{});
    group.sync();

    OrgKdeKwinEffectsInterface interface(QStringLiteral("org.kde.KWin"),
                                         QStringLiteral("/Effects"),
                                         QDBusConnection::sessionBus());
    interface.reconfigureEffect(QStringLiteral("overview"));
}

Ki3Tiler::~Ki3Tiler()
{
    teardownManagedState();
    m_sessionGuard->restoreOnCleanExit();
}

void Ki3Tiler::teardownManagedState()
{
    // Order matters: destroy floating chrome first (self-contained on
    // m_decoration), then hand back every floating/tiled window's pre-ki3
    // decoration/keep-above state and detach every tile (self-contained on
    // m_tileTree), then drop ki3's own split structure.
    m_decoration->teardownFloatChrome();
    m_tileTree->detachAllManagedWindows();
    m_tileTree->dropManagedRoots();
}

void Ki3Tiler::handleWindowAdded(Window *window)
{
    m_decoration->watchWindow(window);

    if (m_tileTree->isManaged(window)) {
        return;
    }
    if (!m_tileTree->shouldManage(window)) {
        if (window && m_tileTree->isNonTileable(window)) {
            qCDebug(KWIN_KI3) << "not tiling" << window->resourceClass()
                              << "- matched a non-tileable rule";
        } else if (window && !window->isDeleted() && window->desktops().size() != 1) {
            qCDebug(KWIN_KI3) << "not tiling" << window->resourceClass()
                              << "- sticky/multi-desktop window, treating as floating (i3 policy)";
        }
        return;
    }
    m_tileTree->insertWindow(window);
}

void Ki3Tiler::handleWindowRemoved(Window *window)
{
    m_tileTree->removeFloating(window);
    m_decoration->destroyFloatChrome(window);
    m_tileTree->forgetWindow(window);
    m_tileTree->dropPresentationBaseline(window); // window is gone; drop its baseline
    // Nothing left to resize (e.g. the last window on a desktop just closed):
    // leaving resize mode on would silently do nothing on the next keypress
    // and strand the border indicator's "mode is active" implication.
    if (m_resizeMode && !m_tileTree->currentLeaf()) {
        setResizeMode(false);
    }
    // Deferred: let KWin finish destroying the window before we check whether
    // its desktop became empty (the window may still appear in workspace()->windows()).
    schedulePrune();
}

void Ki3Tiler::toggleResizeMode()
{
    setResizeMode(!m_resizeMode);
}

void Ki3Tiler::setResizeMode(bool active)
{
    if (m_resizeMode == active) {
        return;
    }
    m_resizeMode = active;
    qCInfo(KWIN_KI3) << "resize mode ->" << (m_resizeMode ? "on" : "off");

    // (Un)bind the bare-key resize/exit shortcuts -- see
    // registerResizeModeShortcuts() -- to approximate i3/sway's keyboard grab:
    // only while active do bare h/j/k/l/arrows/Escape/Return do anything.
    for (const ResizeModeShortcut &shortcut : m_resizeModeActions) {
        if (active) {
            for (const QKeySequence &key : shortcut.keys) {
                // Bare keys (h/j/k/l/arrows/Escape/Return) essentially never
                // have a real prior systemwide owner in practice, but capture
                // defensively anyway -- cheap, and consistent with
                // registerShortcuts()'s add() above.
                m_sessionGuard->noteShortcutGrab(key);
                KGlobalAccel::stealShortcutSystemwide(key);
            }
        }
        KGlobalAccel::self()->setShortcut(shortcut.action, active ? shortcut.keys : QList<QKeySequence>{},
                                          KGlobalAccel::NoAutoloading);
    }

    m_decoration->setResizeModeActive(active);
    Q_EMIT resizeModeChanged();
}

bool Ki3Tiler::resizeModeActive() const
{
    return m_resizeMode;
}

void Ki3Tiler::handleDirectional(Qt::Edge edge)
{
    m_tileTree->moveFocus(edge);
}

void Ki3Tiler::toggleFloating()
{
    Window *window = workspace()->activeWindow();
    if (!window) {
        return;
    }
    if (m_tileTree->isFloating(window)) {
        // Re-tile it.
        m_tileTree->removeFloating(window);
        m_decoration->destroyFloatChrome(window);
        m_tileTree->restoreKeepAbove(window); // back to whatever it was before ki3 floated it
        qCDebug(KWIN_KI3) << "unfloat" << window->caption();
        m_tileTree->insertWindow(window);
    } else if (m_tileTree->isManaged(window)) {
        // Detach from the tree; it keeps its current geometry and floats.
        m_tileTree->addFloating(window);
        qCDebug(KWIN_KI3) << "float" << window->caption();
        m_tileTree->forgetWindow(window); // restores its pre-ki3 decoration policy; createFloatChrome() below re-hides it
        window->requestTile(nullptr);
        window->setNoBorder(true); // ki3 draws its own title bar instead of the native SSD
        // i3/sway: floating windows always stay above tiled ones, even across
        // focus changes -- KWin's default focus-follows-raise would otherwise
        // sink this behind a tiled window the user clicks on next.
        window->setKeepAbove(true);
        m_decoration->createFloatChrome(window);
    }
}

void Ki3Tiler::closeActiveWindow()
{
    Window *window = workspace()->activeWindow();
    if (!window || !window->isCloseable()) {
        return;
    }
    qCDebug(KWIN_KI3) << "close" << window->caption();
    window->closeWindow();
}

void Ki3Tiler::spawnTerminal()
{
    // TODO(config): make the terminal configurable (M5).
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setProcessEnvironment(kwinApp()->processStartupEnvironment());
    process->setProgram(QStringLiteral("konsole"));
    process->startDetached();
    process->deleteLater();
    qCDebug(KWIN_KI3) << "spawn terminal";
}

void Ki3Tiler::handleWindowActivated(Window *window)
{
    // Track the focused leaf so the *next* window splits the right container.
    m_tileTree->noteWindowActivated(window);
    m_decoration->updateSplitIndicator();
    // Reposition/hide group headers (covers desktop switches, which activate a
    // window on the newly shown desktop).
    m_tileTree->refreshAllGroups();
    // A floating window's chrome border tracks focus the same way; see
    // DecorationController::updateFloatChromeBorder().
    m_decoration->updateAllFloatChromeBorders();
}

} // namespace KWin
