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

#include <KConfigGroup>
#include <KSharedConfig>

#include <QMap>
#include <QRectF>
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

QStringList Ki3Tiler::tiledWindowGeometries() const
{
    QStringList out;
    for (Window *window : m_tileTree->managedWindows()) {
        if (!window || window->isDeleted()) {
            continue;
        }
        const QString outputName = window->output() ? window->output()->name() : QStringLiteral("?");
        int desktop = 0;
        if (!window->desktops().isEmpty()) {
            bool ok;
            const int n = window->desktops().constFirst()->name().toInt(&ok);
            desktop = ok ? n : 0;
        }
        const QRectF g = window->frameGeometry();
        out << QStringLiteral("%1|%2|%3,%4,%5,%6")
                   .arg(outputName)
                   .arg(desktop)
                   .arg(int(g.x()))
                   .arg(int(g.y()))
                   .arg(int(g.width()))
                   .arg(int(g.height()));
    }
    return out;
}

QStringList Ki3Tiler::floatingWindowGeometries() const
{
    QStringList out;
    for (Window *window : m_tileTree->floatingWindows()) {
        if (!window || window->isDeleted()) {
            continue;
        }
        const QString outputName = window->output() ? window->output()->name() : QStringLiteral("?");
        const QRectF g = window->frameGeometry();
        out << QStringLiteral("%1|%2,%3,%4,%5")
                   .arg(outputName)
                   .arg(int(g.x()))
                   .arg(int(g.y()))
                   .arg(int(g.width()))
                   .arg(int(g.height()));
    }
    return out;
}

bool Ki3Tiler::activeWindowNoBorder() const
{
    Window *window = workspace()->activeWindow();
    return window && window->noBorder();
}

bool Ki3Tiler::activeWindowKeepAbove() const
{
    Window *window = workspace()->activeWindow();
    return window && window->keepAbove();
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

bool Ki3Tiler::dbusSetActiveWindowNoBorder(bool noBorder)
{
    if (qEnvironmentVariableIsEmpty("KI3_TEST_HOOKS")) {
        return false;
    }
    Window *window = workspace()->activeWindow();
    if (!window) {
        return false;
    }
    window->setNoBorder(noBorder);
    return true;
}

bool Ki3Tiler::dbusSetActiveWindowKeepAbove(bool keepAbove)
{
    if (qEnvironmentVariableIsEmpty("KI3_TEST_HOOKS")) {
        return false;
    }
    Window *window = workspace()->activeWindow();
    if (!window) {
        return false;
    }
    window->setKeepAbove(keepAbove);
    return true;
}

bool Ki3Tiler::dbusSetActiveWindowOnAllDesktops(bool onAllDesktops)
{
    if (qEnvironmentVariableIsEmpty("KI3_TEST_HOOKS")) {
        return false;
    }
    Window *window = workspace()->activeWindow();
    if (!window) {
        return false;
    }
    window->setOnAllDesktops(onAllDesktops);
    // In a real session this would eventually be picked up by whatever next
    // reconcile pass runs (output/desktop-topology change -- see the M4 PLAN
    // entry). Force one now so a test can observe the effect deterministically
    // without waiting on an unrelated topology event.
    scheduleReconcile();
    return true;
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

QString Ki3Tiler::focusedOutputName() const
{
    LogicalOutput *output = focusedOutput();
    return output ? output->name() : QString();
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

void Ki3Tiler::loadWorkspaceOutputPreferences()
{
    m_workspaceOutputPreference.clear();
    const KConfigGroup group =
        KSharedConfig::openConfig(QStringLiteral("ki3rc"))->group(QStringLiteral("Workspaces"));
    for (const QString &key : group.keyList()) {
        bool ok = false;
        const int number = key.toInt(&ok);
        if (!ok || number < 1) {
            qCWarning(KWIN_KI3) << "ki3rc [Workspaces]: ignoring invalid desktop number" << key;
            continue;
        }
        QStringList outputNames = group.readEntry(key, QStringList());
        for (QString &name : outputNames) {
            name = name.trimmed();
        }
        outputNames.removeAll(QString());
        if (outputNames.isEmpty()) {
            continue;
        }
        m_workspaceOutputPreference.insert(number, outputNames);
        qCDebug(KWIN_KI3) << "configured workspace preference: desktop" << number
                          << "->" << outputNames;
    }
}

void Ki3Tiler::claimConfiguredOutputs(QSet<LogicalOutput *> &claimedOutputs)
{
    if (m_workspaceOutputPreference.isEmpty()) {
        return;
    }
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    const auto outputs = workspace()->outputs();

    // QMap iterates in ascending key order, so a lower desktop number always
    // wins a conflicting claim over a higher one.
    for (auto it = m_workspaceOutputPreference.constBegin(); it != m_workspaceOutputPreference.constEnd(); ++it) {
        LogicalOutput *target = nullptr;
        for (const QString &name : it.value()) {
            LogicalOutput *candidate = nullptr;
            for (LogicalOutput *output : outputs) {
                if (output->name() == name) {
                    candidate = output;
                    break;
                }
            }
            if (candidate && !claimedOutputs.contains(candidate)) {
                target = candidate;
                break;
            }
        }
        if (!target) {
            continue; // none of this desktop's preferred outputs are connected/free
        }

        VirtualDesktop *desktop = getOrCreateDesktop(it.key());
        if (!desktop) {
            continue;
        }
        if (vdm->currentDesktop(target) != desktop) {
            // Evict whoever is currently stuck showing this desktop first, so we
            // never leave a stale duplicate around for enforceUniqueDesktops()'s
            // generic dedup pass to find — that pass resolves duplicates by
            // enumeration order, which could just as easily undo this claim as
            // fix it.
            if (LogicalOutput *staleHolder = outputShowingDesktop(desktop)) {
                VirtualDesktop *fallback = firstFreeDesktop();
                if (!fallback) {
                    fallback = createFreshDesktop();
                }
                if (fallback) {
                    vdm->setCurrent(fallback, staleHolder);
                }
            }
            vdm->setCurrent(desktop, target);
            qCDebug(KWIN_KI3) << "configured workspace" << it.key() << "-> output" << target->name();
        }
        claimedOutputs.insert(target);
    }
}

void Ki3Tiler::assignInitialDesktops()
{
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    const auto outputs = workspace()->outputs();

    // Configured preferences claim their output first; every other output falls
    // back to the lowest desktop number not reserved by *any* configured entry
    // (even one whose output isn't connected right now — that number still
    // "belongs" to it in the user's config, so it's not fair game as a filler).
    QSet<LogicalOutput *> claimed;
    claimConfiguredOutputs(claimed);

    int next = 1;
    for (LogicalOutput *output : outputs) {
        if (claimed.contains(output)) {
            continue;
        }
        while (m_workspaceOutputPreference.contains(next)) {
            ++next;
        }
        VirtualDesktop *desktop = getOrCreateDesktop(next);
        if (desktop) {
            vdm->setCurrent(desktop, output);
            qCDebug(KWIN_KI3) << "initial desktop" << next << "-> output" << output->name();
        }
        ++next;
    }

    if (!outputs.isEmpty()) {
        // getOrCreateDesktop() only ever adds desktops; it never touches KWin's
        // original pre-ki3 default desktop (whose name is never a plain number,
        // e.g. "Desktop 1"). If it's still unshown and windowless after the
        // above, drop it the same way any other empty desktop gets dropped.
        schedulePrune();
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
        // isDeleted() checked first, deliberately, before isInternal()/isClient()
        // (both virtual): a Window in KWin's transient "deleted" (closing-
        // animation) state can have a vtable that's unsafe to dispatch through
        // right then -- see TileTreeController::shouldManage()'s doc comment for
        // the gdb-confirmed crash this same pattern caused during output hot-unplug.
        // isDeleted() itself is the one non-virtual accessor here, safe
        // regardless.
        if (w != exclude && !w->isDeleted() && !w->isInternal() && w->isClient()
            && w->isNormalWindow() && w->output() && w->isOnDesktop(desktop)) {
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
        // isDeleted() first -- see outputForDesktop()'s comment above and
        // TileTreeController::shouldManage()'s for why, against isInternal()/
        // isClient() (both virtual) right after it in this same check.
        if (w && !w->isDeleted() && !w->isInternal() && w->isClient() && w->isNormalWindow()
            && w->isShown() && w->isOnOutput(output) && w->isOnDesktop(desktop)) {
            workspace()->activateWindow(w);
            return;
        }
    }
    qCDebug(KWIN_KI3) << "focusOutput: no window to activate on" << (void *)output;
    // i3/sway: switching to an empty workspace means nothing is focused, full
    // stop -- the previously active window must not go on "being active" just
    // because nothing replaced it. Without this, focusedOutput() (which prefers
    // activeWindow()->output() over activeOutput(), see below) would keep
    // reporting the old screen as focused, leaving the pager's focused-screen
    // highlight stuck there instead of following us to the empty desktop.
    if (Window *active = workspace()->activeWindow(); active && active->output() != output) {
        workspace()->activateWindow(nullptr);
    }
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

void Ki3Tiler::teardownGroupsOnOutput(LogicalOutput *output)
{
    TileManager *tm = workspace()->tileManager(output);
    if (!tm) {
        return;
    }
    // Collect first, then tear down -- destroyGroupHeader() mutates state
    // (setHeaderReserve() emits windowGeometryChanged) while we'd still be
    // inside visitDescendants() otherwise.
    QList<CustomTile *> groupsHere;
    for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
        RootTile *root = tm->rootTile(desktop);
        if (!root) {
            continue;
        }
        root->visitDescendants([this, &groupsHere](Tile *t) {
            auto *custom = static_cast<CustomTile *>(t);
            if (m_tileTree->isGroup(custom)) {
                groupsHere.append(custom);
            }
        });
    }
    for (CustomTile *tile : groupsHere) {
        qCDebug(KWIN_KI3) << "output going away: tearing down group on" << (void *)tile;
        m_tileTree->dropGroup(tile);
    }
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
    m_tileTree->purgeStaleRoots();
    enforceUniqueDesktops();
    retileHomelessWindows();
    ensureSaneFocus();
    m_tileTree->refreshAllGroups();
}

void Ki3Tiler::enforceUniqueDesktops()
{
    // Configured preferences reclaim their output first — e.g. a monitor that
    // was unplugged and just came back takes its desktop back from whatever
    // filled in for it. Self-contained (see claimConfiguredOutputs()), so the
    // generic dedup pass below sees already-consistent state for every output
    // it touched and simply no-ops on them.
    QSet<LogicalOutput *> claimed;
    claimConfiguredOutputs(claimed);

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
        if (!m_tileTree->shouldManage(window)) {
            // No longer manageable (e.g. its output vanished and rules changed);
            // drop any stale bookkeeping.
            if (m_tileTree->isManaged(window)) {
                m_tileTree->forgetWindow(window);
            }
            continue;
        }
        if (!m_tileTree->isManaged(window)) {
            // Relocated onto an output we weren't tracking it on -> tile it.
            m_tileTree->insertWindow(window);
            continue;
        }
        CustomTile *leaf = m_tileTree->leafFor(window);
        RootTile *want = m_tileTree->rootForWindow(window);
        if (!leaf || (want && leaf->rootTile() != want)) {
            // Tile destroyed by an unplug, or window moved to another tree.
            m_tileTree->forgetWindow(window);
            m_tileTree->insertWindow(window);
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
    const bool floating = m_tileTree->isFloating(window);
    if (!floating && !m_tileTree->shouldManage(window)) {
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
        m_tileTree->forgetWindow(window);
    }
    window->setDesktops({desktop});
    if (home && home != window->output()) {
        window->sendToOutput(home);
    }
    qCDebug(KWIN_KI3) << (floating ? "move floating window to workspace" : "move window to workspace") << number
                      << "on output" << (void *)window->output();
    if (floating) {
        repositionFloatChrome(window); // hide/reposition chrome for its new desktop
    } else {
        // Pass `home` explicitly: sendToOutput() above updates window->output()
        // only once the Wayland client acks the configure, so it can still report
        // the *old* output here. Without the hint the window would tile into the
        // (old-output, target-desktop) tree — hidden on that output — until an
        // unrelated re-home surfaced it. `home` may be null (target desktop has no
        // home yet); then insertWindow falls back to window->output(), which is
        // correct because no cross-output move happened.
        m_tileTree->insertWindow(window, home);
    }
    schedulePrune(); // the source desktop may now be empty
    // The target desktop just gained a home output (or the source lost one); the
    // pager's per-output list keys off that, but this changed neither the current
    // desktop nor the desktop count, so nothing else emits desktopsChanged().
    Q_EMIT desktopsChanged();
}

} // namespace KWin
