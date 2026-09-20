/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ki3header.h"
#include "ki3rules.h"
#include "tiles/tile.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <memory>

namespace KWin
{

class CustomTile;
class LogicalOutput;
class RootTile;
class Window;
enum class DecorationPolicy;

/**
 * Owns ki3's tile tree: the authoritative window-to-leaf mapping, tab/stack
 * group model, floating-window set, and the navigation (focus/move/resize)
 * that acts on them. Extracted from the original god-object `Ki3Tiler` as
 * the first (root) controller of the M1 refactor -- see
 * `~/.claude/plans/toasty-fluttering-kitten.md` and the matching ki3-PLAN.md
 * entry: every other future controller (decoration, workspace, shortcuts)
 * only ever *reads* from this one, never the reverse, so this class has zero
 * dependencies on any sibling controller or on `Ki3Tiler` itself.
 *
 * The one exception is presentational: creating a tab/stack group header
 * needs a colour palette, which is still computed by `Ki3Tiler` (pending a
 * future decoration-controller extraction). Rather than reaching back for
 * it, `Ki3Tiler` pushes a fresh `Ki3HeaderPalette` in via setHeaderPalette()
 * whenever it recomputes one -- a one-directional data flow that keeps this
 * class from needing to know `Ki3Tiler` exists at all.
 *
 * `Ki3Tiler` is still the one place border/split/resize-indicator *rendering*
 * happens (updateSplitIndicator() and friends), since that also needs float-
 * chrome and tile-border state this class doesn't own. Structural changes
 * here are reported via layoutChanged() instead of calling into that
 * rendering code directly, so this class never needs to know it exists.
 */
class TileTreeController : public QObject
{
    Q_OBJECT

public:
    explicit TileTreeController(QObject *parent = nullptr);

    /**
     * i3/sway container layouts beyond plain splits. A tab/stack "container"
     * is represented as a *single leaf tile owning several windows* (KWin
     * gives every window in a tile the same rect -- tile.cpp:141-143 -- i.e.
     * they overlap, which is exactly a tab/stack group); ki3 layers on top
     * which one is visible and draws the header.
     */
    enum class ContainerMode {
        Tabbed, // one header row of tabs; cycle with focus left/right
        Stacked, // stacked title rows; cycle with focus up/down
    };

    /** The leaf tile of the currently active or last focused window. */
    CustomTile *currentLeaf() const;

    /** Whether @p window currently has a leaf (tiled, not floating/unmanaged). */
    bool isManaged(Window *window) const;

    /** The leaf @p window is tiled in, or nullptr if it isn't managed. */
    CustomTile *leafFor(Window *window) const;

    /** Snapshot of every currently tiled window with a live leaf. */
    QList<Window *> managedWindows() const;

    /** Whether @p window is currently floating (never tiled, i3/sway style -- see wantsFloating()). */
    bool isFloating(Window *window) const;

    /** Every window currently floating, however it got there. */
    const QSet<Window *> &floatingWindows() const;

    /**
     * Add/remove @p window from the floating set (see Ki3Tiler::floatWindow()/
     * toggleFloating()). addFloating() also captures the window's pre-ki3
     * decoration/keep-above baseline (notePresentationBaseline(), a no-op if
     * already recorded), covering both "was already tiled, now floats" and
     * "never tiled, floats from the moment it opened" (a floating-rule or
     * sticky match).
     */
    void addFloating(Window *window);
    void removeFloating(Window *window);

    /** Whether @p tile is a tab/stack group (see ContainerMode). */
    bool isGroup(CustomTile *tile) const;

    /** Default split direction applied to the next inserted window. */
    Tile::LayoutDirection splitDirection() const
    {
        return m_splitDirection;
    }

    /** Whether ki3 should tile this window at all. */
    bool shouldManage(Window *window) const;

    /**
     * Whether a built-in ignore rule matches @p window -- ki3 must never
     * touch it at all: not tiled, not floating, no chrome, exactly as if ki3
     * didn't exist (e.g. xwaylandvideobridge's normally-invisible helper
     * window). Deliberately separate from wantsFloating()/matchesFloatingRule()
     * -- see builtinIgnoredRules()'s doc comment for why.
     */
    bool isIgnored(const Window *window) const;

    /** Whether a user-configured floating rule (ki3rc [General] FloatingClasses/FloatingTitles) matches @p window. */
    bool matchesFloatingRule(const Window *window) const;

    /**
     * Whether ki3 should make this window float automatically, i3/sway style:
     * it's a sticky/multi-desktop window, or it matches a floating rule.
     * False for anything isIgnored() or not a manageable kind of window at
     * all (special/internal/etc.) in the first place -- those must never be
     * touched, floating included. Called at window-added time and by
     * reconcileFloatingState() when rules are (re)loaded.
     */
    bool wantsFloating(const Window *window) const;

    /**
     * Record that the user explicitly toggled @p window's floating/tiled
     * state by hand (Ki3Tiler::toggleFloating()) -- from then on, for the
     * rest of the window's lifetime, it's immune to reloadConfig()'s
     * rule/sticky-driven reconciliation, exactly like manually-floated
     * windows always have been. dropManualOverride() clears it once the
     * window is gone.
     */
    void markManualOverride(Window *window);
    bool isManualOverride(Window *window) const;
    void dropManualOverride(Window *window);

    /**
     * True if some ordinary window stacked above @p window overlaps its
     * frame (e.g. a modal dialog raised over its tiled parent).
     */
    bool leafWindowOccluded(Window *window) const;

    /**
     * Root tile for the window's output and (current) desktop, or nullptr.
     * @p outputHint overrides window->output() when it is known to be stale
     * (see insertWindow()).
     */
    RootTile *rootForWindow(Window *window, LogicalOutput *outputHint = nullptr) const;

    /** Every currently visible tiled leaf (used to draw tile borders). */
    QSet<CustomTile *> liveLeaves() const;

    /**
     * Insert a manageable window into its output/desktop tile tree.
     *
     * @p outputHint, when non-null, overrides window->output() for choosing
     * the target tree -- required right after a cross-output move, see
     * `WorkspaceController::moveActiveToWorkspace()` and
     * `moveWindowAcrossOutput()`.
     */
    void insertWindow(Window *window, LogicalOutput *outputHint = nullptr);

    /** Detach a window and collapse the tile it leaves behind. */
    void forgetWindow(Window *window);

    /** Drop any window's decoration/keep-above baseline (it has left for good). */
    void dropPresentationBaseline(Window *window);

    /**
     * Restore @p window's keep-above state to the value captured before ki3
     * first touched it (false if none was ever recorded). Used when
     * un-floating and during plugin teardown.
     */
    void restoreKeepAbove(Window *window);

    /** Drop every managed root/leaf whose TileManager/RootTile no longer exists. */
    void purgeStaleRoots();

    /**
     * Track which leaf should receive the next window: if @p window is
     * currently tiled, remember its leaf as the last-focused one. Called
     * from `Ki3Tiler::handleWindowActivated()`.
     */
    void noteWindowActivated(Window *window);

    /** Push a freshly (re)computed header palette to every tab/stack header. */
    void setHeaderPalette(const Ki3HeaderPalette &palette);

    /**
     * Push a freshly (re)read indicator thickness (ki3rc [General]
     * BorderThickness, see DecorationController) to every tab/stack header, so
     * a tab/stack header stays flush with the tile border it lines up with
     * (see refreshGroup()) even after the setting changes live.
     */
    void setIndicatorThickness(qreal thickness);

    /**
     * Re-read `ki3rc`'s floating rules and tile/outer gaps (`[General]`
     * `FloatingClasses`/`FloatingTitles`/`Gap`/`OuterGap`) and apply the
     * gaps: swaps m_floatingRules, and re-applies both gaps to every
     * already-managed root's padding/outerPadding (Tile::setPadding()/
     * setOuterPadding() no-op when unchanged and already live-resize any
     * managed windows -- see tile.cpp). Does *not* itself re-check any
     * window against the freshly-reloaded rules -- that needs
     * DecorationController for floating chrome, which this class
     * deliberately has no dependency on; see
     * Ki3Tiler::reconcileFloatingState(), called right after this from
     * Ki3Tiler::reloadConfig().
     */
    void reloadConfig();

    /** Move keyboard focus to the neighbouring leaf in @p edge direction. */
    void moveFocus(Qt::Edge edge);

    /** Swap the active window with its neighbour in @p edge direction. */
    void moveWindow(Qt::Edge edge);

    /** Grow/shrink the active leaf along @p orientation by @p deltaPixels. */
    void resizeActive(Qt::Orientation orientation, qreal deltaPixels);

    /**
     * Toggle the *current container's* layout direction between horizontal
     * and vertical in place (i3/sway "layout toggle split").
     */
    void toggleContainerLayout();

    /**
     * Set the split direction applied to the next inserted window (i3/sway
     * "split h"/"split v").
     */
    void setSplitDirection(Tile::LayoutDirection direction);

    /**
     * Collapse the focused container into a single tab/stack group leaf
     * (i3/sway "layout tabbed"/"layout stacked"). Re-invoking with the same
     * mode splits it back out (untabContainer); invoking the other mode
     * flips it in place.
     */
    void setContainerMode(ContainerMode mode);

    /** Fan a tab/stack group leaf back out into an even split of its windows. */
    void untabContainer(CustomTile *tile);

    /** Advance the visible tab of @p tile by @p delta (wraps), and focus it. */
    void cycleTab(CustomTile *tile, int delta);

    /** Activate the tab at @p index within @p tile (from a header click). */
    void activateTab(CustomTile *tile, int index);

    /** Refresh every tab/stack group (e.g. after a desktop switch). */
    void refreshAllGroups();

    /**
     * Synchronously tear down @p tile's tab/stack group (header + model
     * entry), e.g. right as its output is being removed -- before KWin
     * destroys the tile itself. See `Ki3Tiler::teardownGroupsOnOutput()`.
     */
    void dropGroup(CustomTile *tile);

    /**
     * Part 1 of plugin teardown (see `Ki3Tiler::teardownManagedState()`):
     * restore every floating/tiled window's pre-ki3 decoration/keep-above
     * state, detach every tiled window from its tile, and drop every
     * tab/stack group header. Does *not* touch floating chrome (still
     * `Ki3Tiler`-owned pending the decoration-controller extraction) --
     * callers must destroy that first.
     */
    void detachAllManagedWindows();

    /**
     * Part 2 of plugin teardown: drop ki3's own split structure from every
     * surviving managed root so a fresh load (or KWin's default layout)
     * starts clean. Call after detachAllManagedWindows() so every leaf's
     * windows are already forgotten (remove() then has nothing to migrate).
     */
    void dropManagedRoots();

Q_SIGNALS:
    /**
     * Emitted whenever the tile tree's structure or the current leaf
     * changes in a way that could affect the split/resize/tile-border
     * overlays. `Ki3Tiler` connects this to updateSplitIndicator() (which
     * also refreshes tile borders) -- replaces what used to be a direct
     * call from inside every tree-mutating method.
     */
    void layoutChanged();

private:
    struct TabState
    {
        // Tab order. Each item is a direct (Floating-direction, fully
        // overlapping) child of the group's container tile, and may itself
        // be a whole nested split subtree with several windows -- not just
        // one window -- so tabbing a container that already has a nested
        // split inside it preserves that structure instead of flattening
        // every descendant window into one flat tab list. A plain "tab one
        // window" item is the same shape, just a subtree of depth zero (a
        // leaf). See wrapLeafInPlace(), groupContainerFor().
        //
        // Consequence: the group's *container* tile itself now always has
        // children (its items), unlike the old flat design where it was a
        // plain leaf directly owning every tab's window. currentLeaf() (and
        // m_leafForWindow generally) resolves to a window's own actual leaf,
        // which for a tab is always some *item* (or something inside one),
        // never the container -- so any check for "is the current focus a
        // tab/stack group" must walk up via groupContainerFor() and compare
        // against that container's active item, never a plain
        // `m_tabbed.contains(leaf)` on whatever currentLeaf() returned.
        QList<QPointer<CustomTile>> items;
        int active = 0; // index into items of the visible one
        ContainerMode mode = ContainerMode::Tabbed;
        // The container's split direction before it was collapsed, restored
        // on untab (i3/sway prev_split_layout).
        Tile::LayoutDirection prevSplit = Tile::LayoutDirection::Horizontal;
        std::shared_ptr<Ki3Header> header; // the title bar (created lazily)
    };

    /**
     * A window's decoration policy and keep-above state as they were before
     * ki3 first touched them. Captured once per window
     * (notePresentationBaseline()) so tiling/floating/untiling and eventual
     * plugin teardown can restore exactly this instead of clobbering it to a
     * hardcoded default.
     */
    struct PresentationState
    {
        DecorationPolicy decorationPolicy;
        bool keepAbove = false;
    };

    /**
     * Place @p window at @p target, following i3 join rules: joins a
     * tab/stack group as a new tab, becomes an evenly-redistributed sibling
     * if @p target's parent already runs the current split direction, or
     * otherwise splits @p target in the current split direction.
     */
    void placeWindowAt(Window *window, CustomTile *target, bool insertBefore = false);

    /**
     * Add @p window to @p leaf and make the tile association stick even
     * when @p leaf's desktop is not the one currently shown on its output.
     */
    void attachWindow(Window *window, CustomTile *leaf);

    /**
     * Snapshot @p window's current decoration policy and keep-above state
     * into m_originalPresentation, if not already recorded. Call this at
     * the single point where a previously-untracked window first becomes
     * ki3-owned (attachWindow()).
     */
    void notePresentationBaseline(Window *window);

    /**
     * Restore @p window's decoration policy to the value
     * notePresentationBaseline() recorded (DecorationPolicy::PreferredByClient
     * if none was ever recorded).
     */
    void restoreDecorationPolicy(Window *window);

    /**
     * Clear KWin's default tile layout for @p root so ki3 owns the tree.
     * Done once per root (lazily) the first time we manage it.
     */
    void ensureManaged(RootTile *root);

    /**
     * The actual value passed to Tile::setPadding() (the gap *between*
     * tiles): the configured gap (`ki3rc [General] Gap`, m_gap) *plus* two
     * border thicknesses. The border is drawn outward from a tile's
     * windowGeometry() into its own padding/gap allowance (see
     * outwardBorderStrips() in ki3decorationcontroller.cpp), and on a shared
     * edge between two tiles each side only gets half the padding -- so
     * without this, a configured gap smaller than 2x the border thickness
     * would let the two tiles' borders overlap or clip (one border from
     * each side of the shared edge). m_gap (the KCM's "Gap" setting) is
     * therefore the *extra* breathing room beyond the minimum the borders
     * themselves need, not the total.
     */
    qreal effectivePadding() const
    {
        return m_gap + 2 * m_indicatorThickness;
    }

    /**
     * The actual value passed to Tile::setOuterPadding() (the gap *around
     * the screen edge*): the configured outer gap (`ki3rc [General]
     * OuterGap`, m_outerGap) plus a *single* border thickness -- unlike
     * effectivePadding(), only one border ever faces the screen edge (the
     * other "side" is the physical screen boundary, which draws nothing),
     * so it only needs to fit past that one. Kept as a genuinely separate
     * Tile::outerPadding() (a core patch -- see tile.h/.cpp) rather than
     * reusing padding()'s halving trick, since half of a *combined*
     * inner+outer value can't represent two independently configurable gaps
     * at once; see the ki3-PLAN.md entry for the full derivation of why the
     * single shared `padding()` value made the rendered gap inconsistent
     * between a single window, a tiled split, and a tabbed group before
     * this was split out.
     */
    qreal effectiveOuterPadding() const
    {
        return m_outerGap + m_indicatorThickness;
    }

    /** Re-apply effectivePadding()/effectiveOuterPadding() to every currently-managed root. */
    void applyPaddingToManagedRoots();

    /** First leaf (childless) tile under @p root, or @p root if it has none. */
    static CustomTile *firstLeaf(RootTile *root);

    /** Lay out @p parent's direct children as equal slices along its direction. */
    static void redistributeEvenly(CustomTile *parent);

    /**
     * Re-point m_leafForWindow at the leaf each managed window actually
     * sits in. KWin's CustomTile::remove() can migrate a window into a
     * promoted parent tile, leaving our mapping pointing at a deleted tile;
     * this repairs it for the whole tree under @p root.
     */
    void resyncLeafMapping(RootTile *root);

    /**
     * Set @p tile's geometry to @p geom, bypassing CustomTile's
     * neighbour-push engine, and remap its subtree proportionally so nested
     * layouts keep their internal ratios.
     */
    static void setGeometryRecursive(CustomTile *tile, const RectF &geom);

    /**
     * Move focus from @p leaf to a neighbouring tile (same output, then
     * adjacent output) in @p edge direction. Returns whether a target was
     * found and focused.
     */
    bool leaveLeaf(CustomTile *leaf, Qt::Edge edge);

    /** Move focus from @p leaf to the adjacent output in @p edge direction. */
    bool moveFocusAcrossOutput(CustomTile *leaf, Qt::Edge edge);

    /**
     * Move @p self (currently at @p leaf) to the adjacent output in @p edge
     * direction when moveWindow() found no neighbour tile within the current
     * output's tree -- the moveWindow() counterpart to moveFocusAcrossOutput().
     * No-op if there is no output in that direction.
     */
    void moveWindowAcrossOutput(CustomTile *leaf, Window *self, Qt::Edge edge);

    /**
     * Eject @p self (a member of the group at @p leaf) toward @p edge when
     * moveWindow() found no existing neighbour tile to pop it into.
     */
    void ejectGroupMemberViaSplit(CustomTile *leaf, Window *self, Qt::Edge edge);

    /** Raise the active tab of @p tile above the others; prune dead windows. */
    void updateTabVisibility(CustomTile *tile);

    /**
     * Turn @p leaf (a plain leaf tile, no children) into a single-child
     * layout of @p containerDirection, moving its windows onto a freshly
     * created sole child (given @p childDirection as its own initial
     * direction) that takes over @p leaf's old geometry unchanged -- so
     * nothing visibly changes until a second item/window is later added
     * alongside it. Updates m_leafForWindow/m_lastFocusedLeaf for the moved
     * windows. Returns the new child.
     *
     * Shared by setSplitDirection() (i3 "split h"/"split v": immediately
     * prepare the focused leaf for a same-direction sibling on the next
     * insert, instead of the old global "remember direction, apply
     * whenever" flag that could apply to the wrong container if focus moved
     * on in between) and setContainerMode() (wrap a single window as the
     * sole tab item before any sibling exists to tab it with).
     */
    CustomTile *wrapLeafInPlace(CustomTile *leaf, Tile::LayoutDirection containerDirection,
                                Tile::LayoutDirection childDirection);

    /**
     * Walks up from @p tile to find an enclosing tab/stack group container,
     * if any -- @p tile itself, or any ancestor up to (but not including)
     * the group container, may be several levels deep inside a nested tab
     * item's own subtree. Returns nullptr if @p tile isn't inside a group.
     */
    CustomTile *groupContainerFor(CustomTile *tile) const;

    /**
     * Like CustomTile::nextNonLayoutTileAt(), but a tab/stack group
     * container counts as a leaf too (even though it has children -- see
     * TabState::items) instead of being drilled into: a group must be
     * targeted as one atomic unit by focus/move navigation, exactly like an
     * ordinary window leaf, never as "whichever tab happens to be first".
     */
    CustomTile *nextGroupAwareTileAt(CustomTile *leaf, Qt::Edge edge) const;

    /**
     * The window a group's header/activation logic should treat as
     * representing @p item: the one currently holding keyboard focus if
     * it's inside @p item's subtree, else the first window found there
     * (deterministic default for an inactive/never-focused nested item).
     */
    Window *representativeWindow(CustomTile *item) const;

    /** Every window in @p item's subtree (itself included), in tree order. */
    static QList<Window *> subtreeWindows(CustomTile *item);

    /**
     * The window that activating/focusing @p tile should actually raise:
     * @p tile's own first window for a plain leaf, or the active tab's
     * representativeWindow() if @p tile is a tab/stack group container.
     */
    Window *activationTargetFor(CustomTile *tile) const;

    /**
     * Drop @p item (one of @p container's TabState::items) from the group
     * and physically remove its now-empty tile. If it was the last item,
     * the group itself is dropped and @p container falls through to the
     * ordinary now-genuinely-empty-leaf collapse (mirrors forgetWindow()'s
     * plain-leaf path, since a Floating group container never triggers
     * KWin's own single-child promotion -- see the doc comment on
     * forgetWindow()'s group branch).
     */
    void removeGroupItem(CustomTile *container, CustomTile *item);

    /**
     * Recompute a tab/stack group's header: reserve the right header height
     * on its tile, reposition + repaint the header window over the reserved
     * strip, and hide it when the group is off the current desktop.
     */
    void refreshGroup(CustomTile *tile);

    /** Tear down a group's header and clear its tile's header reserve. */
    void destroyGroupHeader(CustomTile *tile);

    /** Slot: a group tile's geometry changed -- reposition its header. */
    void onGroupGeometryChanged();

    /**
     * Slot: a group's container tile was destroyed out from under us (e.g.
     * an output was unplugged). @p tile is only used as a hash key here
     * (never dereferenced), so it is safe mid-destruction.
     */
    void onGroupTileDestroyed(QObject *tile);

    /**
     * Slot: a root tile ki3 had taken over was destroyed -- not only on
     * output unplug but on every desktop prune. @p tile is a bare key here
     * (never dereferenced).
     */
    void onManagedRootDestroyed(QObject *tile);

    // Default split direction for new windows (i3 default: side-by-side).
    Tile::LayoutDirection m_splitDirection = Tile::LayoutDirection::Horizontal;

    // Authoritative mapping of the windows we manage to their leaf tile.
    QHash<Window *, QPointer<CustomTile>> m_leafForWindow;

    // Pre-ki3 decoration/keep-above state, one entry per window ki3 has ever
    // tiled or floated; see PresentationState and notePresentationBaseline().
    QHash<Window *, PresentationState> m_originalPresentation;

    // The leaf that should receive the next window (last focused tiled leaf).
    QPointer<CustomTile> m_lastFocusedLeaf;

    // Roots whose default layout we've already cleared and taken over.
    QSet<RootTile *> m_managedRoots;

    // Every currently floating window, however it got there (manual toggle,
    // a floating rule, or sticky/multi-desktop policy).
    QSet<Window *> m_floatingWindows;

    // Windows whose current floating/tiled state was an explicit user choice
    // (Ki3Tiler::toggleFloating()) -- see markManualOverride()'s doc comment.
    QSet<Window *> m_manualOverrides;

    // Leaf tiles that are tab/stack groups (own several windows, one visible).
    // Keyed by the group leaf; entries are dropped when the group empties.
    QHash<CustomTile *, TabState> m_tabbed;

    // Guards refreshGroup() against the re-entry triggered when
    // setHeaderReserve emits windowGeometryChanged (which we listen to).
    QSet<CustomTile *> m_refreshingGroups;

    // User-configured floating rules (ki3rc [General] FloatingClasses/
    // FloatingTitles) -- reloaded live, see reloadConfig().
    QList<WindowRule> m_floatingRules;

    // Built-in ignore rules (see builtinIgnoredRules()) -- fixed at
    // construction, never reloaded (no user config feeds these).
    QList<WindowRule> m_ignoredRules;

    // Tile gap in logical px, from ki3rc [General] Gap (default: Tile's own
    // hardcoded default -- see tile.h). Applied to every root the moment ki3
    // takes it over (ensureManaged()) and re-applied to every already-managed
    // root on reloadConfig().
    qreal m_gap = 4.0;

    // Gap around the screen edge, from ki3rc [General] OuterGap. Separate
    // from m_gap -- see effectiveOuterPadding()'s doc comment -- but read/
    // applied at the same points in the lifecycle.
    qreal m_outerGap = 4.0;

    // Pushed in by Ki3Tiler::applyIndicatorColors() whenever it recomputes
    // the colour scheme -- see the class doc comment above. Used when
    // creating/repainting a tab/stack group header.
    Ki3HeaderPalette m_headerPalette;

    // Pushed in by DecorationController::applyIndicatorColors()/reloadConfig()
    // (ki3rc [General] BorderThickness) -- see setIndicatorThickness(). Used
    // by refreshGroup() to line a tab/stack header up with the tile border.
    qreal m_indicatorThickness = kIndicatorThickness;
};

} // namespace KWin
