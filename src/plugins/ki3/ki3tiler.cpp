/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3tiler.h"
#include "ki3_logging.h"

#include "core/output.h"
#include "main.h"
#include "tiles/customtile.h"
#include "tiles/tilemanager.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <KGlobalAccel>
#include <KLocalizedString>

#include <QAction>
#include <QProcess>

#include <functional>

namespace KWin
{

Ki3Tiler::Ki3Tiler()
{
    qCInfo(KWIN_KI3) << "ki3 tiling plugin loaded";

    Workspace *ws = Workspace::self();
    connect(ws, &Workspace::windowAdded, this, &Ki3Tiler::handleWindowAdded);
    connect(ws, &Workspace::windowRemoved, this, &Ki3Tiler::handleWindowRemoved);
    connect(ws, &Workspace::windowActivated, this, &Ki3Tiler::handleWindowActivated);

    // i3/sway model: each output independently shows one workspace.
    VirtualDesktopManager::self()->setPerOutputVirtualDesktops(true);

    registerShortcuts();

    // Adopt any windows that already exist when the plugin loads.
    for (Window *window : ws->windows()) {
        handleWindowAdded(window);
    }
}

void Ki3Tiler::registerShortcuts()
{
    const auto add = [this](const QString &name, const QString &text,
                            const QList<QKeySequence> &keys, std::function<void()> callback) {
        QAction *action = new QAction(this);
        action->setObjectName(name);
        action->setText(text);
        // Take the keys away from any existing owner (e.g. Meta+L = Lock Session)
        // so ki3's tiling bindings win, like a real tiling WM grabbing the keys.
        for (const QKeySequence &key : keys) {
            KGlobalAccel::stealShortcutSystemwide(key);
        }
        KGlobalAccel::self()->setShortcut(action, keys, KGlobalAccel::NoAutoloading);
        connect(action, &QAction::triggered, this, std::move(callback));
        const QList<QKeySequence> active = KGlobalAccel::self()->shortcut(action);
        for (const QKeySequence &key : keys) {
            if (!active.contains(key)) {
                qCWarning(KWIN_KI3) << "could not take over" << key.toString()
                                    << "for" << name << "- got" << active;
            }
        }
    };

    // Focus (Meta + h/j/k/l, also arrow keys)
    add(QStringLiteral("ki3_focus_left"), i18n("ki3: Focus Left"),
        {QKeySequence(Qt::META | Qt::Key_H), QKeySequence(Qt::META | Qt::Key_Left)},
        [this]() {
        moveFocus(Qt::LeftEdge);
    });
    add(QStringLiteral("ki3_focus_down"), i18n("ki3: Focus Down"),
        {QKeySequence(Qt::META | Qt::Key_J), QKeySequence(Qt::META | Qt::Key_Down)},
        [this]() {
        moveFocus(Qt::BottomEdge);
    });
    add(QStringLiteral("ki3_focus_up"), i18n("ki3: Focus Up"),
        {QKeySequence(Qt::META | Qt::Key_K), QKeySequence(Qt::META | Qt::Key_Up)},
        [this]() {
        moveFocus(Qt::TopEdge);
    });
    add(QStringLiteral("ki3_focus_right"), i18n("ki3: Focus Right"),
        {QKeySequence(Qt::META | Qt::Key_L), QKeySequence(Qt::META | Qt::Key_Right)},
        [this]() {
        moveFocus(Qt::RightEdge);
    });

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

    // Split direction toggle (Meta + E) and float toggle (Meta + Shift + Space)
    add(QStringLiteral("ki3_toggle_split"), i18n("ki3: Toggle Split Direction"),
        {QKeySequence(Qt::META | Qt::Key_E)}, [this]() {
        toggleSplitDirection();
    });
    add(QStringLiteral("ki3_toggle_float"), i18n("ki3: Toggle Floating"),
        {QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Space)}, [this]() {
        toggleFloating();
    });

    // Workspaces: Meta+1..9 switch (focused output), Meta+Shift+1..9 move window.
    static constexpr Qt::Key digits[9] = {
        Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5,
        Qt::Key_6, Qt::Key_7, Qt::Key_8, Qt::Key_9};
    for (int i = 0; i < 9; ++i) {
        const int n = i + 1;
        add(QStringLiteral("ki3_workspace_%1").arg(n), i18n("ki3: Switch to Workspace %1", n),
            {QKeySequence(Qt::META | digits[i])}, [this, n]() {
            switchToWorkspace(n);
        });
        add(QStringLiteral("ki3_move_workspace_%1").arg(n), i18n("ki3: Move to Workspace %1", n),
            {QKeySequence(Qt::META | Qt::SHIFT | digits[i])}, [this, n]() {
            moveActiveToWorkspace(n);
        });
    }
}

Ki3Tiler::~Ki3Tiler() = default;

bool Ki3Tiler::shouldManage(Window *window) const
{
    return window
        && window->isClient() // managed by KWin (has placement control)
        && window->isNormalWindow()
        && !window->isSpecialWindow()
        && window->isResizable()
        && window->output()
        && !window->isDeleted()
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
    if (!shouldManage(window) || m_leafForWindow.contains(window)) {
        return;
    }
    insertWindow(window);
}

void Ki3Tiler::handleWindowRemoved(Window *window)
{
    m_floatingWindows.remove(window);
    forgetWindow(window);
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

void Ki3Tiler::moveFocus(Qt::Edge edge)
{
    CustomTile *leaf = currentLeaf();
    if (!leaf) {
        return;
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
    CustomTile *target = leaf->nextNonLayoutTileAt(edge);
    if (!target || target->windows().isEmpty()) {
        return;
    }
    Window *self = leaf->windows().constFirst();
    Window *other = target->windows().constFirst();
    if (self == other) {
        return;
    }

    // Swap the two windows between their leaves (manage() evacuates first).
    target->manage(self);
    leaf->manage(other);
    m_leafForWindow[self] = target;
    m_leafForWindow[other] = leaf;
    m_lastFocusedLeaf = target;
    qCDebug(KWIN_KI3) << "move" << edge << "swap" << leaf->relativeGeometry() << "<->" << target->relativeGeometry();
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

void Ki3Tiler::toggleSplitDirection()
{
    m_splitDirection = (m_splitDirection == Tile::LayoutDirection::Horizontal)
        ? Tile::LayoutDirection::Vertical
        : Tile::LayoutDirection::Horizontal;
    qCInfo(KWIN_KI3) << "split direction ->"
                     << (m_splitDirection == Tile::LayoutDirection::Horizontal ? "horizontal" : "vertical");
}

void Ki3Tiler::toggleFloating()
{
    Window *window = workspace()->activeWindow();
    if (!window) {
        return;
    }
    if (m_floatingWindows.contains(window)) {
        // Re-tile it.
        m_floatingWindows.remove(window);
        qCDebug(KWIN_KI3) << "unfloat" << window->caption();
        insertWindow(window);
    } else if (m_leafForWindow.contains(window)) {
        // Detach from the tree; it keeps its current geometry and floats.
        m_floatingWindows.insert(window);
        qCDebug(KWIN_KI3) << "float" << window->caption();
        forgetWindow(window);
        window->requestTile(nullptr);
    }
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

LogicalOutput *Ki3Tiler::focusedOutput() const
{
    if (Window *active = workspace()->activeWindow()) {
        if (LogicalOutput *output = active->output()) {
            return output;
        }
    }
    return workspace()->activeOutput();
}

void Ki3Tiler::switchToWorkspace(int number)
{
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    if (vdm->count() < uint(number)) {
        vdm->setCount(number);
    }
    VirtualDesktop *desktop = vdm->desktops().value(number - 1);
    if (!desktop) {
        return;
    }
    qCDebug(KWIN_KI3) << "switch to workspace" << number << "on output" << (void *)focusedOutput();
    vdm->setCurrent(desktop, focusedOutput());
}

void Ki3Tiler::moveActiveToWorkspace(int number)
{
    Window *window = workspace()->activeWindow();
    if (!window || !shouldManage(window)) {
        return;
    }
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    if (vdm->count() < uint(number)) {
        vdm->setCount(number);
    }
    VirtualDesktop *desktop = vdm->desktops().value(number - 1);
    if (!desktop || window->isOnDesktop(desktop)) {
        return;
    }

    // Detach from the current desktop's tree, move it, then tile it into the
    // target desktop's tree (geometry is applied when that desktop is shown).
    forgetWindow(window);
    window->setDesktops({desktop});
    qCDebug(KWIN_KI3) << "move window to workspace" << number;
    insertWindow(window);
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
        qCDebug(KWIN_KI3) << "insert (root):" << window->caption() << "->" << root->windowGeometry();
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

    // i3 behaviour: if the focused leaf already lives in a layout running in the
    // current split direction, add the new window as a *sibling* and rebalance
    // all of them evenly (1:1:1...), rather than nesting + halving the focused
    // cell (which would give 2:1:1). Only nest when the direction differs.
    auto *parent = static_cast<CustomTile *>(target->parentTile());
    if (parent && parent->isLayout() && parent->layoutDirection() == m_splitDirection
        && target->childCount() == 0) {
        CustomTile *forNew = parent->createChildAt(target->relativeGeometry(),
                                                   m_splitDirection, target->row() + 1);
        forNew->manage(window);
        m_leafForWindow[window] = forNew;
        m_lastFocusedLeaf = forNew;
        redistributeEvenly(parent);
        qCDebug(KWIN_KI3) << "insert (sibling):" << window->caption()
                          << "siblings now" << parent->childCount()
                          << "->" << forNew->windowGeometry();
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
    qCDebug(KWIN_KI3) << "insert (split):" << window->caption() << "->" << forNew->windowGeometry();
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
}

} // namespace KWin
