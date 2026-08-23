/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3tiletreecontroller.h"
#include "ki3_logging.h"

#include "tiles/customtile.h"
#include "tiles/tilemanager.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

namespace KWin
{

TileTreeController::TileTreeController(QObject *parent)
    : QObject(parent)
    , m_nonTileableRules(loadNonTileableRules())
{
}

CustomTile *TileTreeController::currentLeaf() const
{
    // Every shortcut-driven action (move/focus/resize/container-mode) resolves
    // its target through here, so this is where "act on the current desktop
    // only" is enforced. Switching to an empty desktop leaves KWin's active
    // window -- and m_lastFocusedLeaf -- pointing at whatever was focused on the
    // *previous* desktop; returning that here would let a shortcut mutate the
    // layout on a desktop the user isn't even looking at. Tile::isActive() is
    // true only for the desktop currently shown on the leaf's output (so this
    // stays correct with per-output virtual desktops), and it matches the
    // isOnCurrentDesktop() visibility guard the split/focus/resize indicators
    // already apply.
    if (Window *active = workspace()->activeWindow();
        active != nullptr && active->isOnCurrentDesktop()) {
        const auto it = m_leafForWindow.constFind(active);
        if (it != m_leafForWindow.constEnd() && it.value()) {
            return it.value();
        }
    }
    if (m_lastFocusedLeaf && m_lastFocusedLeaf->isActive()) {
        return m_lastFocusedLeaf;
    }
    return nullptr;
}

bool TileTreeController::isManaged(Window *window) const
{
    return m_leafForWindow.contains(window);
}

CustomTile *TileTreeController::leafFor(Window *window) const
{
    return m_leafForWindow.value(window);
}

QList<Window *> TileTreeController::managedWindows() const
{
    QList<Window *> windows;
    for (auto it = m_leafForWindow.constBegin(); it != m_leafForWindow.constEnd(); ++it) {
        if (it.value()) {
            windows.append(it.key());
        }
    }
    return windows;
}

bool TileTreeController::isFloating(Window *window) const
{
    return m_floatingWindows.contains(window);
}

const QSet<Window *> &TileTreeController::floatingWindows() const
{
    return m_floatingWindows;
}

void TileTreeController::addFloating(Window *window)
{
    m_floatingWindows.insert(window);
}

void TileTreeController::removeFloating(Window *window)
{
    m_floatingWindows.remove(window);
}

bool TileTreeController::isGroup(CustomTile *tile) const
{
    return m_tabbed.contains(tile);
}

// True if some ordinary window stacked above @p window overlaps its frame — e.g.
// a modal dialog (a gpg pinentry) raised over its tiled parent. The leaf-edge
// overlays (focus/split/resize) live in KWin's AboveLayer and would otherwise
// paint *over* such a window, so callers hide the border while it is covered.
// Our own internal overlays and the outline are skipped (they always stack
// above), as are windows that aren't shown on the current desktop.
bool TileTreeController::leafWindowOccluded(Window *window) const
{
    const QList<Window *> &stack = workspace()->stackingOrder();
    const int idx = stack.indexOf(window);
    if (idx < 0) {
        return false;
    }
    // Expand by kIndicatorThickness on every side: outwardBorderStrips() draws
    // outside the window's own frame -- see Ki3Tiler::outwardBorderStrips() --
    // so a window whose edge only clips that outward margin -- not the frame
    // itself -- must still count as occluding, or the border keeps drawing
    // over its edge.
    const RectF geom = window->frameGeometry().adjusted(
        -kIndicatorThickness, -kIndicatorThickness, kIndicatorThickness, kIndicatorThickness);
    for (int i = idx + 1; i < stack.size(); ++i) {
        Window *above = stack.at(i);
        // isDeleted() first, deliberately, before isInternal()/isOutline()
        // (both virtual) -- see shouldManage()'s doc comment below for why.
        if (above->isDeleted() || above->isInternal() || above->isOutline()) {
            continue;
        }
        if (!above->isShown() || !above->isOnCurrentDesktop()) {
            continue;
        }
        // A window ki3 is about to tile itself, but hasn't gotten to yet, briefly
        // sits at its own unconstrained placement (typically ~fullscreen) between
        // being mapped and insertWindow() assigning it a real tile slot -- e.g. its
        // windowActivated can fire before its windowAdded is processed, so
        // currentLeaf() still resolves to the *previous* leaf right as the new
        // window happens to overlap it. That's transit, not a real dialog sitting
        // on top, so don't let it hide the border -- once insertWindow() places it,
        // the subsequent updateSplitIndicator() call re-checks with real geometry.
        if (!m_leafForWindow.contains(above) && shouldManage(above)) {
            continue;
        }
        if (above->frameGeometry().intersects(geom)) {
            return true;
        }
    }
    return false;
}

bool TileTreeController::isNonTileable(const Window *window) const
{
    for (const WindowRule &rule : m_nonTileableRules) {
        if (rule.matches(window)) {
            return true;
        }
    }
    return false;
}

bool TileTreeController::shouldManage(Window *window) const
{
    // isDeleted() checked first, deliberately, and by itself before any of
    // the other (virtual) checks below: it's the one non-virtual accessor
    // here (Window::isDeleted() -- window.h/.cpp -- a plain m_deleted flag
    // read, no vtable dispatch), so it's safe to call even if window's
    // vtable is in a bad way. Confirmed exploitable in practice: a
    // reconcileOutputs() -> retileHomelessWindows() pass during output
    // hot-unplug hit a Window still present in workspace()->windows() with
    // m_deleted == true whose *virtual* isClient() call below crashed with
    // a garbage vtable jump (landed in glibc's malloc arena -- see
    // ki3-PLAN.md for the full gdb trace). Whatever left that window in
    // this half-torn-down state, never touch anything virtual on it first.
    return window
        && !window->isDeleted()
        && window->isClient() // managed by KWin (has placement control)
        && !window->isInternal() // ki3's own overlays (split indicator, tab
                                 // headers) and other internal windows report
                                 // isClient() and windowType() == Normal too
        && window->isNormalWindow()
        && !window->isSpecialWindow()
        && window->isResizable()
        && window->output()
        // i3 treats sticky (all-desktops) windows as floating, never tiled;
        // apply the same policy to sticky *and* multi-desktop windows here.
        // rootForWindow() associates a tiled window with exactly one root
        // (window->desktops().constFirst()), so a window visible on several
        // desktops at once has no stable single root to belong to -- picking
        // "the first" (or, for all-desktops, "whichever is currently shown")
        // is not a real semantic and would apply the wrong tile geometry
        // across desktop switches (review finding M4).
        && window->desktops().size() == 1
        && !isNonTileable(window)
        && !m_floatingWindows.contains(window);
}

RootTile *TileTreeController::rootForWindow(Window *window, LogicalOutput *outputHint) const
{
    LogicalOutput *output = outputHint ? outputHint : window->output();
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

QSet<CustomTile *> TileTreeController::liveLeaves() const
{
    QSet<CustomTile *> leaves;
    for (auto it = m_leafForWindow.constBegin(); it != m_leafForWindow.constEnd(); ++it) {
        if (CustomTile *leaf = it.value()) {
            leaves.insert(leaf);
        }
    }
    return leaves;
}

void TileTreeController::ensureManaged(RootTile *root)
{
    if (m_managedRoots.contains(root)) {
        return;
    }
    m_managedRoots.insert(root);
    // Keyed by raw pointer; a root dies on output unplug AND on desktop prune
    // (TileManager deletes its per-desktop RootTile). Drop the entry the moment
    // it's destroyed so a recycled address can't masquerade as already-managed
    // (which would skip the default-layout teardown below). See the slot's doc.
    connect(root, &QObject::destroyed, this, &TileTreeController::onManagedRootDestroyed,
            Qt::UniqueConnection);

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

void TileTreeController::setGeometryRecursive(CustomTile *tile, const RectF &geom)
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

void TileTreeController::redistributeEvenly(CustomTile *parent)
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

void TileTreeController::resyncLeafMapping(RootTile *root)
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

CustomTile *TileTreeController::firstLeaf(RootTile *root)
{
    CustomTile *leaf = nullptr;
    root->visitDescendants([&leaf](Tile *t) {
        if (!leaf && t->childCount() == 0 && t != t->rootTile()) {
            leaf = static_cast<CustomTile *>(t);
        }
    });
    return leaf ? leaf : root;
}

void TileTreeController::notePresentationBaseline(Window *window)
{
    if (!window || m_originalPresentation.contains(window)) {
        return;
    }
    m_originalPresentation.insert(window, PresentationState{window->decorationPolicy(), window->keepAbove()});
}

void TileTreeController::restoreDecorationPolicy(Window *window)
{
    if (!window || window->isDeleted()) {
        return;
    }
    auto it = m_originalPresentation.constFind(window);
    window->setDecorationPolicy(it != m_originalPresentation.constEnd()
                                    ? it->decorationPolicy
                                    : DecorationPolicy::PreferredByClient);
}

void TileTreeController::restoreKeepAbove(Window *window)
{
    if (!window || window->isDeleted()) {
        return;
    }
    auto it = m_originalPresentation.constFind(window);
    window->setKeepAbove(it != m_originalPresentation.constEnd() && it->keepAbove);
}

void TileTreeController::dropPresentationBaseline(Window *window)
{
    m_originalPresentation.remove(window);
}

void TileTreeController::noteWindowActivated(Window *window)
{
    // Track the focused leaf so the *next* window splits the right container.
    // The freshly added window isn't in the map yet when its activation fires,
    // so this won't clobber the previous focus before insertWindow() runs.
    auto it = m_leafForWindow.constFind(window);
    if (it != m_leafForWindow.constEnd() && it.value()) {
        m_lastFocusedLeaf = it.value();
    }
}

void TileTreeController::setHeaderPalette(const Ki3HeaderPalette &palette)
{
    m_headerPalette = palette;
    for (auto it = m_tabbed.begin(); it != m_tabbed.end(); ++it) {
        if (it->header) {
            it->header->setPalette(m_headerPalette);
        }
    }
}

void TileTreeController::attachWindow(Window *window, CustomTile *leaf)
{
    // Snapshot the pre-ki3 decoration/keep-above state the first time this
    // window becomes ki3-owned -- every path that hands a window to ki3
    // (fresh insert, tab join, sibling split, move) funnels through here.
    notePresentationBaseline(window);
    leaf->manage(window);
    // Tile::manage() wires geometry (window->requestTile) only when the leaf's
    // desktop is currently shown on its output; onto a hidden destination it
    // skips it -- and for a window it evacuated from another tile it actively
    // clears the tile (tile.cpp:444-448). That path is hit routinely by a
    // cross-output move onto a background workspace, leaving the window untiled
    // (overlapping at its raw geometry) even once the desktop is shown. Re-request
    // the tile explicitly so the association survives; a no-op when manage()
    // already set it (active desktop) or if the window isn't actually managed.
    if (!window->isDeleted() && leaf->windows().contains(window)
        && window->requestedTile() != leaf) {
        window->requestTile(leaf);
    }
}

void TileTreeController::insertWindow(Window *window, LogicalOutput *outputHint)
{
    RootTile *root = rootForWindow(window, outputHint);
    if (!root) {
        qCWarning(KWIN_KI3) << "no root tile for" << window;
        return;
    }
    ensureManaged(root);

    // First window on this root fills it.
    if (root->childCount() == 0 && root->windows().isEmpty()) {
        attachWindow(window, root);
        m_leafForWindow[window] = root;
        m_lastFocusedLeaf = root;
        window->setNoBorder(true); // i3-style: tiled windows never show their own title bar
        qCDebug(KWIN_KI3) << "insert (root):" << window->caption() << "->" << root->windowGeometry();
        Q_EMIT layoutChanged();
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

void TileTreeController::placeWindowAt(Window *window, CustomTile *target, bool insertBefore)
{
    // If the chosen container is a tab/stack group, the window joins it as a
    // new tab rather than splitting the tree.
    if (auto it = m_tabbed.find(target); it != m_tabbed.end()) {
        attachWindow(window, target);
        it->windows.append(window);
        it->active = it->windows.size() - 1;
        m_leafForWindow[window] = target;
        m_lastFocusedLeaf = target;
        window->setNoBorder(true); // hide its own title bar; ki3's tab bar shows instead
        qCDebug(KWIN_KI3) << "insert (tab):" << window->caption() << "tabs now" << it->windows.size();
        refreshGroup(target);
        Q_EMIT layoutChanged();
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
        attachWindow(window, forNew);
        m_leafForWindow[window] = forNew;
        m_lastFocusedLeaf = forNew;
        window->setNoBorder(true);
        redistributeEvenly(parent);
        qCDebug(KWIN_KI3) << "insert (sibling):" << window->caption()
                          << "siblings now" << parent->childCount()
                          << "at position" << position
                          << "->" << forNew->windowGeometry();
        Q_EMIT layoutChanged();
        return;
    }

    // Capture the windows on the target *before* splitting: split() changes the
    // tile geometry, which makes KWin re-home the existing window on its own. We
    // re-assign them explicitly afterwards so our bookkeeping stays authoritative.
    const QList<Window *> existing = target->windows();

    const QList<CustomTile *> created = target->split(m_splitDirection);
    if (created.size() != 2) {
        qCWarning(KWIN_KI3) << "unexpected split result, size" << created.size();
        attachWindow(window, target);
        m_leafForWindow[window] = target;
        m_lastFocusedLeaf = target;
        window->setNoBorder(true);
        Q_EMIT layoutChanged();
        return;
    }

    CustomTile *forOld = created.first();
    CustomTile *forNew = created.last();

    // If the target became a layout, move its existing window(s) into the new
    // first child so every window stays on a leaf.
    if (forOld != target) {
        for (Window *w : existing) {
            attachWindow(w, forOld);
            m_leafForWindow[w] = forOld;
        }
    }

    attachWindow(window, forNew);
    m_leafForWindow[window] = forNew;
    m_lastFocusedLeaf = forNew;
    window->setNoBorder(true);
    qCDebug(KWIN_KI3) << "insert (split):" << window->caption() << "->" << forNew->windowGeometry();
    Q_EMIT layoutChanged();
}

void TileTreeController::forgetWindow(Window *window)
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
    // Leaving the tile tree: restore whatever decoration policy it had before
    // ki3 first touched it (see PresentationState), not a hardcoded default --
    // it may have been borderless already (user choice or a WindowRule).
    restoreDecorationPolicy(window);

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
            Q_EMIT layoutChanged();
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
    Q_EMIT layoutChanged();
}

void TileTreeController::purgeStaleRoots()
{
    // A removed output's TileManager (and its RootTiles) are destroyed, leaving
    // dangling raw pointers in m_managedRoots. Keep only roots that still belong
    // to a live TileManager. Pointer identity comparison never derefs the dead
    // ones, so this is safe.
    QSet<RootTile *> valid;
    const auto outputs = workspace()->outputs();
    const auto desktops = VirtualDesktopManager::self()->desktops();
    for (LogicalOutput *output : outputs) {
        TileManager *tm = workspace()->tileManager(output);
        if (!tm) {
            continue;
        }
        for (VirtualDesktop *desktop : desktops) {
            if (RootTile *root = tm->rootTile(desktop)) {
                valid.insert(root);
            }
        }
    }
    m_managedRoots.intersect(valid);
}

void TileTreeController::onManagedRootDestroyed(QObject *tile)
{
    // Bare key only (mid-destruction); never dereferenced. See the header doc.
    if (m_managedRoots.remove(static_cast<RootTile *>(tile)) > 0) {
        qCDebug(KWIN_KI3) << "managed root destroyed; dropped stale entry";
    }
}

void TileTreeController::detachAllManagedWindows()
{
    // Floating windows: hand back keep-above/decoration. Chrome (still
    // Ki3Tiler-owned) must already be gone by the time this runs.
    const auto floatingWindows = m_floatingWindows;
    for (Window *window : floatingWindows) {
        restoreKeepAbove(window);
        restoreDecorationPolicy(window);
    }
    m_floatingWindows.clear();

    // Tiled windows: restore decoration and detach from whatever tile they're
    // still on. A null QPointer here means that tile's root already died
    // (e.g. an unplugged output) without a reconcile pass catching it -- the
    // window itself is still live and still needs its decoration restored,
    // there's just nothing left to call forget() on.
    const auto tiledWindows = m_leafForWindow.keys();
    for (Window *window : tiledWindows) {
        restoreDecorationPolicy(window);
        if (CustomTile *leaf = m_leafForWindow.value(window)) {
            leaf->forget(window); // clears the tile's window list *and* the
                                  // window's requested tile so remove() below
                                  // can't re-home it into a sibling
        }
    }
    m_leafForWindow.clear();
    m_lastFocusedLeaf = nullptr;

    // Tab/stack group headers must go before their tiles do.
    const auto groupTiles = m_tabbed.keys();
    for (CustomTile *tile : groupTiles) {
        destroyGroupHeader(tile);
    }
    m_tabbed.clear();
}

void TileTreeController::dropManagedRoots()
{
    // Drop ki3's own split structure from every surviving root so the next
    // load (or KWin's own default layout) starts clean instead of piling new
    // splits on top of stale ones. Every leaf's windows are already forgotten
    // by detachAllManagedWindows(), so remove() has nothing left to migrate; a
    // leaf that a sibling's removal already collapsed (see CustomTile::remove()'s
    // single-child promotion) is simply parentless by the time we reach it, and
    // remove() on an already-detached tile is a no-op.
    purgeStaleRoots();
    const auto roots = m_managedRoots;
    for (RootTile *root : roots) {
        QList<CustomTile *> leaves;
        root->visitDescendants([&leaves](Tile *t) {
            if (t->childCount() == 0 && !t->isRoot()) {
                leaves.append(static_cast<CustomTile *>(t));
            }
        });
        for (CustomTile *leaf : leaves) {
            leaf->remove();
        }
    }
    m_managedRoots.clear();
}

} // namespace KWin
