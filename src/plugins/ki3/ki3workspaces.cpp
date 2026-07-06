/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3_logging.h"
#include "ki3tiler.h"

#include "core/output.h"
#include "core/outputbackend.h"
#include "main.h"
#include "tiles/customtile.h"
#include "tiles/tilemanager.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <QSize>
#include <QStringList>

namespace KWin
{

QStringList Ki3Tiler::outputNames() const
{
    QStringList names;
    const auto outputs = workspace()->outputs();
    names.reserve(outputs.size());
    for (LogicalOutput *output : outputs) {
        names << output->name();
    }
    return names;
}

int Ki3Tiler::currentDesktopNumber(const QString &outputName) const
{
    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        if (output->name() == outputName) {
            VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(output);
            if (!desktop) {
                return 0;
            }
            bool ok;
            const int n = desktop->name().toInt(&ok);
            return ok ? n : 0;
        }
    }
    return 0;
}

int Ki3Tiler::desktopCount() const
{
    return VirtualDesktopManager::self()->count();
}

QList<int> Ki3Tiler::liveDesktopNumbers() const
{
    QList<int> numbers;
    for (VirtualDesktop *d : VirtualDesktopManager::self()->desktops()) {
        bool ok;
        const int n = d->name().toInt(&ok);
        if (ok) {
            numbers.append(n);
        }
    }
    return numbers; // already in insertion order, which we keep sorted
}

QList<int> Ki3Tiler::desktopNumbersForOutput(const QString &outputName) const
{
    LogicalOutput *target = nullptr;
    for (LogicalOutput *output : workspace()->outputs()) {
        if (output->name() == outputName) {
            target = output;
            break;
        }
    }
    if (!target) {
        return {};
    }

    VirtualDesktop *current = VirtualDesktopManager::self()->currentDesktop(target);
    QList<int> numbers;
    for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
        bool ok;
        const int n = desktop->name().toInt(&ok);
        if (!ok) {
            continue;
        }
        // Per-output workspace set: the desktop shown here, plus any desktop whose
        // windows physically live on this output (its home). By ki3's one-desktop-
        // per-screen invariant these never collide with another output's set.
        if (desktop == current || outputForDesktop(desktop) == target) {
            numbers.append(n);
        }
    }
    return numbers;
}

QString Ki3Tiler::dbusAddTestOutput()
{
    if (qEnvironmentVariableIsEmpty("KI3_TEST_HOOKS")) {
        return QString();
    }
    const QString name = QStringLiteral("KI3-VIRT-%1").arg(m_testOutputSeq++);
    BackendOutput *out = kwinApp()->outputBackend()->createVirtualOutput(
        name, name, QSize(1280, 800), 1.0);
    if (!out) {
        qCWarning(KWIN_KI3) << "dbusAddTestOutput: backend refused" << name;
        return QString();
    }
    m_testOutputs.append(out);
    qCDebug(KWIN_KI3) << "test hotplug: added output" << name;
    return name;
}

QString Ki3Tiler::dbusRemoveTestOutput()
{
    if (qEnvironmentVariableIsEmpty("KI3_TEST_HOOKS")) {
        return QString();
    }
    // Drop any that vanished by another route, then unplug the newest survivor.
    while (!m_testOutputs.isEmpty() && !m_testOutputs.last()) {
        m_testOutputs.removeLast();
    }
    if (m_testOutputs.isEmpty()) {
        return QString();
    }
    BackendOutput *out = m_testOutputs.takeLast();
    kwinApp()->outputBackend()->removeVirtualOutput(out);
    qCDebug(KWIN_KI3) << "test hotplug: removed an output";
    return QStringLiteral("removed");
}

void Ki3Tiler::dbusSwitchToWorkspace(int number)
{
    switchToWorkspace(number);
}

void Ki3Tiler::dbusMoveActiveToWorkspace(int number)
{
    moveActiveToWorkspace(number);
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

VirtualDesktop *Ki3Tiler::desktopByNumber(int n) const
{
    if (n < 1) {
        return nullptr;
    }
    const QString name = QString::number(n);
    for (VirtualDesktop *d : VirtualDesktopManager::self()->desktops()) {
        if (d->name() == name) {
            return d;
        }
    }
    return nullptr;
}

VirtualDesktop *Ki3Tiler::getOrCreateDesktop(int n)
{
    if (n < 1) {
        return nullptr;
    }
    if (VirtualDesktop *existing = desktopByNumber(n)) {
        return existing;
    }
    // Insert in name-sorted order so the pager displays them in numeric order.
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    const auto desktops = vdm->desktops();
    uint pos = uint(desktops.size());
    for (uint i = 0; i < uint(desktops.size()); ++i) {
        bool ok;
        const int existing_n = desktops[int(i)]->name().toInt(&ok);
        if (ok && existing_n > n) {
            pos = i;
            break;
        }
    }
    qCDebug(KWIN_KI3) << "creating desktop" << n << "at position" << pos;
    return vdm->createVirtualDesktop(pos, QString::number(n));
}

VirtualDesktop *Ki3Tiler::createFreshDesktop()
{
    const auto desktops = VirtualDesktopManager::self()->desktops();
    QSet<int> used;
    for (VirtualDesktop *d : desktops) {
        bool ok;
        const int n = d->name().toInt(&ok);
        if (ok) {
            used.insert(n);
        }
    }
    int number = 1;
    while (used.contains(number)) {
        ++number;
    }
    return getOrCreateDesktop(number);
}

void Ki3Tiler::schedulePrune()
{
    if (m_prunePending) {
        return;
    }
    m_prunePending = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_prunePending = false;
        pruneEmptyDesktops();
    }, Qt::QueuedConnection);
}

void Ki3Tiler::pruneEmptyDesktops()
{
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    // Iterate a copy: removeVirtualDesktop modifies the live list.
    const QList<VirtualDesktop *> desktops = vdm->desktops();
    for (VirtualDesktop *desktop : desktops) {
        if (outputShowingDesktop(desktop)) {
            continue; // currently visible on some screen
        }
        if (outputForDesktop(desktop)) {
            continue; // has at least one window (even if hidden)
        }
        qCDebug(KWIN_KI3) << "pruning empty desktop" << desktop->name();
        vdm->removeVirtualDesktop(desktop);
    }
}

void Ki3Tiler::assignInitialDesktops()
{
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    const auto outputs = workspace()->outputs();
    const int nOutputs = std::max(1, int(outputs.size()));

    // Target one numbered desktop per output. setCount() removes desktops from
    // the end, so shrinking straight to nOutputs would destroy higher-numbered
    // desktops and re-home their windows — harmless on a fresh boot (a single
    // default desktop) but destructive on a plugin reload into a populated
    // session. Never shrink past an occupied desktop: keep at least up to the
    // highest one currently holding a window (pruneEmptyDesktops() clears any
    // leftover empties afterwards anyway).
    const auto existing = vdm->desktops();
    int required = nOutputs;
    for (int i = 0; i < existing.size(); ++i) {
        if (outputForDesktop(existing[i])) {
            required = std::max(required, i + 1);
        }
    }
    vdm->setCount(uint(required));
    const auto desktops = vdm->desktops();
    for (int i = 0; i < nOutputs; ++i) {
        desktops[i]->setName(QString::number(i + 1));
        if (i < int(outputs.size())) {
            vdm->setCurrent(desktops[i], outputs[i]);
            qCDebug(KWIN_KI3) << "initial desktop" << (i + 1) << "-> output" << (void *)outputs[i];
        }
    }
}

LogicalOutput *Ki3Tiler::outputShowingDesktop(VirtualDesktop *desktop) const
{
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        if (vdm->currentDesktop(output) == desktop) {
            return output;
        }
    }
    return nullptr;
}

LogicalOutput *Ki3Tiler::outputForDesktop(VirtualDesktop *desktop, Window *exclude) const
{
    const auto windows = workspace()->windows();
    for (Window *w : windows) {
        // KWin's InternalWindow (ki3's own split-indicator overlay, tab/stack
        // headers) unconditionally reports windowType() == Normal and is always
        // on all desktops (see InternalWindow::windowType()/setOnAllDesktops in
        // internalwindow.cpp) -- without this check, whenever our own overlay
        // happens to be visible it looks like "a real window on every desktop"
        // and permanently blocks that desktop from ever being pruned.
        if (w != exclude && !w->isInternal() && w->isClient() && w->isNormalWindow()
            && w->output() && w->isOnDesktop(desktop)) {
            return w->output();
        }
    }
    return nullptr;
}

void Ki3Tiler::focusOutput(LogicalOutput *output)
{
    if (!output) {
        return;
    }
    workspace()->setActiveOutput(output);
    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(output);
    // Activate the topmost client on this output+desktop so keyboard focus
    // actually follows; if it is empty, the active-output change is enough.
    const auto &stacking = workspace()->stackingOrder();
    for (auto it = stacking.crbegin(); it != stacking.crend(); ++it) {
        Window *w = *it;
        // Exclude our own overlays (see outputForDesktop()): the split
        // indicator is "shown" and "on every desktop" whenever it's visible,
        // and being topmost it would otherwise steal focus from a real window.
        if (w && !w->isInternal() && w->isClient() && w->isNormalWindow() && w->isShown()
            && w->isOnOutput(output) && w->isOnDesktop(desktop)) {
            workspace()->activateWindow(w);
            return;
        }
    }
    qCDebug(KWIN_KI3) << "focusOutput: no window to activate on" << (void *)output;
    // No window here to trigger handleWindowActivated -> refresh the
    // indicator ourselves so it doesn't linger on the previous output.
    updateSplitIndicator();
}

VirtualDesktop *Ki3Tiler::firstFreeDesktop() const
{
    const auto desktops = VirtualDesktopManager::self()->desktops();
    // Prefer a desktop that is both free (shown on no output) AND truly empty (no
    // windows anywhere). "Free" alone isn't enough: a desktop not currently shown
    // can still hold windows hidden on another output, and assigning it to a fresh
    // screen would show an empty tree while those windows stay stranded elsewhere
    // (split-brain). Fall back to any free desktop only if every empty one is shown.
    VirtualDesktop *freeFallback = nullptr;
    for (VirtualDesktop *desktop : desktops) {
        if (outputShowingDesktop(desktop)) {
            continue; // shown somewhere -> not free
        }
        if (!outputForDesktop(desktop)) {
            return desktop; // free and empty: ideal
        }
        if (!freeFallback) {
            freeFallback = desktop; // free but has hidden windows: last resort
        }
    }
    return freeFallback; // nullptr only if every desktop is shown (won't happen with 10)
}

void Ki3Tiler::scheduleReconcile()
{
    if (m_reconcilePending) {
        return;
    }
    m_reconcilePending = true;
    // Run after KWin finishes the output reconfiguration (re-homing windows,
    // destroying defunct TileManagers) so we reconcile against settled state.
    QMetaObject::invokeMethod(this, &Ki3Tiler::reconcileOutputs, Qt::QueuedConnection);
}

void Ki3Tiler::reconcileOutputs()
{
    m_reconcilePending = false;
    qCDebug(KWIN_KI3) << "reconcile outputs:" << workspace()->outputs().size() << "output(s)";
    purgeStaleRoots();
    enforceUniqueDesktops();
    retileHomelessWindows();
    ensureSaneFocus();
    refreshAllGroups();
}

void Ki3Tiler::purgeStaleRoots()
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

void Ki3Tiler::enforceUniqueDesktops()
{
    // KWin gives a freshly plugged output desktop 1 by default
    // (initialDesktopForNewOutput), duplicating whatever another screen shows.
    // For each output whose desktop is already shown on an earlier output, move
    // it to a free desktop so each number lives on exactly one screen.
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    const auto outputs = workspace()->outputs();
    for (int i = 0; i < outputs.size(); ++i) {
        VirtualDesktop *desktop = vdm->currentDesktop(outputs[i]);
        bool reassign = (desktop == nullptr);
        // (a) The same desktop is already current on an earlier output -> KWin
        // duplicated it (initialDesktopForNewOutput defaults a new screen to 1).
        for (int j = 0; j < i && !reassign; ++j) {
            if (vdm->currentDesktop(outputs[j]) == desktop) {
                reassign = true;
            }
        }
        // (b) This desktop's windows physically live on a *different* live output
        // (their home). Showing it here would be split-brain: an empty view on
        // this screen while those windows stay hidden on the other. A freshly
        // plugged output landing on a hidden-but-occupied desktop hits this even
        // though it isn't a literal duplicate of any current desktop.
        if (!reassign && desktop) {
            LogicalOutput *home = outputForDesktop(desktop);
            if (home && home != outputs[i]) {
                reassign = true;
            }
        }
        if (!reassign) {
            continue;
        }
        VirtualDesktop *free = firstFreeDesktop();
        if (!free) {
            // All existing desktops are shown; create a new numbered one.
            free = createFreshDesktop();
        }
        if (free) {
            vdm->setCurrent(free, outputs[i]);
            qCDebug(KWIN_KI3) << "reconcile: output" << (void *)outputs[i]
                              << "-> desktop" << free->name();
        }
    }
}

void Ki3Tiler::retileHomelessWindows()
{
    const auto windows = workspace()->windows();
    for (Window *window : windows) {
        if (!shouldManage(window)) {
            // No longer manageable (e.g. its output vanished and rules changed);
            // drop any stale bookkeeping.
            if (m_leafForWindow.contains(window)) {
                forgetWindow(window);
            }
            continue;
        }
        auto it = m_leafForWindow.find(window);
        if (it == m_leafForWindow.end()) {
            // Relocated onto an output we weren't tracking it on -> tile it.
            insertWindow(window);
            continue;
        }
        CustomTile *leaf = it.value();
        RootTile *want = rootForWindow(window);
        if (!leaf || (want && leaf->rootTile() != want)) {
            // Tile destroyed by an unplug, or window moved to another tree.
            forgetWindow(window);
            insertWindow(window);
        }
    }
}

void Ki3Tiler::ensureSaneFocus()
{
    Window *active = workspace()->activeWindow();
    // isShown() ignores virtual-desktop visibility, so also require the window to
    // be on its output's current desktop; otherwise focus is on a hidden window.
    if (active && active->isShown() && active->isOnCurrentDesktop()) {
        return;
    }
    qCDebug(KWIN_KI3) << "reconcile: focus was hidden/gone, refocusing active output";
    focusOutput(workspace()->activeOutput());
}

void Ki3Tiler::switchToWorkspace(int number)
{
    VirtualDesktop *desktop = getOrCreateDesktop(number);
    if (!desktop) {
        return;
    }

    // i3/sway: a desktop lives on exactly one screen. If it is already shown
    // somewhere, just move focus to that output rather than duplicating it.
    if (LogicalOutput *shown = outputShowingDesktop(desktop)) {
        qCDebug(KWIN_KI3) << "switch to workspace" << number
                          << "- already shown, focusing output" << (void *)shown;
        focusOutput(shown);
        schedulePrune(); // the previous desktop we drifted from may now be empty
        return;
    }

    // Hidden (or freshly created): raise it on its home output (where its windows
    // already live), or on the focused output if it has none yet.
    LogicalOutput *home = outputForDesktop(desktop);
    if (!home) {
        home = focusedOutput();
    }
    qCDebug(KWIN_KI3) << "switch to workspace" << number << "on output" << (void *)home;
    VirtualDesktopManager::self()->setCurrent(desktop, home);
    focusOutput(home);
    schedulePrune(); // the desktop we just left may now be empty
}

void Ki3Tiler::moveActiveToWorkspace(int number)
{
    Window *window = workspace()->activeWindow();
    if (!window) {
        return;
    }
    // A floating window isn't in the tile tree, but i3/sway still let it follow
    // to the target workspace — it just stays floating there instead of being
    // re-tiled. Everything else must be a window ki3 actually tiles.
    const bool floating = m_floatingWindows.contains(window);
    if (!floating && !shouldManage(window)) {
        return;
    }
    VirtualDesktop *desktop = getOrCreateDesktop(number);
    if (!desktop || window->isOnDesktop(desktop)) {
        return;
    }

    // i3/sway: the target desktop lives on one screen, so the window must follow
    // it there. If the desktop has no home yet, the window's current output
    // becomes that home. Detach from the old tree, move, then tile into the
    // target (output, desktop) tree — geometry applies when that desktop shows.
    LogicalOutput *home = outputForDesktop(desktop, window);
    if (!home) {
        home = outputShowingDesktop(desktop);
    }
    if (!floating) {
        forgetWindow(window);
    }
    window->setDesktops({desktop});
    if (home && home != window->output()) {
        window->sendToOutput(home);
    }
    qCDebug(KWIN_KI3) << "move" << (floating ? "floating " : "") << "window to workspace" << number
                      << "on output" << (void *)window->output();
    if (floating) {
        repositionFloatChrome(window); // hide/reposition chrome for its new desktop
    } else {
        insertWindow(window);
    }
    schedulePrune(); // the source desktop may now be empty
    // The target desktop just gained a home output (or the source lost one); the
    // pager's per-output list keys off that, but this changed neither the current
    // desktop nor the desktop count, so nothing else emits desktopsChanged().
    Q_EMIT desktopsChanged();
}

} // namespace KWin
