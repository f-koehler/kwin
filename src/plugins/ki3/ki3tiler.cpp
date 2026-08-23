/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3tiler.h"
#include "ki3_logging.h"
#include "ki3header.h"

#include "main.h"
#include "tiles/customtile.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <KColorScheme>
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

// Linear-RGB blend of @p a and @p b, @p t of the way from a to b.
static QColor mixColors(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

// The four kIndicatorThickness-wide edge strips outlining @p geom, in the order
// the resize/focus border arrays expect: top, bottom, left, right.
static std::array<RectF, 4> borderStrips(const RectF &geom)
{
    return {
        RectF(geom.left(), geom.top(), geom.width(), kIndicatorThickness),
        RectF(geom.left(), geom.bottom() - kIndicatorThickness, geom.width(), kIndicatorThickness),
        RectF(geom.left(), geom.top(), kIndicatorThickness, geom.height()),
        RectF(geom.right() - kIndicatorThickness, geom.top(), kIndicatorThickness, geom.height()),
    };
}

// Same as borderStrips(), but the four strips sit just *outside* @p geom (in
// the tile's own padding/gap, like a floating window's chrome border — see
// repositionFloatChrome()) instead of overlapping its content. Top/bottom
// extend the full outward width (including both side thicknesses) so all
// four strips still meet cleanly at the corners, same as borderStrips()'s
// inward overlap does.
static std::array<RectF, 4> outwardBorderStrips(const RectF &geom)
{
    return {
        RectF(geom.left() - kIndicatorThickness, geom.top() - kIndicatorThickness,
              geom.width() + 2 * kIndicatorThickness, kIndicatorThickness),
        RectF(geom.left() - kIndicatorThickness, geom.bottom(),
              geom.width() + 2 * kIndicatorThickness, kIndicatorThickness),
        RectF(geom.left() - kIndicatorThickness, geom.top(), kIndicatorThickness, geom.height()),
        RectF(geom.right(), geom.top(), kIndicatorThickness, geom.height()),
    };
}

Ki3Tiler::Ki3Tiler()
    : m_sessionGuard(std::make_unique<Ki3SessionGuard>())
    , m_tileTree(std::make_unique<TileTreeController>())
    , m_splitIndicatorWindow(std::make_unique<Ki3SolidOverlay>())
{
    qCInfo(KWIN_KI3) << "ki3 tiling plugin loaded";

    // Snapshot whatever ki3 is about to overwrite in the real kwinrc/
    // kglobalshortcutsrc, before anything below touches either -- see
    // ki3sessionguard.h/ki3session.cpp and ~Ki3Tiler().
    m_sessionGuard->backupIfNeeded();

    loadWorkspaceOutputPreferences();

    for (auto &strip : m_resizeBorder) {
        strip = std::make_unique<Ki3SolidOverlay>();
    }

    // Colour both overlays from the active colour scheme now, and re-read them
    // whenever the user switches scheme (kdeglobals changes) so the accents stay
    // in step with ki3-pager's Kirigami colours instead of freezing at load.
    applyIndicatorColors();
    m_colorSchemeWatcher = KConfigWatcher::create(KSharedConfig::openConfig());
    connect(m_colorSchemeWatcher.get(), &KConfigWatcher::configChanged, this,
            [this](const KConfigGroup &group, const QByteArrayList &) {
        if (group.name().startsWith(QLatin1String("Colors:"))
            || group.name() == QLatin1String("General")) {
            applyIndicatorColors();
        }
    });

    // Replaces what used to be a direct updateSplitIndicator()/
    // updateTileBorders() call from inside every tile-tree-mutating method --
    // see TileTreeController::layoutChanged()'s doc comment.
    connect(m_tileTree.get(), &TileTreeController::layoutChanged, this, &Ki3Tiler::updateSplitIndicator);

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
    updateSplitIndicator();

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
    // Floating windows: drop ki3's chrome first (still Ki3Tiler-owned pending
    // a future decoration-controller extraction) -- everything else (handing
    // back keep-above/decoration, detaching tiled windows, dropping group
    // headers, and removing ki3's own split structure) is self-contained on
    // m_tileTree. Snapshot the set: destroyFloatChrome() doesn't touch it, but
    // copying defensively costs nothing and matches how every other teardown
    // loop here treats "the collection we're about to hand off" as a fixed
    // point-in-time list.
    const QSet<Window *> floatingWindows = m_tileTree->floatingWindows();
    for (Window *window : floatingWindows) {
        destroyFloatChrome(window);
    }
    m_tileTree->detachAllManagedWindows();
    m_tileTree->dropManagedRoots();
}

void Ki3Tiler::handleWindowAdded(Window *window)
{
    // Any window's real geometry settling can flip another leaf's occlusion
    // state (leafWindowOccluded() walks the whole stacking order, not just
    // managed windows), and window->frameGeometry() often still reflects the
    // *old* rect for one more round-trip after a fresh split/insert already
    // reassigned its tile -- see the 2026-07-05 PLAN entry "tile borders stuck
    // invisible after a fresh split". Recheck borders whenever any window's
    // geometry actually commits, not only on ki3's own explicit resync calls;
    // deferred (see scheduleBorderRecheck()) since this signal can fire from
    // inside a Wayland surface-commit transaction, where synchronously
    // touching ki3's own overlay windows crashes. Qt::UniqueConnection guards
    // the harmless case of this firing twice for the same window.
    connect(window, &Window::frameGeometryChanged, this, &Ki3Tiler::scheduleBorderRecheck,
            Qt::UniqueConnection);

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
    destroyFloatChrome(window);
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

void Ki3Tiler::applyIndicatorColors()
{
    // Split indicator: the scheme's selection background — the same value
    // Kirigami exposes as Theme.highlightColor, which ki3-pager uses for the
    // current/active desktop cell.
    const QColor highlight = KColorScheme(QPalette::Active, KColorScheme::Selection)
                                 .background(KColorScheme::NormalBackground)
                                 .color();
    m_splitIndicatorWindow->setColor(highlight);

    // Resize border: the scheme's neutral ("warning") foreground — Kirigami's
    // Theme.neutralTextColor, which ki3-pager switches to while resize mode is
    // active, and distinct from the split indicator's selection accent.
    const QColor resizeColor = KColorScheme(QPalette::Active, KColorScheme::View)
                                   .foreground(KColorScheme::NeutralText)
                                   .color();
    for (auto &strip : m_resizeBorder) {
        strip->setColor(resizeColor);
    }

    // Window chrome (tile borders, tab/stack headers, floating title bar):
    // border/text/unfocused-background come from KColorScheme's Header set —
    // the same role KWin's own window decorations use for their titlebar —
    // but the *focused* background reuses the split indicator's own
    // highlight, mixed halfway toward the view background so it reads as a
    // muted accent rather than the raw selection colour. The Header set's own
    // Active background turned out to have poor/inverted contrast against its
    // Inactive background in some colour schemes -- barely distinguishable
    // from "unfocused", the opposite of the point -- so it isn't used for the
    // focused background itself (see the 2026-07-05 PLAN entry).
    const KColorScheme activeHeader(QPalette::Active, KColorScheme::Header);
    const KColorScheme inactiveHeader(QPalette::Inactive, KColorScheme::Header);
    const QColor windowBg = KColorScheme(QPalette::Active, KColorScheme::View)
                                .background(KColorScheme::NormalBackground)
                                .color();
    m_focusBorderColor = mixColors(highlight, windowBg, 0.5);
    m_unfocusedBorderColor = inactiveHeader.background().color();
    m_headerPalette.activeBg = m_focusBorderColor;
    m_headerPalette.inactiveBg = m_unfocusedBorderColor;
    m_headerPalette.border = activeHeader.background(KColorScheme::AlternateBackground).color();
    m_headerPalette.activeText = activeHeader.foreground().color();
    m_headerPalette.inactiveText = inactiveHeader.foreground().color();

    // Reused for a focused floating window's chrome border and every tab/
    // stack header + floating title bar, so all of ki3's own chrome tracks a
    // colour-scheme switch together.
    updateTileBorders();
    updateAllFloatChromeBorders();
    updateHeaderPalette();
}

void Ki3Tiler::updateHeaderPalette()
{
    m_tileTree->setHeaderPalette(m_headerPalette);
    for (auto it = m_floatChrome.begin(); it != m_floatChrome.end(); ++it) {
        it->titleBar->setPalette(m_headerPalette);
    }
}

void Ki3Tiler::scheduleBorderRecheck()
{
    if (m_borderRecheckPending) {
        return;
    }
    m_borderRecheckPending = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_borderRecheckPending = false;
        updateSplitIndicator();
    }, Qt::QueuedConnection);
}

void Ki3Tiler::updateSplitIndicator()
{
    // Refreshed unconditionally (not just when the split indicator itself has
    // something to show below) so it also runs -- and hides the borders -- when
    // the current leaf disappears, e.g. the last window on a desktop closing
    // while resize mode is active.
    //
    // Order matters for stacking: tile borders must be (re)shown *before* the
    // resize border and the split indicator so those two internal windows are
    // created after them and therefore stack above them in AboveLayer (see
    // updateTileBorders()).
    updateTileBorders();
    updateResizeIndicator();

    CustomTile *leaf = m_tileTree->currentLeaf();

    if (m_splitIndicatorLeaf != leaf) {
        if (m_splitIndicatorLeaf) {
            disconnect(m_splitIndicatorLeaf, &Tile::windowGeometryChanged, this, &Ki3Tiler::updateSplitIndicator);
        }
        m_splitIndicatorLeaf = leaf;
        if (leaf) {
            connect(leaf, &Tile::windowGeometryChanged, this, &Ki3Tiler::updateSplitIndicator);
        }
    }

    // Only meaningful on an actual leaf holding a window that is presently
    // visible; a bare root, a tile mid-restructure (childCount() > 0), or a
    // leaf left over on a desktop we've since switched away from has nothing
    // to highlight.
    Window *window = (leaf && leaf->childCount() == 0 && !leaf->windows().isEmpty())
        ? leaf->windows().constFirst()
        : nullptr;
    if (!window || !window->isShown() || !window->isOnCurrentDesktop() || m_tileTree->leafWindowOccluded(window)) {
        m_splitIndicatorWindow->hide();
        return;
    }

    const RectF geom = leaf->windowGeometry();
    const RectF strip = (m_tileTree->splitDirection() == Tile::LayoutDirection::Horizontal)
        ? RectF(geom.right() - kIndicatorThickness, geom.top(), kIndicatorThickness, geom.height())
        : RectF(geom.left(), geom.bottom() - kIndicatorThickness, geom.width(), kIndicatorThickness);
    m_splitIndicatorWindow->setGeometry(strip.toRect());
    m_splitIndicatorWindow->show();
}

void Ki3Tiler::updateResizeIndicator()
{
    CustomTile *leaf = m_resizeMode ? m_tileTree->currentLeaf() : nullptr;

    if (m_resizeIndicatorLeaf != leaf) {
        if (m_resizeIndicatorLeaf) {
            disconnect(m_resizeIndicatorLeaf, &Tile::windowGeometryChanged, this, &Ki3Tiler::updateResizeIndicator);
        }
        m_resizeIndicatorLeaf = leaf;
        if (leaf) {
            connect(leaf, &Tile::windowGeometryChanged, this, &Ki3Tiler::updateResizeIndicator);
        }
    }

    // Same visibility rule as the split indicator: only a leaf with a
    // presently-shown window on the current desktop has anything to outline.
    Window *window = (leaf && leaf->childCount() == 0 && !leaf->windows().isEmpty())
        ? leaf->windows().constFirst()
        : nullptr;
    if (!window || !window->isShown() || !window->isOnCurrentDesktop() || m_tileTree->leafWindowOccluded(window)) {
        for (auto &strip : m_resizeBorder) {
            strip->hide();
        }
        return;
    }

    const auto strips = borderStrips(leaf->windowGeometry());
    for (int i = 0; i < 4; ++i) {
        m_resizeBorder[i]->setGeometry(strips[i].toRect());
        m_resizeBorder[i]->show();
    }
}

void Ki3Tiler::updateTileBorders()
{
    // Every leaf currently showing a real tiled window gets a border -- unlike
    // a single "current leaf only" indicator, so the boundary line between two
    // tiles stays put and only *changes colour* as focus moves between them,
    // instead of popping in/out on alternating sides of the gap.
    const QSet<CustomTile *> liveLeaves = m_tileTree->liveLeaves();

    // i3-style "smart borders": a lone tile filling its whole output/desktop
    // has no neighbour to delimit, so only draw borders where more than one
    // leaf shares a root. Counted across every live leaf regardless of its
    // own shown/occlusion state below -- a root's leaves are always all on
    // the same (single) desktop, so this is just "how many tiles does this
    // output/desktop have", independent of transient per-window occlusion.
    QHash<Tile *, int> leafCountByRoot;
    for (CustomTile *leaf : liveLeaves) {
        ++leafCountByRoot[leaf->rootTile()];
    }

    for (CustomTile *leaf : liveLeaves) {
        // Same visibility rule as the split/resize indicators: only a leaf
        // with a presently-shown, unoccluded window on the current desktop
        // has anything to outline.
        Window *window = (leaf->childCount() == 0 && !leaf->windows().isEmpty())
            ? leaf->windows().constFirst()
            : nullptr;
        const bool visible = window && window->isShown() && window->isOnCurrentDesktop()
            && !m_tileTree->leafWindowOccluded(window) && leafCountByRoot.value(leaf->rootTile()) > 1;
        if (!visible) {
            if (auto it = m_tileBorders.find(leaf); it != m_tileBorders.end()) {
                disconnect(it->geometryConn);
                m_tileBorders.erase(it);
            }
            continue;
        }

        if (!m_tileBorders.contains(leaf)) {
            TileBorder border;
            for (auto &strip : border.strips) {
                strip = std::make_shared<Ki3SolidOverlay>();
            }
            // A resize/redistribute that doesn't itself route through
            // updateSplitIndicator() (e.g. a neighbour tile being pushed by
            // another one resizing) still needs to keep the border glued to
            // its leaf.
            border.geometryConn = connect(leaf, &Tile::windowGeometryChanged, this,
                                          [this, leaf] {
                repositionTileBorder(leaf);
            });
            m_tileBorders.insert(leaf, std::move(border));
            // Drop the entry the instant the tile is destroyed (e.g. an
            // output unplug tears down its tile tree) so the raw-pointer key
            // never dangles; see TileTreeController::onGroupTileDestroyed()
            // for the same pattern.
            connect(leaf, &QObject::destroyed, this, &Ki3Tiler::onTileBorderDestroyed,
                    Qt::UniqueConnection);
        }
        repositionTileBorder(leaf);
    }

    // Drop borders for leaves no longer live at all (window removed, floated
    // away, or the tile's own tree torn down); everything else not currently
    // visible was already handled inside the loop above.
    for (auto it = m_tileBorders.begin(); it != m_tileBorders.end();) {
        if (!liveLeaves.contains(it.key())) {
            disconnect(it->geometryConn);
            it = m_tileBorders.erase(it);
        } else {
            ++it;
        }
    }
}

void Ki3Tiler::repositionTileBorder(CustomTile *leaf)
{
    auto it = m_tileBorders.find(leaf);
    if (it == m_tileBorders.end()) {
        return;
    }
    TileBorder &border = it.value();

    const auto strips = outwardBorderStrips(leaf->windowGeometry());
    // A tab/stack group already has its own header showing the title right
    // above windowGeometry() (see TileTreeController::refreshGroup()); the top
    // strip there would just be redundant wasted space, so skip it for
    // grouped leaves only.
    const bool skipTop = m_tileTree->isGroup(leaf);
    const QColor &color = (leaf == m_tileTree->currentLeaf()) ? m_focusBorderColor : m_unfocusedBorderColor;
    for (int i = 0; i < 4; ++i) {
        if (i == 0 && skipTop) {
            border.strips[i]->hide();
            continue;
        }
        border.strips[i]->setColor(color);
        border.strips[i]->setGeometry(strips[i].toRect());
        border.strips[i]->show();
    }
}

void Ki3Tiler::onTileBorderDestroyed(QObject *tile)
{
    // The tile is mid-destruction; use it as a bare key only (no dereference).
    auto it = m_tileBorders.find(static_cast<CustomTile *>(tile));
    if (it == m_tileBorders.end()) {
        return;
    }
    // This slot runs reentrantly: synchronously nested inside the *dying
    // tile's own* QObject destructor (Tile::~Tile() -> ~QObject() ->
    // destroyed() -> here). Actually tearing down the border's
    // Ki3SolidOverlay windows right now is unsafe -- destroying an internal
    // QWindow cascades through Workspace::removeInternalWindow() ->
    // windowRemoved(), which every live Tile relays into unmanage(); if
    // that reaches back into a tile whose own destructor is still
    // unwinding on this very call stack, it's a real crash (ki3-PLAN.md has
    // the trace -- this is exactly the follow-on crash the Workspace-scoped
    // disconnect() in Tile::~Tile() alone didn't cover). Drop the map entry
    // now (m_tileBorders itself must never keep the stale key), but let the
    // TileBorder's shared_ptrs -- and hence the overlay windows -- actually
    // die on the next event loop turn instead, safely outside this cascade.
    TileBorder border = std::move(it.value());
    m_tileBorders.erase(it);
    QMetaObject::invokeMethod(this, [border = std::move(border)]() {
        qCDebug(KWIN_KI3) << "tile border: deferred teardown of stale entry running";
    }, Qt::QueuedConnection);
    qCDebug(KWIN_KI3) << "tile border: owning tile destroyed; dropped stale entry";
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

    updateResizeIndicator();
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
    updateSplitIndicator();
    // Reposition/hide group headers (covers desktop switches, which activate a
    // window on the newly shown desktop).
    m_tileTree->refreshAllGroups();
    // A floating window's chrome border tracks focus the same way; see
    // updateFloatChromeBorder().
    updateAllFloatChromeBorders();
}

} // namespace KWin
