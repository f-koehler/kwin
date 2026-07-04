/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3tiler.h"
#include "ki3_logging.h"
#include "ki3header.h"
#include "ki3rules.h"

#include "core/output.h"
#include "main.h"
#include "tiles/customtile.h"
#include "tiles/tilemanager.h"
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
#include <QScopeGuard>
#include <QStringList>

#include <functional>

namespace KWin
{

// Shared thickness (device-independent px) of every leaf-edge overlay: the
// split-direction hint, the resize-mode border, and the focused-window border.
// Kept in one place so they stay visually consistent.
static constexpr qreal kIndicatorThickness = 3.0;

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

Ki3Tiler::Ki3Tiler()
    : m_splitIndicatorWindow(std::make_unique<Ki3SolidOverlay>())
{
    qCInfo(KWIN_KI3) << "ki3 tiling plugin loaded";

    m_nonTileableRules = loadNonTileableRules();

    for (auto &strip : m_resizeBorder) {
        strip = std::make_unique<Ki3SolidOverlay>();
    }
    for (auto &strip : m_focusBorder) {
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

    Workspace *ws = Workspace::self();
    connect(ws, &Workspace::windowAdded, this, &Ki3Tiler::handleWindowAdded);
    connect(ws, &Workspace::windowRemoved, this, &Ki3Tiler::handleWindowRemoved);
    connect(ws, &Workspace::windowActivated, this, &Ki3Tiler::handleWindowActivated);

    // Adapt to monitors being plugged/unplugged (keep the one-desktop-per-screen
    // invariant and re-tile windows KWin re-homes across outputs).
    connect(ws, &Workspace::outputAdded, this, &Ki3Tiler::scheduleReconcile);
    connect(ws, &Workspace::outputRemoved, this, &Ki3Tiler::scheduleReconcile);

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
        for (const QKeySequence &key : keys) {
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
        moveWindow(Qt::LeftEdge);
    });
    add(QStringLiteral("ki3_move_down"), i18n("ki3: Move Down"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_J), QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Down)},
        [this]() {
        moveWindow(Qt::BottomEdge);
    });
    add(QStringLiteral("ki3_move_up"), i18n("ki3: Move Up"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_K), QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Up)},
        [this]() {
        moveWindow(Qt::TopEdge);
    });
    add(QStringLiteral("ki3_move_right"), i18n("ki3: Move Right"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_L), QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Right)},
        [this]() {
        moveWindow(Qt::RightEdge);
    });

    // Resize (Meta + Ctrl + h/j/k/l, also arrow keys), 50px steps
    add(QStringLiteral("ki3_resize_shrink_h"), i18n("ki3: Shrink Width"),
        {QKeySequence(Qt::META | Qt::CTRL | Qt::Key_H), QKeySequence(Qt::META | Qt::CTRL | Qt::Key_Left)},
        [this]() {
        resizeActive(Qt::Horizontal, -50);
    });
    add(QStringLiteral("ki3_resize_grow_h"), i18n("ki3: Grow Width"),
        {QKeySequence(Qt::META | Qt::CTRL | Qt::Key_L), QKeySequence(Qt::META | Qt::CTRL | Qt::Key_Right)},
        [this]() {
        resizeActive(Qt::Horizontal, 50);
    });
    add(QStringLiteral("ki3_resize_grow_v"), i18n("ki3: Grow Height"),
        {QKeySequence(Qt::META | Qt::CTRL | Qt::Key_J), QKeySequence(Qt::META | Qt::CTRL | Qt::Key_Down)},
        [this]() {
        resizeActive(Qt::Vertical, 50);
    });
    add(QStringLiteral("ki3_resize_shrink_v"), i18n("ki3: Shrink Height"),
        {QKeySequence(Qt::META | Qt::CTRL | Qt::Key_K), QKeySequence(Qt::META | Qt::CTRL | Qt::Key_Up)},
        [this]() {
        resizeActive(Qt::Vertical, -50);
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
        setSplitDirection(Tile::LayoutDirection::Horizontal);
    });
    add(QStringLiteral("ki3_split_vertical"), i18n("ki3: Split Vertical"),
        {QKeySequence(Qt::META | Qt::Key_V)},
        [this]() {
        setSplitDirection(Tile::LayoutDirection::Vertical);
    });

    // Toggle the current container's layout direction (i3/sway "layout toggle
    // split") and float toggle (Meta + Shift + Space)
    add(QStringLiteral("ki3_toggle_layout"), i18n("ki3: Toggle Container Layout"),
        {QKeySequence(Qt::META | Qt::Key_E)}, [this]() {
        toggleContainerLayout();
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
        setContainerMode(ContainerMode::Tabbed);
    });
    add(QStringLiteral("ki3_layout_stacked"), i18n("ki3: Stacked Layout"),
        {QKeySequence(Qt::META | Qt::Key_S)}, [this]() {
        setContainerMode(ContainerMode::Stacked);
    });

    // Close the active window (i3/sway "kill").
    add(QStringLiteral("ki3_close_window"), i18n("ki3: Close Window"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Q)}, [this]() {
        closeActiveWindow();
    });

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
        resizeActive(Qt::Horizontal, -step);
    });
    add(QStringLiteral("ki3_resize_mode_grow_h"), i18n("ki3: Resize Mode: Grow Width"),
        {QKeySequence(Qt::Key_L), QKeySequence(Qt::Key_Right)},
        [this]() {
        resizeActive(Qt::Horizontal, step);
    });
    add(QStringLiteral("ki3_resize_mode_shrink_v"), i18n("ki3: Resize Mode: Shrink Height"),
        {QKeySequence(Qt::Key_K), QKeySequence(Qt::Key_Up)},
        [this]() {
        resizeActive(Qt::Vertical, -step);
    });
    add(QStringLiteral("ki3_resize_mode_grow_v"), i18n("ki3: Resize Mode: Grow Height"),
        {QKeySequence(Qt::Key_J), QKeySequence(Qt::Key_Down)},
        [this]() {
        resizeActive(Qt::Vertical, step);
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

Ki3Tiler::~Ki3Tiler() = default;

bool Ki3Tiler::isNonTileable(const Window *window) const
{
    for (const WindowRule &rule : m_nonTileableRules) {
        if (rule.matches(window)) {
            return true;
        }
    }
    return false;
}

bool Ki3Tiler::shouldManage(Window *window) const
{
    return window
        && window->isClient() // managed by KWin (has placement control)
        && !window->isInternal() // ki3's own overlays (split indicator, tab
                                 // headers) and other internal windows report
                                 // isClient() and windowType() == Normal too
        && window->isNormalWindow()
        && !window->isSpecialWindow()
        && window->isResizable()
        && window->output()
        && !window->isDeleted()
        && !isNonTileable(window)
        && !m_floatingWindows.contains(window);
}

RootTile *Ki3Tiler::rootForWindow(Window *window) const
{
    LogicalOutput *output = window->output();
    if (!output) {
        return nullptr;
    }
    TileManager *tm = workspace()->tileManager(output);
    if (!tm) {
        return nullptr;
    }
    VirtualDesktop *desktop = window->desktops().isEmpty()
        ? VirtualDesktopManager::self()->currentDesktop(output)
        : window->desktops().constFirst();
    return tm->rootTile(desktop);
}

void Ki3Tiler::ensureManaged(RootTile *root)
{
    if (m_managedRoots.contains(root)) {
        return;
    }
    m_managedRoots.insert(root);

    // KWin seeds new roots with a default 3-column layout (see
    // TileManager::readSettings). Tear it down so ki3 starts from an empty
    // root and fully owns the tree.
    const QList<Tile *> children = root->childTiles();
    for (Tile *child : children) {
        static_cast<CustomTile *>(child)->remove();
    }
    root->setLayoutDirection(Tile::LayoutDirection::Horizontal);
    qCDebug(KWIN_KI3) << "took over root; cleared" << children.size() << "default tiles";
}

void Ki3Tiler::setGeometryRecursive(CustomTile *tile, const RectF &geom)
{
    const RectF old = tile->relativeGeometry();
    // Set the geometry via the base class to skip CustomTile's neighbour-push +
    // min-size-abort logic (which fights exact slice assignment near the 0.15
    // floor). We compute non-overlapping slices ourselves, so the invariant holds.
    tile->Tile::setRelativeGeometry(geom);

    const QList<Tile *> children = tile->childTiles();
    if (children.isEmpty() || old.width() <= 0 || old.height() <= 0) {
        return;
    }
    // Remap each child's rect from the old box onto the new box proportionally,
    // so a nested layout keeps its internal split ratios when its slice resizes.
    const qreal sx = geom.width() / old.width();
    const qreal sy = geom.height() / old.height();
    for (Tile *c : children) {
        const RectF cg = c->relativeGeometry();
        const RectF ng(geom.left() + (cg.left() - old.left()) * sx,
                       geom.top() + (cg.top() - old.top()) * sy,
                       cg.width() * sx,
                       cg.height() * sy);
        setGeometryRecursive(static_cast<CustomTile *>(c), ng);
    }
}

void Ki3Tiler::redistributeEvenly(CustomTile *parent)
{
    const QList<Tile *> children = parent->childTiles();
    const int n = children.size();
    if (n < 2) {
        return;
    }
    const RectF area = parent->relativeGeometry();
    const bool horizontal = parent->layoutDirection() == Tile::LayoutDirection::Horizontal;

    for (int i = 0; i < n; ++i) {
        RectF g = area;
        if (horizontal) {
            const qreal w = area.width() / n;
            g.setX(area.left() + i * w);
            g.setWidth(w);
        } else {
            const qreal h = area.height() / n;
            g.setY(area.top() + i * h);
            g.setHeight(h);
        }
        setGeometryRecursive(static_cast<CustomTile *>(children[i]), g);
    }
    qCDebug(KWIN_KI3) << "redistribute" << n << (horizontal ? "H" : "V")
                      << "->" << [&] {
        QList<RectF> r;
        for (Tile *t : children) {
            r << t->relativeGeometry();
        }
        return r;
    }();
}

void Ki3Tiler::resyncLeafMapping(RootTile *root)
{
    root->visitDescendants([this](Tile *t) {
        if (t->childCount() != 0 || t->isRoot()) {
            return;
        }
        auto *leaf = static_cast<CustomTile *>(t);
        for (Window *w : leaf->windows()) {
            if (m_leafForWindow.contains(w)) {
                m_leafForWindow[w] = leaf;
            }
        }
    });
}

CustomTile *Ki3Tiler::firstLeaf(RootTile *root)
{
    CustomTile *leaf = nullptr;
    root->visitDescendants([&leaf](Tile *t) {
        if (!leaf && t->childCount() == 0 && t != t->rootTile()) {
            leaf = static_cast<CustomTile *>(t);
        }
    });
    return leaf ? leaf : root;
}

void Ki3Tiler::handleWindowAdded(Window *window)
{
    if (m_leafForWindow.contains(window)) {
        return;
    }
    if (!shouldManage(window)) {
        if (window && isNonTileable(window)) {
            qCDebug(KWIN_KI3) << "not tiling" << window->resourceClass()
                              << "- matched a non-tileable rule";
        }
        return;
    }
    insertWindow(window);
}

void Ki3Tiler::handleWindowRemoved(Window *window)
{
    m_floatingWindows.remove(window);
    destroyFloatChrome(window);
    forgetWindow(window);
    // Nothing left to resize (e.g. the last window on a desktop just closed):
    // leaving resize mode on would silently do nothing on the next keypress
    // and strand the border indicator's "mode is active" implication.
    if (m_resizeMode && !currentLeaf()) {
        setResizeMode(false);
    }
    // Deferred: let KWin finish destroying the window before we check whether
    // its desktop became empty (the window may still appear in workspace()->windows()).
    schedulePrune();
}

CustomTile *Ki3Tiler::currentLeaf() const
{
    if (Window *active = workspace()->activeWindow(); active != nullptr) {
        const auto it = m_leafForWindow.constFind(active);
        if (it != m_leafForWindow.constEnd() && it.value()) {
            return it.value();
        }
    }
    return m_lastFocusedLeaf;
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

    // Focus border: a dimmed sibling of the split indicator's highlight, mixed
    // halfway toward the window background so it stays clearly distinct from the
    // full-strength highlight the split strip draws on top of it (the scheme's
    // FocusColor role can equal the selection colour, hiding the split strip).
    const QColor windowBg = KColorScheme(QPalette::Active, KColorScheme::View)
                                .background(KColorScheme::NormalBackground)
                                .color();
    const QColor focusColor = mixColors(highlight, windowBg, 0.5);
    for (auto &strip : m_focusBorder) {
        strip->setColor(focusColor);
    }
}

void Ki3Tiler::updateSplitIndicator()
{
    // Refreshed unconditionally (not just when the split indicator itself has
    // something to show below) so it also runs -- and hides the borders -- when
    // the current leaf disappears, e.g. the last window on a desktop closing
    // while resize mode is active.
    //
    // Order matters for stacking: the focus border must be (re)shown *before* the
    // resize border and the split indicator so those two internal windows are
    // created after it and therefore stack above it in AboveLayer (see
    // updateFocusIndicator()).
    updateFocusIndicator();
    updateResizeIndicator();

    CustomTile *leaf = currentLeaf();

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
    if (!window || !window->isShown() || !window->isOnCurrentDesktop()) {
        m_splitIndicatorWindow->hide();
        return;
    }

    const RectF geom = leaf->windowGeometry();
    const RectF strip = (m_splitDirection == Tile::LayoutDirection::Horizontal)
        ? RectF(geom.right() - kIndicatorThickness, geom.top(), kIndicatorThickness, geom.height())
        : RectF(geom.left(), geom.bottom() - kIndicatorThickness, geom.width(), kIndicatorThickness);
    m_splitIndicatorWindow->setGeometry(strip.toRect());
    m_splitIndicatorWindow->show();
}

void Ki3Tiler::updateResizeIndicator()
{
    CustomTile *leaf = m_resizeMode ? currentLeaf() : nullptr;

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
    if (!window || !window->isShown() || !window->isOnCurrentDesktop()) {
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

void Ki3Tiler::updateFocusIndicator()
{
    CustomTile *leaf = currentLeaf();

    if (m_focusIndicatorLeaf != leaf) {
        if (m_focusIndicatorLeaf) {
            disconnect(m_focusIndicatorLeaf, &Tile::windowGeometryChanged, this, &Ki3Tiler::updateFocusIndicator);
        }
        m_focusIndicatorLeaf = leaf;
        if (leaf) {
            connect(leaf, &Tile::windowGeometryChanged, this, &Ki3Tiler::updateFocusIndicator);
        }
    }

    // Same visibility rule as the split/resize indicators: only a leaf with a
    // presently-shown window on the current desktop has anything to outline.
    Window *window = (leaf && leaf->childCount() == 0 && !leaf->windows().isEmpty())
        ? leaf->windows().constFirst()
        : nullptr;
    if (!window || !window->isShown() || !window->isOnCurrentDesktop()) {
        for (auto &strip : m_focusBorder) {
            strip->hide();
        }
        return;
    }

    const auto strips = borderStrips(leaf->windowGeometry());
    for (int i = 0; i < 4; ++i) {
        m_focusBorder[i]->setGeometry(strips[i].toRect());
        m_focusBorder[i]->show();
    }
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
    moveFocus(edge);
}

void Ki3Tiler::moveFocus(Qt::Edge edge)
{
    CustomTile *leaf = currentLeaf();
    if (!leaf) {
        return;
    }

    // Inside a tab/stack group, motion along the container's axis cycles the
    // visible tab (tabbed: left/right; stacked: up/down) instead of leaving it.
    if (auto it = m_tabbed.constFind(leaf); it != m_tabbed.constEnd()) {
        const bool horizontal = (edge == Qt::LeftEdge || edge == Qt::RightEdge);
        const bool alongAxis = (it->mode == ContainerMode::Tabbed) ? horizontal : !horizontal;
        if (alongAxis) {
            cycleTab(leaf, (edge == Qt::RightEdge || edge == Qt::BottomEdge) ? +1 : -1);
            return;
        }
    }

    // Neighbour within the same output's tree.
    if (CustomTile *target = leaf->nextNonLayoutTileAt(edge)) {
        if (!target->windows().isEmpty()) {
            qCDebug(KWIN_KI3) << "focus" << edge << leaf->relativeGeometry() << "->" << target->relativeGeometry();
            workspace()->activateWindow(target->windows().constFirst());
        }
        return;
    }

    // At the output edge: cross to the adjacent output in that direction.
    moveFocusAcrossOutput(leaf, edge);
}

void Ki3Tiler::moveFocusAcrossOutput(CustomTile *leaf, Qt::Edge edge)
{
    TileManager *manager = leaf->manager();
    LogicalOutput *output = manager ? manager->output() : nullptr;
    if (!output) {
        return;
    }
    const RectF geom = output->geometryF();
    const RectF leafGeom = leaf->absoluteGeometry();

    // A probe point just beyond the relevant edge of the current output.
    QPointF probe = geom.center();
    switch (edge) {
    case Qt::LeftEdge:
        probe = {geom.left() - 1.0, leafGeom.center().y()};
        break;
    case Qt::RightEdge:
        probe = {geom.right() + 1.0, leafGeom.center().y()};
        break;
    case Qt::TopEdge:
        probe = {leafGeom.center().x(), geom.top() - 1.0};
        break;
    case Qt::BottomEdge:
        probe = {leafGeom.center().x(), geom.bottom() + 1.0};
        break;
    }

    LogicalOutput *nextOutput = workspace()->outputAt(probe);
    qCDebug(KWIN_KI3) << "cross-output probe" << edge << probe << "from" << (void *)output
                      << "-> nextOutput" << (void *)nextOutput;
    if (!nextOutput || nextOutput == output) {
        return;
    }
    TileManager *nextManager = workspace()->tileManager(nextOutput);
    if (!nextManager) {
        return;
    }
    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(nextOutput);
    RootTile *nextRoot = nextManager->rootTile(desktop);
    if (!nextRoot) {
        return;
    }

    // Entry point just inside the adjacent output near the shared edge.
    const RectF ngeom = nextOutput->geometryF();
    QPointF entry = ngeom.center();
    switch (edge) {
    case Qt::LeftEdge:
        entry = {ngeom.right() - 2.0, std::clamp(leafGeom.center().y(), ngeom.top(), ngeom.bottom() - 1.0)};
        break;
    case Qt::RightEdge:
        entry = {ngeom.left() + 2.0, std::clamp(leafGeom.center().y(), ngeom.top(), ngeom.bottom() - 1.0)};
        break;
    case Qt::TopEdge:
        entry = {std::clamp(leafGeom.center().x(), ngeom.left(), ngeom.right() - 1.0), ngeom.bottom() - 2.0};
        break;
    case Qt::BottomEdge:
        entry = {std::clamp(leafGeom.center().x(), ngeom.left(), ngeom.right() - 1.0), ngeom.top() + 2.0};
        break;
    }

    CustomTile *target = qobject_cast<CustomTile *>(nextRoot->pick(entry));
    if (!target || target->windows().isEmpty()) {
        target = firstLeaf(nextRoot);
    }
    if (target && !target->windows().isEmpty()) {
        qCDebug(KWIN_KI3) << "focus" << edge << "across output ->" << target->absoluteGeometry();
        workspace()->activateWindow(target->windows().constFirst());
    }
}

void Ki3Tiler::moveWindow(Qt::Edge edge)
{
    CustomTile *leaf = currentLeaf();
    if (!leaf || leaf->windows().isEmpty()) {
        return;
    }

    // The window we relocate. Inside a tab/stack group all members share the
    // tile, so windows().constFirst() need not be the focused one — move the
    // *active tab* out instead. A plain leaf owns a single window.
    auto srcGroup = m_tabbed.find(leaf);
    const bool fromGroup = (srcGroup != m_tabbed.end());
    Window *self = nullptr;
    if (fromGroup) {
        if (srcGroup->windows.isEmpty()) {
            return;
        }
        const int idx = std::clamp(srcGroup->active, 0, int(srcGroup->windows.size()) - 1);
        self = srcGroup->windows[idx];
    } else {
        self = leaf->windows().constFirst();
    }
    if (!self) {
        return;
    }

    // The neighbouring leaf in `edge` direction. A group is a single leaf, so
    // this neighbour is always *outside* the group — moving a tab in any
    // direction pops it out toward that neighbour (intra-group tab reordering
    // is a separate follow-up). If the neighbour is itself a group,
    // placeWindowAt joins it as a new tab (group-to-group move).
    CustomTile *target = leaf->nextNonLayoutTileAt(edge);
    if (!target || target->windows().isEmpty()) {
        return;
    }
    if (self == target->windows().constFirst()) {
        return;
    }

    // A real remove+reinsert, not a positional swap: collapse the leaf `self`
    // leaves behind and re-place it at `target` with the same tab/sibling/split
    // rules a brand-new window gets (placeWindowAt), so the destination nests
    // per i3 semantics — e.g. moving a window onto a leaf whose parent runs a
    // different split direction than m_splitDirection actually splits that
    // leaf — instead of just trading places with its neighbour.
    //
    // The sibling case needs to know which side of `target` to land on: moving
    // *up*/*left* must insert before target, or (e.g. within an existing
    // V[top,bottom] pair) the moved window lands back in the exact slot its own
    // vacated leaf occupied and nothing visibly changes. Derived from target's
    // *actual* container direction, not just the raw edge, since
    // nextNonLayoutTileAt() can hand back a tile in a differently-oriented
    // ancestor container.
    auto *targetParent = static_cast<CustomTile *>(target->parentTile());
    const bool insertBefore = targetParent
        && ((targetParent->layoutDirection() == Tile::LayoutDirection::Horizontal && edge == Qt::LeftEdge)
            || (targetParent->layoutDirection() == Tile::LayoutDirection::Vertical && edge == Qt::TopEdge));

    CustomTile *parent = static_cast<CustomTile *>(leaf->parentTile());
    auto *root = static_cast<RootTile *>(leaf->rootTile());

    // Moving out of a group: drop `self` from the group's TabState up front so
    // our bookkeeping stays consistent once placeWindowAt re-homes the window.
    // The tile keeps the surviving tabs (refreshed below), or empties out and
    // is collapsed with every other vacated leaf. srcGroup is not reused after
    // the erase.
    if (fromGroup) {
        srcGroup->windows.removeAll(self);
        if (srcGroup->windows.isEmpty()) {
            destroyGroupHeader(leaf); // last tab gone: drop header + clear reserve
            m_tabbed.erase(srcGroup);
        } else {
            srcGroup->active = std::clamp(srcGroup->active, 0, int(srcGroup->windows.size()) - 1);
        }
    }

    placeWindowAt(self, target, insertBefore); // manage() evacuates `self` from `leaf` internally

    if (!leaf->isRoot() && leaf->childCount() == 0 && leaf->windows().isEmpty()) {
        qCDebug(KWIN_KI3) << "move: collapse empty leaf left by" << self->caption();
        leaf->remove();
        resyncLeafMapping(root);
        if (parent && parent->isLayout()) {
            redistributeEvenly(parent);
        }
        updateSplitIndicator();
    } else if (fromGroup && m_tabbed.contains(leaf)) {
        refreshGroup(leaf); // group survived with remaining tabs: restack its header
    }
    qCDebug(KWIN_KI3) << "move" << edge << self->caption() << (fromGroup ? "(out of group)" : "");
    workspace()->activateWindow(self);
}

void Ki3Tiler::resizeActive(Qt::Orientation orientation, qreal deltaPixels)
{
    CustomTile *leaf = currentLeaf();
    if (!leaf) {
        return;
    }
    // Resize towards a neighbour if one exists on that side, else the other side.
    if (orientation == Qt::Horizontal) {
        if (leaf->nextTileAt(Qt::RightEdge)) {
            leaf->resizeByPixels(deltaPixels, Qt::RightEdge);
        } else if (leaf->nextTileAt(Qt::LeftEdge)) {
            leaf->resizeByPixels(-deltaPixels, Qt::LeftEdge);
        }
    } else {
        if (leaf->nextTileAt(Qt::BottomEdge)) {
            leaf->resizeByPixels(deltaPixels, Qt::BottomEdge);
        } else if (leaf->nextTileAt(Qt::TopEdge)) {
            leaf->resizeByPixels(-deltaPixels, Qt::TopEdge);
        }
    }
    qCDebug(KWIN_KI3) << "resize" << orientation << deltaPixels << "->" << leaf->relativeGeometry();
}

void Ki3Tiler::setSplitDirection(Tile::LayoutDirection direction)
{
    m_splitDirection = direction;
    qCInfo(KWIN_KI3) << "split direction ->"
                     << (m_splitDirection == Tile::LayoutDirection::Horizontal ? "horizontal" : "vertical");
    updateSplitIndicator();
}

void Ki3Tiler::toggleContainerLayout()
{
    CustomTile *leaf = currentLeaf();
    if (!leaf) {
        return;
    }

    // The focused leaf is itself a tabbed/stacked group: collapse it back to a
    // plain split, mirroring setContainerMode()'s same-key-toggles-back rule.
    if (m_tabbed.contains(leaf)) {
        untabContainer(leaf);
        return;
    }

    auto *parent = static_cast<CustomTile *>(leaf->parentTile());
    if (!parent || !parent->isLayout()) {
        return;
    }
    // Floating containers have no h/v orientation to flip.
    if (parent->layoutDirection() != Tile::LayoutDirection::Horizontal
        && parent->layoutDirection() != Tile::LayoutDirection::Vertical) {
        return;
    }

    const auto newDirection = (parent->layoutDirection() == Tile::LayoutDirection::Horizontal)
        ? Tile::LayoutDirection::Vertical
        : Tile::LayoutDirection::Horizontal;
    parent->setLayoutDirection(newDirection);
    redistributeEvenly(parent);
    qCInfo(KWIN_KI3) << "container layout ->"
                     << (newDirection == Tile::LayoutDirection::Horizontal ? "horizontal" : "vertical");
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
    // The freshly added window isn't in the map yet when its activation fires,
    // so this won't clobber the previous focus before insertWindow() runs.
    auto it = m_leafForWindow.constFind(window);
    if (it != m_leafForWindow.constEnd() && it.value()) {
        m_lastFocusedLeaf = it.value();
    }
    updateSplitIndicator();
    // Reposition/hide group headers (covers desktop switches, which activate a
    // window on the newly shown desktop).
    refreshAllGroups();
}

void Ki3Tiler::insertWindow(Window *window)
{
    RootTile *root = rootForWindow(window);
    if (!root) {
        qCWarning(KWIN_KI3) << "no root tile for" << window;
        return;
    }
    ensureManaged(root);

    // First window on this root fills it.
    if (root->childCount() == 0 && root->windows().isEmpty()) {
        root->manage(window);
        m_leafForWindow[window] = root;
        m_lastFocusedLeaf = root;
        window->setNoBorder(true); // i3-style: tiled windows never show their own title bar
        qCDebug(KWIN_KI3) << "insert (root):" << window->caption() << "->" << root->windowGeometry();
        updateSplitIndicator();
        return;
    }

    // Pick the target leaf: the last focused leaf if it's in this root tree,
    // otherwise the first available leaf.
    CustomTile *target = nullptr;
    if (m_lastFocusedLeaf && m_lastFocusedLeaf->rootTile() == root && m_lastFocusedLeaf->childCount() == 0) {
        target = m_lastFocusedLeaf;
    } else {
        target = firstLeaf(root);
    }

    placeWindowAt(window, target);
}

void Ki3Tiler::placeWindowAt(Window *window, CustomTile *target, bool insertBefore)
{
    // If the chosen container is a tab/stack group, the window joins it as a
    // new tab rather than splitting the tree.
    if (auto it = m_tabbed.find(target); it != m_tabbed.end()) {
        target->manage(window);
        it->windows.append(window);
        it->active = it->windows.size() - 1;
        m_leafForWindow[window] = target;
        m_lastFocusedLeaf = target;
        window->setNoBorder(true); // hide its own title bar; ki3's tab bar shows instead
        qCDebug(KWIN_KI3) << "insert (tab):" << window->caption() << "tabs now" << it->windows.size();
        refreshGroup(target);
        updateSplitIndicator();
        return;
    }

    // i3 behaviour: if the target leaf already lives in a layout running in the
    // current split direction, add the window as a *sibling* and rebalance all
    // of them evenly (1:1:1...), rather than nesting + halving the target cell
    // (which would give 2:1:1). Only nest when the direction differs.
    auto *parent = static_cast<CustomTile *>(target->parentTile());
    if (parent && parent->isLayout() && parent->layoutDirection() == m_splitDirection
        && target->childCount() == 0) {
        const int position = insertBefore ? target->row() : target->row() + 1;
        CustomTile *forNew = parent->createChildAt(target->relativeGeometry(),
                                                   m_splitDirection, position);
        forNew->manage(window);
        m_leafForWindow[window] = forNew;
        m_lastFocusedLeaf = forNew;
        window->setNoBorder(true);
        redistributeEvenly(parent);
        qCDebug(KWIN_KI3) << "insert (sibling):" << window->caption()
                          << "siblings now" << parent->childCount()
                          << "at position" << position
                          << "->" << forNew->windowGeometry();
        updateSplitIndicator();
        return;
    }

    // Capture the windows on the target *before* splitting: split() changes the
    // tile geometry, which makes KWin re-home the existing window on its own. We
    // re-assign them explicitly afterwards so our bookkeeping stays authoritative.
    const QList<Window *> existing = target->windows();

    const QList<CustomTile *> created = target->split(m_splitDirection);
    if (created.size() != 2) {
        qCWarning(KWIN_KI3) << "unexpected split result, size" << created.size();
        target->manage(window);
        m_leafForWindow[window] = target;
        m_lastFocusedLeaf = target;
        window->setNoBorder(true);
        updateSplitIndicator();
        return;
    }

    CustomTile *forOld = created.first();
    CustomTile *forNew = created.last();

    // If the target became a layout, move its existing window(s) into the new
    // first child so every window stays on a leaf.
    if (forOld != target) {
        for (Window *w : existing) {
            forOld->manage(w);
            m_leafForWindow[w] = forOld;
        }
    }

    forNew->manage(window);
    m_leafForWindow[window] = forNew;
    m_lastFocusedLeaf = forNew;
    window->setNoBorder(true);
    qCDebug(KWIN_KI3) << "insert (split):" << window->caption() << "->" << forNew->windowGeometry();
    updateSplitIndicator();
}

void Ki3Tiler::forgetWindow(Window *window)
{
    auto it = m_leafForWindow.find(window);
    if (it == m_leafForWindow.end()) {
        return;
    }
    QPointer<CustomTile> tile = it.value();
    m_leafForWindow.erase(it);
    if (m_lastFocusedLeaf == tile) {
        m_lastFocusedLeaf = nullptr;
    }
    if (!tile) {
        return;
    }
    if (!window->isDeleted()) {
        window->setNoBorder(false); // leaving the tile tree: restore its own title bar
    }

    // Tab/stack group member: drop it from the group. As long as at least one
    // tab remains, the leaf stays put (still owns the survivors) — only refresh
    // which tab shows. If it was the last, fall through to collapse the leaf.
    if (auto ti = m_tabbed.find(tile); ti != m_tabbed.end()) {
        tile->forget(window);
        ti->windows.removeAll(window);
        if (!ti->windows.isEmpty()) {
            ti->active = std::clamp(ti->active, 0, int(ti->windows.size()) - 1);
            m_lastFocusedLeaf = tile;
            refreshGroup(tile);
            updateSplitIndicator();
            return;
        }
        destroyGroupHeader(tile); // last tab gone: drop header + clear reserve
        m_tabbed.erase(ti);
    }

    // Detach the window if still attached, then collapse the empty leaf.
    // CustomTile::remove() only stretches the immediate prev/next neighbour into
    // the freed space (customtile.cpp:290-324); i3 instead shares it across all
    // remaining siblings, so we rebalance the parent evenly afterwards.
    QPointer<CustomTile> parent = static_cast<CustomTile *>(tile->parentTile());
    auto *root = static_cast<RootTile *>(tile->rootTile());
    tile->forget(window);
    if (!tile->isRoot() && tile->childCount() == 0 && tile->windows().isEmpty()) {
        qCDebug(KWIN_KI3) << "collapse empty leaf left by" << window->caption();
        tile->remove();
        // remove() may have promoted a single-child layout, migrating its window
        // into the parent tile and invalidating our leaf mapping; repair it
        // before redistributing so a later close finds the right leaf.
        resyncLeafMapping(root);
        if (parent && parent->isLayout()) {
            redistributeEvenly(parent);
        }
    }
    updateSplitIndicator();
}

} // namespace KWin
