/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Focus/move/resize navigation acting on the tile tree (TileTreeController).
// Split out from ki3tiletreecontroller.cpp purely for file size; there is no
// behavioral seam between the two.

#include "ki3_logging.h"
#include "ki3tiletreecontroller.h"

#include "core/output.h"
#include "tiles/customtile.h"
#include "tiles/tilemanager.h"
#include "window.h"
#include "workspace.h"

#include <algorithm>

namespace KWin
{

void TileTreeController::moveFocus(Qt::Edge edge)
{
    CustomTile *leaf = currentLeaf();
    if (!leaf) {
        return;
    }

    // If `leaf` is itself a tab/stack group's currently active item (the
    // common case: a plain single-window tab, or a nested item whose own
    // internal navigation already ran out of room -- see leaveLeaf(), which
    // this same edge already went through when reached recursively), motion
    // along the container's axis cycles the visible tab (tabbed: left/right;
    // stacked: up/down) instead of leaving it, unless the active tab is
    // already at that end. There we first try to leave the group like a
    // normal neighbour move, and only wrap within the group as a fallback if
    // there's truly nowhere else to go -- mirrors i3's focus_wrapping:
    // escape outward first, wrap only at a genuine dead end.
    if (CustomTile *container = groupContainerFor(leaf)) {
        if (auto it = m_tabbed.constFind(container);
            it != m_tabbed.constEnd() && it->items.value(it->active) == leaf) {
            const bool horizontal = (edge == Qt::LeftEdge || edge == Qt::RightEdge);
            const bool alongAxis = (it->mode == ContainerMode::Tabbed) ? horizontal : !horizontal;
            if (alongAxis) {
                const int delta = (edge == Qt::RightEdge || edge == Qt::BottomEdge) ? +1 : -1;
                const bool atBoundary = (delta > 0) ? (it->active >= it->items.size() - 1) : (it->active <= 0);
                if (!atBoundary || !leaveLeaf(leaf, edge)) {
                    cycleTab(container, delta);
                }
                return;
            }
        }
    }

    leaveLeaf(leaf, edge);
}

bool TileTreeController::leaveLeaf(CustomTile *leaf, Qt::Edge edge)
{
    // Neighbour within the same output's tree. A tab/stack group counts as
    // one atomic leaf here too (nextGroupAwareTileAt()) instead of being
    // drilled into, and its currently visible tab's representative window is
    // what actually gets focused (activationTargetFor()). This also
    // transparently covers navigating *within* a nested tab item's own
    // split before ever reaching its enclosing group's boundary, since this
    // is a plain sibling lookup relative to `leaf`'s own immediate parent,
    // wherever that is.
    if (CustomTile *target = nextGroupAwareTileAt(leaf, edge)) {
        if (Window *w = activationTargetFor(target)) {
            qCDebug(KWIN_KI3) << "focus" << edge << leaf->relativeGeometry() << "->" << target->relativeGeometry();
            workspace()->activateWindow(w);
        }
        return true;
    }

    // At the output edge: cross to the adjacent output in that direction.
    return moveFocusAcrossOutput(leaf, edge);
}

bool TileTreeController::moveFocusAcrossOutput(CustomTile *leaf, Qt::Edge edge)
{
    TileManager *manager = leaf->manager();
    LogicalOutput *output = manager ? manager->output() : nullptr;
    if (!output) {
        return false;
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
        return false;
    }
    TileManager *nextManager = workspace()->tileManager(nextOutput);
    if (!nextManager) {
        return false;
    }
    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(nextOutput);
    RootTile *nextRoot = nextManager->rootTile(desktop);
    if (!nextRoot) {
        return false;
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
        return true;
    }
    return false;
}

void TileTreeController::moveWindow(Qt::Edge edge)
{
    CustomTile *leaf = currentLeaf();
    if (!leaf || leaf->windows().isEmpty()) {
        return;
    }

    // If `leaf` is a tab/stack group's own active item, the whole group is
    // the unit that gets navigated from (a plain sibling lookup on `leaf`
    // itself would be meaningless: its parent is the group's Floating,
    // fully-overlapping container -- see nextGroupAwareTileAt()'s doc
    // comment). A nested item's own internal move (something other than the
    // active tab, or the active tab not yet at its item's own boundary)
    // isn't a group-move at all and falls through to the plain leaf path
    // below unchanged.
    CustomTile *container = groupContainerFor(leaf);
    auto srcGroup = container ? m_tabbed.find(container) : m_tabbed.end();
    const bool fromGroup = srcGroup != m_tabbed.end() && srcGroup->items.value(srcGroup->active) == leaf;
    Window *self = nullptr;
    CustomTile *pivot = leaf;
    if (fromGroup) {
        pivot = container;
        self = representativeWindow(leaf);
        if (!self) {
            return;
        }

        // Motion along the container's axis (tabbed: left/right, stacked:
        // up/down) reorders the tab in place first, mirroring moveFocus()'s
        // cycleTab-before-leave semantics -- only once the active tab is
        // already at that end does the move fall through below to pop it out
        // toward the neighbouring leaf.
        const bool horizontal = (edge == Qt::LeftEdge || edge == Qt::RightEdge);
        const bool alongAxis = (srcGroup->mode == ContainerMode::Tabbed) ? horizontal : !horizontal;
        if (alongAxis) {
            const int delta = (edge == Qt::RightEdge || edge == Qt::BottomEdge) ? +1 : -1;
            const int newIdx = srcGroup->active + delta;
            if (newIdx >= 0 && newIdx < srcGroup->items.size()) {
                srcGroup->items.swapItemsAt(srcGroup->active, newIdx);
                srcGroup->active = newIdx;
                qCDebug(KWIN_KI3) << "tab reorder -> active" << newIdx;
                refreshGroup(container);
                workspace()->activateWindow(self);
                return;
            }
        }
    } else {
        self = leaf->windows().constFirst();
    }
    if (!self) {
        return;
    }

    // The neighbouring leaf in `edge` direction (relative to the group as a
    // whole when moving a tab out, else relative to `leaf` itself). If the
    // neighbour is itself a group, placeWindowAt joins it as a new tab
    // (group-to-group move).
    CustomTile *target = nextGroupAwareTileAt(pivot, edge);
    if (!target || (target->windows().isEmpty() && !m_tabbed.contains(target))) {
        // No existing tile to pop into. For a group member this is the common
        // case, not a true dead end: the tile that would receive it was very
        // often the window's own former position before it joined the group,
        // and that slot no longer exists (its leaf was collapsed on the way
        // in). Mirror i3: still eject, by splitting the group's own tile to
        // make room, instead of silently doing nothing.
        if (fromGroup) {
            ejectGroupMemberViaSplit(container, self, edge);
            return;
        }
        // A plain leaf with nowhere left in this output's tree: try the
        // adjacent output in that direction, mirroring leaveLeaf()'s
        // same-output-then-adjacent-output order for focus.
        moveWindowAcrossOutput(leaf, self, edge);
        return;
    }
    if (!m_tabbed.contains(target) && self == target->windows().constFirst()) {
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
    // nextGroupAwareTileAt() can hand back a tile in a differently-oriented
    // ancestor container.
    auto *targetParent = static_cast<CustomTile *>(target->parentTile());
    const bool insertBefore = targetParent
        && ((targetParent->layoutDirection() == Tile::LayoutDirection::Horizontal && edge == Qt::LeftEdge)
            || (targetParent->layoutDirection() == Tile::LayoutDirection::Vertical && edge == Qt::TopEdge));

    CustomTile *parent = static_cast<CustomTile *>(leaf->parentTile());
    auto *root = static_cast<RootTile *>(leaf->rootTile());

    // Moving out of a group: drop `leaf` (the item) from the group's
    // TabState up front so our bookkeeping stays consistent once
    // placeWindowAt re-homes the window. srcGroup is not reused after this.
    if (fromGroup) {
        srcGroup->items.removeAll(QPointer<CustomTile>(leaf));
        if (srcGroup->items.isEmpty()) {
            destroyGroupHeader(container); // last tab gone: drop header + clear reserve
            m_tabbed.erase(srcGroup);
        } else {
            srcGroup->active = std::clamp(srcGroup->active, 0, int(srcGroup->items.size()) - 1);
        }
    }

    placeWindowAt(self, target, insertBefore); // manage() evacuates `self` from `leaf` internally

    if (!leaf->isRoot() && leaf->childCount() == 0 && leaf->windows().isEmpty()) {
        qCDebug(KWIN_KI3) << "move: collapse empty leaf left by" << self->caption();
        leaf->remove();
        resyncLeafMapping(root);
        // A Floating parent (a group's own container) must never go through
        // this: redistributeEvenly() reads layoutDirection() to pick an axis
        // and would otherwise mis-arrange its overlapping children.
        if (parent && parent->isLayout() && parent->layoutDirection() != Tile::LayoutDirection::Floating) {
            redistributeEvenly(parent);
        }
        Q_EMIT layoutChanged();
    }
    if (fromGroup && m_tabbed.contains(container)) {
        refreshGroup(container); // group survived with remaining tabs: restack its header
    }
    qCDebug(KWIN_KI3) << "move" << edge << self->caption() << (fromGroup ? "(out of group)" : "");
    workspace()->activateWindow(self);
}

void TileTreeController::moveWindowAcrossOutput(CustomTile *leaf, Window *self, Qt::Edge edge)
{
    TileManager *manager = leaf->manager();
    LogicalOutput *output = manager ? manager->output() : nullptr;
    if (!output) {
        return;
    }
    const RectF geom = output->geometryF();
    const RectF leafGeom = leaf->absoluteGeometry();

    // Same edge probe as moveFocusAcrossOutput(): a point just beyond the
    // relevant edge of the current output, in real screen-geometry terms --
    // works for any physical arrangement (side by side, one above the
    // other, ...), not just left/right.
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
    qCDebug(KWIN_KI3) << "move cross-output probe" << edge << probe << "from" << (void *)output
                      << "-> nextOutput" << (void *)nextOutput;
    if (!nextOutput || nextOutput == output) {
        return;
    }
    // Fail fast, before mutating anything, if the target output turns out to
    // have no tile tree or no current desktop -- same guard order as
    // moveFocusAcrossOutput().
    if (!workspace()->tileManager(nextOutput)) {
        return;
    }
    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(nextOutput);
    if (!desktop) {
        return;
    }

    // Same "detach, retarget desktop/output, reattach" shape as
    // WorkspaceController::moveActiveToWorkspace() -- the other place a
    // tiled window crosses outputs. forgetWindow() collapses the leaf left
    // behind and restores self's pre-ki3 decoration baseline; insertWindow()
    // re-tiles it into the target (output, desktop) tree, using the same
    // empty-root/last-focused-leaf/first-leaf policy a brand-new window gets.
    forgetWindow(self);
    self->setDesktops({desktop});
    if (nextOutput != self->output()) {
        self->sendToOutput(nextOutput);
    }
    // Pass nextOutput explicitly: sendToOutput() above only updates
    // self->output() once the client acks the configure, so insertWindow()'s
    // own window->output() lookup could still see the old output here.
    insertWindow(self, nextOutput);

    qCDebug(KWIN_KI3) << "move" << edge << self->caption() << "across output ->" << (void *)nextOutput;
    workspace()->activateWindow(self);
}

void TileTreeController::ejectGroupMemberViaSplit(CustomTile *container, Window *self, Qt::Edge edge)
{
    auto srcGroup = m_tabbed.find(container);
    if (srcGroup == m_tabbed.end()) {
        return;
    }
    CustomTile *ejectedItem = srcGroup->items.value(srcGroup->active);
    if (!ejectedItem) {
        return;
    }
    QList<CustomTile *> remainingItems;
    for (const QPointer<CustomTile> &item : srcGroup->items) {
        if (item && item != ejectedItem) {
            remainingItems.append(item);
        }
    }
    // self was the group's only tab: nothing to split off from.
    if (remainingItems.isEmpty()) {
        return;
    }

    auto *containerParent = static_cast<CustomTile *>(container->parentTile());
    if (!containerParent) {
        qCWarning(KWIN_KI3) << "eject-from-group: group container has no parent to split into";
        return;
    }
    // Split perpendicular to the edge: Left/Right make a new horizontal pair,
    // Top/Bottom a vertical one. Left/Top ejects "before" `container` in the
    // parent's child order, mirroring CustomTile::split()'s own convention.
    const Tile::LayoutDirection direction =
        (edge == Qt::LeftEdge || edge == Qt::RightEdge) ? Tile::LayoutDirection::Horizontal
                                                        : Tile::LayoutDirection::Vertical;
    const bool selfLeadsGroup = (edge == Qt::LeftEdge || edge == Qt::TopEdge);

    // `container`'s remaining items can only be resized in place (no
    // reparenting needed -- see below) if `containerParent` either doesn't
    // yet have an established, *conflicting* direction of its own (root, a
    // lone child, or already the same axis) or is itself Floating (a group
    // nested inside another group's item). A real, differently-directioned
    // sibling of `container` under `containerParent` can't be disturbed
    // without reparenting `container`'s own children -- unsupported (there's
    // no Tile reparent primitive) -- so that specific shape bails out rather
    // than risk corrupting the tree. Rare: needs a group with no existing
    // same-output neighbour *and* sitting directly among differently-split
    // siblings. See ki3-PLAN.md.
    const bool compatibleParent = containerParent->childCount() < 2
        || containerParent->layoutDirection() == direction
        || containerParent->layoutDirection() == Tile::LayoutDirection::Floating;
    if (!compatibleParent) {
        qCDebug(KWIN_KI3) << "eject-from-group: parent's own split direction conflicts, "
                             "declining rather than reparenting";
        return;
    }

    // Every window in the ejected item's own subtree -- for the common case
    // (a plain single-window tab) this is just [self]. A genuinely nested
    // multi-window tab item ejected this way doesn't keep its internal split
    // structure (unlike a normal tab/stack collapse or a move onto an
    // *existing* neighbour tile, both of which preserve a subtree intact) --
    // a rare, explicitly-scoped corner case: there's no neighbour tile to
    // pop into *and* the popped-out tab was itself a nested split, so its
    // windows just fan out into a fresh even split instead. See ki3-PLAN.md.
    const QList<Window *> ejectedWindows = subtreeWindows(ejectedItem);

    // `container` keeps its own object identity as (a resized, still-Floating)
    // home for the surviving tabs -- every remaining item is still its real
    // child, so setGeometryRecursive() below remaps each one's own nested
    // split ratios into the smaller slice for free, no reinsertion needed.
    // Only the ejected item's tile is actually detached; a brand-new sibling
    // of `container` (under its own parent) becomes its new home.
    ejectedItem->remove();
    srcGroup->items.clear();
    for (CustomTile *item : std::as_const(remainingItems)) {
        srcGroup->items.append(item);
    }
    srcGroup->active = std::clamp(srcGroup->active, 0, int(srcGroup->items.size()) - 1);

    RectF containerGeo = container->relativeGeometry();
    RectF ejectedGeo = containerGeo;
    if (direction == Tile::LayoutDirection::Horizontal) {
        const qreal half = containerGeo.width() / 2;
        (selfLeadsGroup ? ejectedGeo : containerGeo).setWidth(half);
        (selfLeadsGroup ? containerGeo : ejectedGeo).setLeft(containerGeo.left() + half);
    } else {
        const qreal half = containerGeo.height() / 2;
        (selfLeadsGroup ? ejectedGeo : containerGeo).setHeight(half);
        (selfLeadsGroup ? containerGeo : ejectedGeo).setTop(containerGeo.top() + half);
    }

    setGeometryRecursive(container, containerGeo);
    const int position = selfLeadsGroup ? container->row() : container->row() + 1;
    CustomTile *ejectedSlot = containerParent->createChildAt(ejectedGeo, containerParent->layoutDirection(), position);

    CustomTile *target = ejectedSlot;
    for (int i = 0; i < ejectedWindows.size(); ++i) {
        Window *w = ejectedWindows[i];
        if (i > 0) {
            // A genuinely nested ejected item: fan the rest out into an even
            // split instead of dropping them (see the doc comment above).
            const auto created = target->split(direction == Tile::LayoutDirection::Horizontal
                                                   ? Tile::LayoutDirection::Vertical
                                                   : Tile::LayoutDirection::Horizontal);
            target = created.isEmpty() ? target : created.last();
        }
        attachWindow(w, target);
        m_leafForWindow[w] = target;
        w->setNoBorder(true);
    }
    m_lastFocusedLeaf = ejectedSlot;

    qCDebug(KWIN_KI3) << "move" << edge << self->caption() << "(ejected from group via split)";
    refreshGroup(container);
    Q_EMIT layoutChanged();
    workspace()->activateWindow(self);
}

void TileTreeController::resizeActive(Qt::Orientation orientation, qreal deltaPixels)
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

void TileTreeController::setSplitDirection(Tile::LayoutDirection direction)
{
    m_splitDirection = direction;
    qCInfo(KWIN_KI3) << "split direction ->"
                     << (m_splitDirection == Tile::LayoutDirection::Horizontal ? "horizontal" : "vertical");

    // True i3/sway "split h"/"split v" semantics: immediately turn the
    // *currently focused* leaf into a single-child layout of this direction
    // (wrapLeafInPlace()), so the next window opened here becomes a genuine
    // sibling in that direction -- instead of the old "remember direction
    // globally, apply it to whatever's focused whenever a window eventually
    // opens" approximation, which could apply to the wrong container if
    // focus moved on to something else in between.
    //
    // Skipped when `leaf` already has children (it's a tab/stack group
    // container, reachable only via the m_lastFocusedLeaf fallback in
    // currentLeaf() when there's no active window on the current desktop --
    // wrapping it here would corrupt the group, see wrapLeafInPlace()'s
    // precondition) or is already primed for this exact direction (pressing
    // the same direction twice in a row is a no-op until a second window
    // actually arrives, matching i3 -- this is exactly the condition
    // placeWindowAt()'s "sibling" fast path itself checks).
    CustomTile *leaf = currentLeaf();
    if (leaf && leaf->childCount() == 0 && !m_tabbed.contains(leaf)) {
        auto *parent = static_cast<CustomTile *>(leaf->parentTile());
        const bool alreadyPrimed = parent && parent->isLayout()
            && parent->layoutDirection() == direction;
        if (!alreadyPrimed) {
            CustomTile *child = wrapLeafInPlace(leaf, direction, direction);
            qCDebug(KWIN_KI3) << "split direction: wrapped" << leaf << "->" << child;
        }
    }
    Q_EMIT layoutChanged();
}

void TileTreeController::toggleContainerLayout()
{
    CustomTile *leaf = currentLeaf();
    if (!leaf) {
        return;
    }

    // The focused leaf is itself a tabbed/stacked group's active item:
    // collapse the group back to a plain split, mirroring
    // setContainerMode()'s same-key-toggles-back rule.
    if (CustomTile *container = groupContainerFor(leaf)) {
        if (auto it = m_tabbed.constFind(container);
            it != m_tabbed.constEnd() && it->items.value(it->active) == leaf) {
            untabContainer(container);
            return;
        }
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

} // namespace KWin
