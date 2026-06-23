/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ki3rules.h"
#include "plugin.h"
#include "tiles/tile.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>

namespace KWin
{

class CustomTile;
class LogicalOutput;
class RootTile;
class Window;

class Ki3Tiler : public Plugin
{
    Q_OBJECT

public:
    explicit Ki3Tiler();
    ~Ki3Tiler() override;

private:
    void handleWindowAdded(Window *window);
    void handleWindowRemoved(Window *window);
    void handleWindowActivated(Window *window);

    /** Register global shortcuts. */
    void registerShortcuts();

    /** The leaf tile of the currently active or last focused window. */
    CustomTile *currentLeaf() const;

    /** Move keyboard focus to the neighbouring leaf in @p edge direction. */
    void moveFocus(Qt::Edge edge);

    /** Move focus from @p leaf to the adjacent output in @p edge direction. */
    void moveFocusAcrossOutput(CustomTile *leaf, Qt::Edge edge);

    /** Swap the active window with its neighbour in @p edge direction. */
    void moveWindow(Qt::Edge edge);

    /** Grow/shrink the active leaf along @p orientation by @p deltaPixels. */
    void resizeActive(Qt::Orientation orientation, qreal deltaPixels);

    /** Toggle the split direction applied to the next inserted window. */
    void toggleSplitDirection();

    /** Toggle the active window between tiled and floating. */
    void toggleFloating();

    /** Switch the focused output to workspace (virtual desktop) @p number (1-based). */
    void switchToWorkspace(int number);

    /** Move the active window to workspace @p number (1-based) on its output. */
    void moveActiveToWorkspace(int number);

    /** The output of the active window, else the globally active output. */
    LogicalOutput *focusedOutput() const;

    /** Launch a terminal emulator (i3-style Meta+Return). */
    void spawnTerminal();

    /** Whether ki3 should tile this window at all. */
    bool shouldManage(Window *window) const;

    /** Whether a non-tileable rule (built-in or user-configured) matches @p window. */
    bool isNonTileable(const Window *window) const;

    /** Insert a manageable window into its output/desktop tile tree. */
    void insertWindow(Window *window);

    /** Detach a window and collapse the tile it leaves behind. */
    void forgetWindow(Window *window);

    /** Root tile for the window's output and (current) desktop, or nullptr. */
    RootTile *rootForWindow(Window *window) const;

    /**
     * Clear KWin's default tile layout for @p root so ki3 owns the tree.
     * Done once per root (lazily) the first time we manage it.
     */
    void ensureManaged(RootTile *root);

    /** First leaf (childless) tile under @p root, or @p root if it has none. */
    static CustomTile *firstLeaf(RootTile *root);

    /** Lay out @p parent's direct children as equal slices along its direction. */
    static void redistributeEvenly(CustomTile *parent);

    /**
     * Re-point m_leafForWindow at the leaf each managed window actually sits in.
     * KWin's CustomTile::remove() can migrate a window into a promoted parent
     * tile (customtile.cpp:326-340), leaving our mapping pointing at a deleted
     * tile; this repairs it for the whole tree under @p root.
     */
    void resyncLeafMapping(RootTile *root);

    /**
     * Set @p tile's geometry to @p geom, bypassing CustomTile's neighbour-push
     * engine, and remap its subtree proportionally so nested layouts keep their
     * internal ratios. Used by redistributeEvenly to assign exact slices.
     */
    static void setGeometryRecursive(CustomTile *tile, const RectF &geom);

    // Default split direction for new windows (i3 default: side-by-side).
    Tile::LayoutDirection m_splitDirection = Tile::LayoutDirection::Horizontal;

    // Authoritative mapping of the windows we manage to their leaf tile.
    QHash<Window *, QPointer<CustomTile>> m_leafForWindow;

    // The leaf that should receive the next window (last focused tiled leaf).
    QPointer<CustomTile> m_lastFocusedLeaf;

    // Roots whose default layout we've already cleared and taken over.
    QSet<RootTile *> m_managedRoots;

    // Windows the user has detached from tiling (floating).
    QSet<Window *> m_floatingWindows;

    // Rules (built-in + user config) for windows ki3 must never tile.
    QList<WindowRule> m_nonTileableRules;
};

} // namespace KWin
