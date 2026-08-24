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

    // Inside a tab/stack group, motion along the container's axis cycles the
    // visible tab (tabbed: left/right; stacked: up/down) instead of leaving it,
    // unless the active tab is already at that end. There we first try to
    // leave the group like a normal neighbour move, and only wrap within the
    // group as a fallback if there's truly nowhere else to go -- mirrors i3's
    // focus_wrapping: escape outward first, wrap only at a genuine dead end.
    if (auto it = m_tabbed.constFind(leaf); it != m_tabbed.constEnd()) {
        const bool horizontal = (edge == Qt::LeftEdge || edge == Qt::RightEdge);
        const bool alongAxis = (it->mode == ContainerMode::Tabbed) ? horizontal : !horizontal;
        if (alongAxis) {
            const int delta = (edge == Qt::RightEdge || edge == Qt::BottomEdge) ? +1 : -1;
            const bool atBoundary = (delta > 0) ? (it->active >= it->windows.size() - 1) : (it->active <= 0);
            if (!atBoundary || !leaveLeaf(leaf, edge)) {
                cycleTab(leaf, delta);
            }
            return;
        }
    }

    leaveLeaf(leaf, edge);
}

bool TileTreeController::leaveLeaf(CustomTile *leaf, Qt::Edge edge)
{
    // Neighbour within the same output's tree.
    if (CustomTile *target = leaf->nextNonLayoutTileAt(edge)) {
        if (!target->windows().isEmpty()) {
            qCDebug(KWIN_KI3) << "focus" << edge << leaf->relativeGeometry() << "->" << target->relativeGeometry();
            workspace()->activateWindow(target->windows().constFirst());
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

        // Motion along the container's axis (tabbed: left/right, stacked:
        // up/down) reorders the tab in place first, mirroring moveFocus()'s
        // cycleTab-before-leave semantics -- only once the active tab is
        // already at that end does the move fall through below to pop it out
        // toward the neighbouring leaf.
        const bool horizontal = (edge == Qt::LeftEdge || edge == Qt::RightEdge);
        const bool alongAxis = (srcGroup->mode == ContainerMode::Tabbed) ? horizontal : !horizontal;
        if (alongAxis) {
            const int delta = (edge == Qt::RightEdge || edge == Qt::BottomEdge) ? +1 : -1;
            const int newIdx = idx + delta;
            if (newIdx >= 0 && newIdx < srcGroup->windows.size()) {
                srcGroup->windows.swapItemsAt(idx, newIdx);
                srcGroup->active = newIdx;
                qCDebug(KWIN_KI3) << "tab reorder -> active" << newIdx;
                refreshGroup(leaf);
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

    // The neighbouring leaf in `edge` direction. A group is a single leaf, so
    // this neighbour is always *outside* the group — moving a tab in any
    // direction pops it out toward that neighbour (intra-group tab reordering
    // is a separate follow-up). If the neighbour is itself a group,
    // placeWindowAt joins it as a new tab (group-to-group move).
    CustomTile *target = leaf->nextNonLayoutTileAt(edge);
    if (!target || target->windows().isEmpty()) {
        // No existing tile to pop into. For a group member this is the common
        // case, not a true dead end: the tile that would receive it was very
        // often the window's own former position before it joined the group,
        // and that slot no longer exists (its leaf was collapsed on the way
        // in). Mirror i3: still eject, by splitting the group's own tile to
        // make room, instead of silently doing nothing.
        if (fromGroup) {
            ejectGroupMemberViaSplit(leaf, self, edge);
            return;
        }
        // A plain leaf with nowhere left in this output's tree: try the
        // adjacent output in that direction, mirroring leaveLeaf()'s
        // same-output-then-adjacent-output order for focus.
        moveWindowAcrossOutput(leaf, self, edge);
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
        Q_EMIT layoutChanged();
    } else if (fromGroup && m_tabbed.contains(leaf)) {
        refreshGroup(leaf); // group survived with remaining tabs: restack its header
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

void TileTreeController::ejectGroupMemberViaSplit(CustomTile *leaf, Window *self, Qt::Edge edge)
{
    auto srcGroup = m_tabbed.find(leaf);
    if (srcGroup == m_tabbed.end()) {
        return;
    }

    // The group's other surviving tabs; self is still listed in the group at
    // this point (moveWindow() only drops it once a target leaf is found,
    // which didn't happen here). If self was the group's only member there's
    // nothing to split off from and nowhere for it to have come from either.
    QList<Window *> remaining;
    for (const QPointer<Window> &w : srcGroup->windows) {
        if (w && w != self) {
            remaining.append(w);
        }
    }
    if (remaining.isEmpty()) {
        return;
    }

    // Split perpendicular to the edge: Left/Right make a new horizontal pair,
    // Top/Bottom a vertical one. CustomTile::split() always puts the
    // "before" half (left/top) at index 0 -- reusing `leaf` itself when the
    // parent already runs the same direction, or two brand-new tiles when it
    // has to nest a new sub-layout (see the CustomTile::split()/placeWindowAt
    // comments). Either way we treat both results as opaque and reattach
    // every window explicitly afterwards, exactly like placeWindowAt()'s own
    // split fallback does.
    const Tile::LayoutDirection direction =
        (edge == Qt::LeftEdge || edge == Qt::RightEdge) ? Tile::LayoutDirection::Horizontal
                                                        : Tile::LayoutDirection::Vertical;
    const QList<CustomTile *> created = leaf->split(direction);
    if (created.size() != 2) {
        qCWarning(KWIN_KI3) << "eject-from-group: unexpected split result, size" << created.size();
        return;
    }
    const bool selfLeadsGroup = (edge == Qt::LeftEdge || edge == Qt::TopEdge);
    CustomTile *ejectedSlot = selfLeadsGroup ? created.first() : created.last();
    CustomTile *groupSlot = selfLeadsGroup ? created.last() : created.first();

    for (Window *w : remaining) {
        attachWindow(w, groupSlot);
        m_leafForWindow[w] = groupSlot;
    }
    if (groupSlot != leaf) {
        // The group moved to a freshly created tile: migrate its TabState
        // (header included) to the new key. `leaf` itself is now either the
        // ejected window's plain tile (case 1, see CustomTile::split()) or a
        // defunct non-leaf layout node (case 2) -- neither should keep
        // driving the header, so drop its geometry-tracking connection and
        // any stale reserve it's still carrying from being the group's home
        // a moment ago (destroyGroupHeader() would also erase the m_tabbed
        // entry we're about to move ourselves, so do its other two jobs
        // directly instead of calling it). refreshGroup() only wires this
        // connection when it creates a *new* header, so with the header
        // carried over unchanged we have to reconnect it to groupSlot here.
        disconnect(leaf, &Tile::windowGeometryChanged, this, &TileTreeController::onGroupGeometryChanged);
        leaf->setHeaderReserve(0.0);

        TabState st = *srcGroup;
        st.windows.clear();
        for (Window *w : remaining) {
            st.windows.append(w);
        }
        st.active = std::clamp(st.active, 0, int(st.windows.size()) - 1);
        m_tabbed.erase(srcGroup);
        m_tabbed.insert(groupSlot, st);
        connect(groupSlot, &Tile::windowGeometryChanged, this, &TileTreeController::onGroupGeometryChanged, Qt::UniqueConnection);
        connect(groupSlot, &QObject::destroyed, this, &TileTreeController::onGroupTileDestroyed, Qt::UniqueConnection);
    } else {
        srcGroup->windows.clear();
        for (Window *w : remaining) {
            srcGroup->windows.append(w);
        }
        srcGroup->active = std::clamp(srcGroup->active, 0, int(srcGroup->windows.size()) - 1);
    }

    // ejectedSlot is now a plain leaf, never a group. If split() reused
    // `leaf` as its object (case 1, ejecting toward the "before" i.e.
    // left/top half -- see CustomTile::split()), it's still carrying the
    // header reserve from when it *was* the group's tile: left uncleared,
    // the window's content area stays shrunk by a header strip that's no
    // longer drawn there (the reported bug). A no-op when ejectedSlot is a
    // brand-new tile, which already defaults to 0.
    ejectedSlot->setHeaderReserve(0.0);

    attachWindow(self, ejectedSlot);
    m_leafForWindow[self] = ejectedSlot;
    self->setNoBorder(true);
    m_lastFocusedLeaf = ejectedSlot;

    qCDebug(KWIN_KI3) << "move" << edge << self->caption() << "(ejected from group via split)";
    refreshGroup(groupSlot);
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
    Q_EMIT layoutChanged();
}

void TileTreeController::toggleContainerLayout()
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

} // namespace KWin
