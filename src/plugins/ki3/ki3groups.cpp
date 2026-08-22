/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3_logging.h"
#include "ki3header.h"
#include "ki3tiler.h"

#include "tiles/customtile.h"
#include "window.h"
#include "workspace.h"

#include <QScopeGuard>

namespace KWin
{

void Ki3Tiler::setContainerMode(ContainerMode mode)
{
    CustomTile *leaf = currentLeaf();
    if (!leaf) {
        return;
    }

    // Already a tab/stack group: the same key toggles back to a split, the other
    // key flips the mode in place.
    if (auto it = m_tabbed.find(leaf); it != m_tabbed.end()) {
        if (it->mode == mode) {
            untabContainer(leaf);
        } else {
            it->mode = mode;
            qCInfo(KWIN_KI3) << "container mode ->" << (mode == ContainerMode::Tabbed ? "tabbed" : "stacked");
            refreshGroup(leaf);
            updateSplitIndicator();
        }
        return;
    }

    // The container to collapse is the focused leaf's parent layout, or the leaf
    // itself when it is the root (single window: nothing to collapse, but we
    // still record the mode so a later-opened window joins as a tab).
    auto *container = static_cast<CustomTile *>(leaf->parentTile());
    if (!container) {
        container = leaf;
    }
    Window *active = workspace()->activeWindow();

    // Remember the container's split direction so untab can restore it
    // (prev_split_layout). Only h/v are meaningful; default to horizontal.
    const Tile::LayoutDirection prevSplit =
        (container->layoutDirection() == Tile::LayoutDirection::Vertical)
        ? Tile::LayoutDirection::Vertical
        : Tile::LayoutDirection::Horizontal;

    // Every window in the container's subtree, in tree order (visitDescendants
    // includes the container itself, covering the single-window root case).
    QList<Window *> windows;
    container->visitDescendants([&windows](Tile *t) {
        if (t->childCount() == 0) {
            for (Window *w : t->windows()) {
                if (!windows.contains(w)) {
                    windows.append(w);
                }
            }
        }
    });
    if (windows.isEmpty()) {
        return;
    }

    // Move every window into the container tile (they now share its full rect),
    // then tear the empty child leaves down so the container becomes one leaf
    // owning them all. Evacuating BEFORE removing is essential: CustomTile::
    // remove() re-homes any window still on a removed tile via pick() and can
    // cascade-promote a lone survivor (customtile.cpp:326-340) — we must never
    // let it see a live window.
    for (Window *w : windows) {
        container->manage(w);
    }
    while (container->childCount() > 0) {
        CustomTile *victim = nullptr;
        container->visitDescendants([&victim, container](Tile *t) {
            if (!victim && t != container && t->childCount() == 0) {
                victim = static_cast<CustomTile *>(t);
            }
        });
        if (!victim) {
            break; // safety: no removable leaf found
        }
        victim->remove();
    }

    TabState st;
    st.mode = mode;
    st.prevSplit = prevSplit;
    for (Window *w : windows) {
        st.windows.append(w);
        m_leafForWindow[w] = container;
        // Hide the window's own title bar so only ki3's tab/stack header shows
        // (the i3 look). No-op for client-side-decorated apps.
        w->setNoBorder(true);
    }
    st.active = std::max(0, int(windows.indexOf(active)));
    m_tabbed.insert(container, st);
    // Drop the entry the instant the container tile is destroyed (e.g. an
    // output unplug tears down its tile tree), so the raw-pointer key never
    // dangles. UniqueConnection dedupes if the same tile is re-tabbed later.
    connect(container, &QObject::destroyed, this, &Ki3Tiler::onGroupTileDestroyed,
            Qt::UniqueConnection);
    m_lastFocusedLeaf = container;

    qCInfo(KWIN_KI3) << (mode == ContainerMode::Tabbed ? "tabbed" : "stacked")
                     << windows.size() << "windows; active" << st.active;
    refreshGroup(container);
    updateSplitIndicator();
}

void Ki3Tiler::untabContainer(CustomTile *tile)
{
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end()) {
        return;
    }
    QList<Window *> windows;
    for (const QPointer<Window> &w : it->windows) {
        if (w) {
            windows.append(w);
        }
    }
    const Tile::LayoutDirection prevSplit = it->prevSplit;
    destroyGroupHeader(tile); // drop the header + clear the tile's header reserve
    m_tabbed.erase(it);
    // Windows here stay in the tile tree (as the lone leaf, or re-inserted
    // below into a split) — since default/split layouts hide title bars too
    // (see ki3-PLAN.md 2026-07-04), they must stay borderless, not regain
    // their native SSD title bar.
    if (windows.size() < 2) {
        return; // a lone (or empty) group is already a plain leaf
    }

    // Keep the first window on the tile; detach the rest and feed them back
    // through the normal split path so they fan out into an even split. Restore
    // the pre-tab split direction (prev_split_layout) for the duration.
    Window *first = windows.constFirst();
    for (int i = 1; i < windows.size(); ++i) {
        tile->forget(windows[i]);
        m_leafForWindow.remove(windows[i]);
    }
    m_leafForWindow[first] = tile;
    m_lastFocusedLeaf = tile;
    const Tile::LayoutDirection savedDirection = m_splitDirection;
    m_splitDirection = prevSplit;
    qCInfo(KWIN_KI3) << "untab" << windows.size() << "windows back to split"
                     << (prevSplit == Tile::LayoutDirection::Horizontal ? "H" : "V");
    for (int i = 1; i < windows.size(); ++i) {
        insertWindow(windows[i]);
    }
    m_splitDirection = savedDirection;
    updateSplitIndicator();
}

void Ki3Tiler::cycleTab(CustomTile *tile, int delta)
{
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end() || it->windows.isEmpty()) {
        return;
    }
    const int n = it->windows.size();
    it->active = ((it->active + delta) % n + n) % n; // wrap both ways
    Window *active = it->windows[it->active];
    qCDebug(KWIN_KI3) << "tab cycle -> active" << it->active;
    refreshGroup(tile);
    if (active) {
        workspace()->activateWindow(active);
    }
}

void Ki3Tiler::updateTabVisibility(CustomTile *tile)
{
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end()) {
        return;
    }
    TabState &st = it.value();

    // Drop windows that vanished, keeping active pointing at a live tab.
    for (int i = st.windows.size() - 1; i >= 0; --i) {
        if (!st.windows[i]) {
            st.windows.removeAt(i);
            if (i < st.active || (i == st.active && st.active > 0)) {
                --st.active;
            }
        }
    }
    if (st.windows.isEmpty()) {
        m_tabbed.erase(it);
        return;
    }
    st.active = std::clamp(st.active, 0, int(st.windows.size()) - 1);

    // Raise the active tab above its peers. All group windows share the tile
    // rect, so raising the active one occludes the rest (T0: visibility by
    // stacking; truly hiding inactive tabs is a later refinement).
    //
    // Exception: if focus is currently on a window ki3 doesn't manage (a
    // floating dialog such as a gpg/pinentry prompt, or a user-floated
    // window), never raise over it. handleWindowActivated() refreshes every
    // group on each activation, so without this guard a freshly mapped
    // pinentry dialog gets buried under this group's stale active tab the
    // instant it steals focus.
    if (Window *active = st.windows[st.active]) {
        Window *globalActive = workspace()->activeWindow();
        const bool wouldBuryFocusedDialog = globalActive && globalActive != active
            && !m_leafForWindow.contains(globalActive);
        if (!wouldBuryFocusedDialog) {
            workspace()->raiseWindow(active);
        }
    }
    qCDebug(KWIN_KI3) << "tab visibility:" << st.windows.size() << "tabs, active" << st.active;
}

void Ki3Tiler::refreshGroup(CustomTile *tile)
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

    // Prune dead windows, raise the active tab (shared T0 logic). May erase the
    // group if it emptied.
    updateTabVisibility(tile);
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end()) {
        destroyGroupHeader(tile);
        return;
    }
    TabState &st = it.value();

    const bool stacked = (st.mode == ContainerMode::Stacked);
    const qreal headerPx = Ki3Header::heightForTabs(st.windows.size(), stacked);

    // Reserve the header strip on the tile so its windows lay out below it.
    tile->setHeaderReserve(headerPx);

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
                &Ki3Tiler::onGroupGeometryChanged, Qt::UniqueConnection);
    }

    // Hide the header when the visible tab isn't actually on screen.
    Window *active = st.windows[st.active];
    if (!active || !active->isShown() || !active->isOnCurrentDesktop()) {
        st.header->hide();
        return;
    }

    // The header sits directly above the reserved window area, extended by
    // kIndicatorThickness on each side to line up with the tile border's
    // left/right strips (see outwardBorderStrips() in ki3tiler.cpp) instead of
    // falling short of them -- same fix as the floating title bar's
    // repositionFloatChrome() got earlier.
    const RectF content = tile->windowGeometry();
    const QRectF headerRect(content.left() - kIndicatorThickness, content.top() - headerPx,
                            content.width() + 2 * kIndicatorThickness, headerPx);

    QStringList titles;
    titles.reserve(st.windows.size());
    for (const QPointer<Window> &w : st.windows) {
        titles << (w ? w->caption() : QString());
    }
    const bool focused = (workspace()->activeWindow() == active);
    st.header->setGeometry(headerRect.toRect());
    st.header->setTabs(titles, st.active, stacked, focused);
    st.header->show();

    qCDebug(KWIN_KI3) << "group header:" << st.windows.size() << (stacked ? "stacked" : "tabbed")
                      << "active" << st.active << "reserve" << headerPx;
}

void Ki3Tiler::refreshAllGroups()
{
    const auto tiles = m_tabbed.keys(); // snapshot: refreshGroup may erase
    for (CustomTile *tile : tiles) {
        if (m_tabbed.contains(tile)) {
            refreshGroup(tile);
        }
    }
}

void Ki3Tiler::destroyGroupHeader(CustomTile *tile)
{
    if (!tile) {
        return;
    }
    disconnect(tile, &Tile::windowGeometryChanged, this, &Ki3Tiler::onGroupGeometryChanged);
    tile->setHeaderReserve(0.0);
    // The Ki3Header itself is owned by the TabState's shared_ptr and dies when
    // the caller erases the group from m_tabbed.
}

void Ki3Tiler::onGroupGeometryChanged()
{
    if (auto *tile = qobject_cast<CustomTile *>(sender()); tile && m_tabbed.contains(tile)) {
        refreshGroup(tile);
    }
}

void Ki3Tiler::onGroupTileDestroyed(QObject *tile)
{
    // The tile is mid-destruction; use it as a bare key only (no dereference).
    // qobject_cast would already return nullptr here, so cast statically.
    auto it = m_tabbed.find(static_cast<CustomTile *>(tile));
    if (it == m_tabbed.end()) {
        return;
    }
    // This slot runs reentrantly, synchronously nested inside the dying
    // tile's own QObject destructor -- see onTileBorderDestroyed()'s doc
    // comment (ki3tiler.cpp) for the full trace of why actually tearing
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

void Ki3Tiler::onManagedRootDestroyed(QObject *tile)
{
    // Bare key only (mid-destruction); never dereferenced. See the header doc.
    if (m_managedRoots.remove(static_cast<RootTile *>(tile)) > 0) {
        qCDebug(KWIN_KI3) << "managed root destroyed; dropped stale entry";
    }
}

void Ki3Tiler::activateTab(CustomTile *tile, int index)
{
    auto it = m_tabbed.find(tile);
    if (it == m_tabbed.end() || index < 0 || index >= it->windows.size()) {
        return;
    }
    it->active = index;
    Window *window = it->windows[index];
    qCDebug(KWIN_KI3) << "tab click -> active" << index;
    refreshGroup(tile);
    if (window) {
        workspace()->activateWindow(window);
    }
}

} // namespace KWin
