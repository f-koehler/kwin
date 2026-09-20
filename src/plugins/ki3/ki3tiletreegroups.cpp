/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3_logging.h"
#include "ki3tiletreecontroller.h"

#include "tiles/customtile.h"
#include "window.h"
#include "workspace.h"

#include <QScopeGuard>

namespace KWin
{

// Applies @p px as Tile::headerReserve() to every leaf within @p item's
// subtree whose own top edge coincides with @p item's -- i.e. it's visually
// adjacent to the header strip drawn above the whole item -- and clears it
// (0px) on every other leaf. Necessary because under the subtree-item model
// an item's own tile may have no windows of its own to shrink (a nested
// item's windows live on its own internal leaves -- see TabState::items'
// doc comment), and a leaf that *isn't* at the item's own top (e.g. the
// bottom half of a nested vertical split) must not also get the reserve, or
// it would be double-shrunk on top of its own internal split boundary.
// Exact: Tile::headerReserve() itself works purely in absolute pixels, so
// this never needs (and never introduces) the rounding a relative-geometry
// resize would.
static void applyHeaderReserveToTopLeaves(CustomTile *item, qreal px)
{
    if (!item) {
        return;
    }
    const qreal top = item->relativeGeometry().top();
    item->visitDescendants([top, px](Tile *t) {
        if (t->childCount() != 0) {
            return;
        }
        auto *leaf = static_cast<CustomTile *>(t);
        // +1.0 sidesteps qFuzzyCompare()'s documented unreliability for
        // values very close to zero (a leaf flush with the output's own top
        // edge has top() == 0.0).
        leaf->setHeaderReserve(qFuzzyCompare(leaf->relativeGeometry().top() + 1.0, top + 1.0) ? px : 0.0);
    });
}

void TileTreeController::setContainerMode(ContainerMode mode)
{
    CustomTile *leaf = currentLeaf();
    if (!leaf) {
        return;
    }

    // Already a tab/stack group (leaf is that group's own active item -- see
    // groupContainerFor()'s doc comment on why this can't just be
    // m_tabbed.contains(leaf) any more): the same key toggles back to a
    // split, the other key flips the mode in place.
    if (CustomTile *existingContainer = groupContainerFor(leaf)) {
        if (auto it = m_tabbed.find(existingContainer);
            it != m_tabbed.end() && it->items.value(it->active) == leaf) {
            if (it->mode == mode) {
                untabContainer(existingContainer);
            } else {
                it->mode = mode;
                qCInfo(KWIN_KI3) << "container mode ->" << (mode == ContainerMode::Tabbed ? "tabbed" : "stacked");
                refreshGroup(existingContainer);
                Q_EMIT layoutChanged();
            }
            return;
        }
    }

    // The container to collapse is the focused leaf's parent layout, or the leaf
    // itself when it is the root (single window: nothing to collapse yet, but we
    // still record the mode so a later-opened window joins as a 2nd tab).
    CustomTile *container = static_cast<CustomTile *>(leaf->parentTile());
    QList<CustomTile *> items;
    Tile::LayoutDirection prevSplit = Tile::LayoutDirection::Horizontal;

    if (!container || !container->isLayout() || container->childCount() < 2) {
        // No existing siblings to group with -- wrap the focused leaf as the
        // sole tab item first (same primitive setSplitDirection() uses to
        // prepare a leaf for a same-direction sibling), so a later-inserted
        // window has a real group tile (`leaf`, now Floating) to join.
        container = leaf;
        items = {wrapLeafInPlace(container, Tile::LayoutDirection::Floating, Tile::LayoutDirection::Floating)};
    } else {
        // The container's existing children become the tab items *as-is* --
        // each keeps its own subtree intact (a nested split stays nested)
        // instead of flattening every descendant window into one flat tab
        // list the way this used to work. Only their *geometry* changes
        // (below): every item is resized to fill the whole container so the
        // active one appears to occupy it entirely, matching how KWin
        // already renders several windows sharing one tile.
        prevSplit = (container->layoutDirection() == Tile::LayoutDirection::Vertical)
            ? Tile::LayoutDirection::Vertical
            : Tile::LayoutDirection::Horizontal;
        for (Tile *child : container->childTiles()) {
            items.append(static_cast<CustomTile *>(child));
        }
    }

    Window *active = workspace()->activeWindow();
    container->setLayoutDirection(Tile::LayoutDirection::Floating);
    for (CustomTile *item : std::as_const(items)) {
        setGeometryRecursive(item, container->relativeGeometry());
        for (Window *w : subtreeWindows(item)) {
            w->setNoBorder(true); // hide native title bars; ki3's tab bar shows instead
        }
    }

    TabState st;
    st.mode = mode;
    st.prevSplit = prevSplit;
    for (CustomTile *item : std::as_const(items)) {
        st.items.append(item);
    }
    st.active = 0;
    for (int i = 0; i < items.size(); ++i) {
        if (subtreeWindows(items[i]).contains(active)) {
            st.active = i;
            break;
        }
    }
    m_tabbed.insert(container, st);
    // Drop the entry the instant the container tile is destroyed (e.g. an
    // output unplug tears down its tile tree), so the raw-pointer key never
    // dangles. UniqueConnection dedupes if the same tile is re-tabbed later.
    connect(container, &QObject::destroyed, this, &TileTreeController::onGroupTileDestroyed,
            Qt::UniqueConnection);
    m_lastFocusedLeaf = container;

    qCInfo(KWIN_KI3) << (mode == ContainerMode::Tabbed ? "tabbed" : "stacked")
                     << items.size() << "items; active" << st.active;
    refreshGroup(container);
    Q_EMIT layoutChanged();
}

void TileTreeController::untabContainer(CustomTile *tile)
{
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end()) {
        return;
    }
    QList<CustomTile *> items;
    for (const QPointer<CustomTile> &item : it->items) {
        if (item) {
            items.append(item);
        }
    }
    const Tile::LayoutDirection prevSplit = it->prevSplit;
    Window *activeWindow = representativeWindow(it->items.value(it->active));
    destroyGroupHeader(tile); // drop the header + clear the tile's header reserve
    m_tabbed.erase(it);

    if (items.size() < 2) {
        // A lone (or empty) group: fold its sole item's subtree back up into
        // `tile` itself (undo wrapLeafInPlace()) rather than leaving a
        // pointless single-child layout behind. An empty group (shouldn't
        // normally happen -- forgetWindow()/removeGroupItem() erase it
        // instead once it hits zero) just falls through with nothing to do.
        if (items.size() == 1 && items.first()->parentTile() == tile) {
            CustomTile *item = items.first();
            const QList<Window *> windows = item->windows();
            const bool itemIsLayout = item->childCount() > 0;
            if (!itemIsLayout) {
                for (Window *w : windows) {
                    attachWindow(w, tile);
                    m_leafForWindow[w] = tile;
                }
                item->remove();
                tile->setLayoutDirection(prevSplit);
                if (m_lastFocusedLeaf == item) {
                    m_lastFocusedLeaf = tile;
                }
            } else {
                // The sole item is itself a real nested split -- keep it as
                // a genuine child rather than trying to merge two levels of
                // structure into one tile; just drop the group's own
                // Floating wrapper direction so a later split/insert next to
                // it behaves normally. `tile` stays a single-child layout
                // (the same, already-supported shape setSplitDirection()'s
                // wrap produces), no separate un-wrap needed.
                tile->setLayoutDirection(prevSplit);
                m_lastFocusedLeaf = item;
            }
        }
        Q_EMIT layoutChanged();
        return;
    }

    // 2+ items: turn the Floating (overlapping) container into a genuine
    // split of `prevSplit`, redistributing the *existing* item tiles evenly
    // -- redistributeEvenly()/setGeometryRecursive() remap each item's own
    // internal structure proportionally into its new (smaller) slice, so a
    // nested item's split ratios survive the round trip untouched. No
    // reinsertion needed: the items were always real children of `tile`.
    tile->setLayoutDirection(prevSplit);
    redistributeEvenly(tile);
    m_lastFocusedLeaf = activeWindow && m_leafForWindow.contains(activeWindow)
        ? m_leafForWindow.value(activeWindow)
        : QPointer<CustomTile>(tile);
    qCInfo(KWIN_KI3) << "untab" << items.size() << "items back to split"
                     << (prevSplit == Tile::LayoutDirection::Horizontal ? "H" : "V");
    Q_EMIT layoutChanged();
}

void TileTreeController::cycleTab(CustomTile *tile, int delta)
{
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end() || it->items.isEmpty()) {
        return;
    }
    const int n = it->items.size();
    it->active = ((it->active + delta) % n + n) % n; // wrap both ways
    Window *active = representativeWindow(it->items[it->active]);
    qCDebug(KWIN_KI3) << "tab cycle -> active" << it->active;
    refreshGroup(tile);
    if (active) {
        workspace()->activateWindow(active);
    }
}

void TileTreeController::updateTabVisibility(CustomTile *tile)
{
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end()) {
        return;
    }
    TabState &st = it.value();

    // Drop items that vanished, keeping active pointing at a live one.
    // Items are normally removed explicitly (removeGroupItem()), so this is
    // defensive -- mirrors the old flat design's same defensive prune.
    for (int i = st.items.size() - 1; i >= 0; --i) {
        if (!st.items[i]) {
            st.items.removeAt(i);
            if (i < st.active || (i == st.active && st.active > 0)) {
                --st.active;
            }
        }
    }
    if (st.items.isEmpty()) {
        m_tabbed.erase(it);
        return;
    }
    st.active = std::clamp(st.active, 0, int(st.items.size()) - 1);

    // Raise every window in the active item's subtree above every other
    // item's windows. All items share the container's full rect, so this is
    // exactly the old "visibility by stacking" scheme, just applied to a
    // (possibly multi-window) subtree instead of a single window.
    //
    // Exception: if focus is currently on a window ki3 doesn't manage (a
    // floating dialog such as a gpg/pinentry prompt, or a user-floated
    // window), never raise over it. handleWindowActivated() refreshes every
    // group on each activation, so without this guard a freshly mapped
    // pinentry dialog gets buried under this group's stale active tab the
    // instant it steals focus.
    CustomTile *activeItem = st.items[st.active];
    const QList<Window *> activeWindows = subtreeWindows(activeItem);
    if (!activeWindows.isEmpty()) {
        Window *globalActive = workspace()->activeWindow();
        const bool wouldBuryFocusedDialog = globalActive && !activeWindows.contains(globalActive)
            && !m_leafForWindow.contains(globalActive);
        if (!wouldBuryFocusedDialog) {
            for (Window *w : activeWindows) {
                workspace()->raiseWindow(w);
            }
        }
    }
    qCDebug(KWIN_KI3) << "tab visibility:" << st.items.size() << "tabs, active" << st.active;
}

void TileTreeController::refreshGroup(CustomTile *tile)
{
    // setHeaderReserve() below emits windowGeometryChanged, which we listen to;
    // ignore that re-entry *for this tile* — the outer call finishes against
    // settled geometry. A refresh of a different group nested in the cascade is
    // still allowed to proceed.
    if (m_refreshingGroups.contains(tile)) {
        return;
    }
    m_refreshingGroups.insert(tile);
    auto guard = qScopeGuard([this, tile] {
        m_refreshingGroups.remove(tile);
    });

    // Prune dead items, raise the active tab (shared T0 logic). May erase the
    // group if it emptied.
    updateTabVisibility(tile);
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end()) {
        destroyGroupHeader(tile);
        return;
    }
    TabState &st = it.value();

    const bool stacked = (st.mode == ContainerMode::Stacked);
    const qreal headerPx = Ki3Header::heightForTabs(st.items.size(), stacked);

    // Reserve the header strip. Tile::setHeaderReserve()'s own auto-resize
    // only affects windows managed *directly* on the reserve-holding tile,
    // which under the subtree-item model (see TabState::items' doc comment)
    // is never `tile` itself -- so it's kept here purely for windowGeometry()'s
    // header-*position* math below (content.top() needs to already reflect
    // the reserve for the header to land exactly above it, not past the
    // tile's own edge). The windows themselves are pushed down/shrunk by
    // applying the *same* headerReserve directly to whichever leaf(ves)
    // within each item's own subtree actually sit at its top edge, next --
    // exact (Tile::headerReserve() works in absolute pixels already), unlike
    // a relative-geometry resize computed from a headerPx/outputHeight
    // fraction, which rounds and was off by a couple of px in practice.
    // Every item is *also* kept resized to `tile`'s own current geometry
    // (unrelated to the header -- CustomTile::setRelativeGeometry()'s
    // generic Floating-child handling only intersects with the old geometry,
    // which isn't a real substitute for "always fill the parent" here) so a
    // sibling closing/appearing elsewhere in the tree that resizes `tile`
    // keeps every item filling it exactly, nested split ratios included.
    tile->setHeaderReserve(headerPx);
    for (const QPointer<CustomTile> &item : std::as_const(st.items)) {
        if (!item) {
            continue;
        }
        setGeometryRecursive(item, tile->relativeGeometry());
        applyHeaderReserveToTopLeaves(item, headerPx);
    }

    if (!st.header) {
        st.header = std::make_shared<Ki3Header>();
        st.header->setPalette(m_headerPalette);
        connect(st.header.get(), &Ki3Header::tabActivated, this,
                [this, tile](int index) {
            activateTab(tile, index);
        });
        // Reposition the header whenever the group tile's geometry changes (a
        // sibling closing/resizing redistributes our ancestor).
        connect(tile, &Tile::windowGeometryChanged, this,
                &TileTreeController::onGroupGeometryChanged, Qt::UniqueConnection);
    }

    // Hide the header when the visible tab isn't actually on screen.
    Window *active = representativeWindow(st.items[st.active]);
    if (!active || !active->isShown() || !active->isOnCurrentDesktop()) {
        st.header->hide();
        return;
    }

    // The header sits directly above the reserved window area, extended by
    // m_indicatorThickness on each side to line up with the tile border's
    // left/right strips (see outwardBorderStrips() in ki3decorationcontroller.cpp)
    // instead of falling short of them -- same fix as the floating title bar's
    // repositionFloatChrome() got earlier. Pushed in by
    // DecorationController::applyIndicatorColors()/reloadConfig() via
    // setIndicatorThickness() (ki3rc [General] BorderThickness).
    const RectF content = tile->windowGeometry();
    const QRectF headerRect(content.left() - m_indicatorThickness, content.top() - headerPx,
                            content.width() + 2 * m_indicatorThickness, headerPx);

    QStringList titles;
    titles.reserve(st.items.size());
    for (const QPointer<CustomTile> &item : st.items) {
        Window *w = item ? representativeWindow(item) : nullptr;
        titles << (w ? w->caption() : QString());
    }
    const bool focused = (workspace()->activeWindow() == active);
    st.header->setGeometry(headerRect.toRect());
    st.header->setTabs(titles, st.active, stacked, focused);
    st.header->show();

    qCDebug(KWIN_KI3) << "group header:" << st.items.size() << (stacked ? "stacked" : "tabbed")
                      << "active" << st.active << "reserve" << headerPx;
}

void TileTreeController::refreshAllGroups()
{
    const auto tiles = m_tabbed.keys(); // snapshot: refreshGroup may erase
    for (CustomTile *tile : tiles) {
        if (m_tabbed.contains(tile)) {
            refreshGroup(tile);
        }
    }
}

void TileTreeController::destroyGroupHeader(CustomTile *tile)
{
    if (!tile) {
        return;
    }
    disconnect(tile, &Tile::windowGeometryChanged, this, &TileTreeController::onGroupGeometryChanged);
    tile->setHeaderReserve(0.0);
    // The Ki3Header itself is owned by the TabState's shared_ptr and dies when
    // the caller erases the group from m_tabbed.
}

void TileTreeController::dropGroup(CustomTile *tile)
{
    // Combines destroyGroupHeader() with the m_tabbed erase, for a caller (see
    // WorkspaceController::teardownGroupsOnOutput()) that needs both done
    // together and has no other reason to reach into the group model.
    destroyGroupHeader(tile);
    m_tabbed.remove(tile);
}

void TileTreeController::onGroupGeometryChanged()
{
    if (auto *tile = qobject_cast<CustomTile *>(sender()); tile && m_tabbed.contains(tile)) {
        refreshGroup(tile);
    }
}

void TileTreeController::onGroupTileDestroyed(QObject *tile)
{
    // The tile is mid-destruction; use it as a bare key only (no dereference).
    // qobject_cast would already return nullptr here, so cast statically.
    auto it = m_tabbed.find(static_cast<CustomTile *>(tile));
    if (it == m_tabbed.end()) {
        return;
    }
    // This slot runs reentrantly, synchronously nested inside the dying
    // tile's own QObject destructor -- see
    // DecorationController::onTileBorderDestroyed()'s doc comment
    // (ki3decorationcontroller.cpp) for the full trace of why actually tearing
    // down TabState::header (a Ki3Header, itself a QWindow) right here is
    // unsafe: it cascades into Workspace::windowRemoved, which every live
    // Tile relays into unmanage(), and can reach back into this exact
    // dying tile. Drop the map entry now, but let the header (and its
    // window) actually die on the next event loop turn.
    TabState st = std::move(it.value());
    m_tabbed.erase(it);
    QMetaObject::invokeMethod(this, [st = std::move(st)]() {
        qCDebug(KWIN_KI3) << "tab group: deferred teardown of stale entry running";
    }, Qt::QueuedConnection);
    qCDebug(KWIN_KI3) << "tab group tile destroyed; dropped stale entry";
}

void TileTreeController::activateTab(CustomTile *tile, int index)
{
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end() || index < 0 || index >= it->items.size()) {
        return;
    }
    it->active = index;
    Window *window = representativeWindow(it->items[index]);
    qCDebug(KWIN_KI3) << "tab click -> active" << index;
    refreshGroup(tile);
    if (window) {
        workspace()->activateWindow(window);
    }
}

} // namespace KWin
