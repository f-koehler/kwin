/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3tiler.h"
#include "ki3_logging.h"

#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <QDBusConnection>

namespace KWin
{

Ki3Tiler::Ki3Tiler()
    : m_sessionGuard(std::make_unique<Ki3SessionGuard>())
    , m_tileTree(std::make_unique<TileTreeController>())
    , m_decoration(std::make_unique<DecorationController>(m_tileTree.get()))
    , m_workspace(std::make_unique<WorkspaceController>(m_tileTree.get(), m_decoration.get()))
    , m_shortcuts(std::make_unique<ShortcutController>(m_sessionGuard.get(), m_tileTree.get(),
                                                       m_decoration.get(), m_workspace.get(), this))
{
    qCInfo(KWIN_KI3) << "ki3 tiling plugin loaded";

    // Snapshot whatever ki3 is about to overwrite in the real kwinrc/
    // kglobalshortcutsrc, before anything below touches either -- see
    // ki3sessionguard.h/ki3session.cpp and ~Ki3Tiler().
    m_sessionGuard->backupIfNeeded();

    Workspace *ws = Workspace::self();
    connect(ws, &Workspace::windowAdded, this, &Ki3Tiler::handleWindowAdded);
    connect(ws, &Workspace::windowRemoved, this, &Ki3Tiler::handleWindowRemoved);
    connect(ws, &Workspace::windowActivated, this, &Ki3Tiler::handleWindowActivated);

    // KWin re-syncs its active output to the active window's output on every
    // activation (activation.cpp), and to the pointer/touch position otherwise
    // (e.g. clicking empty desktop background) -- so this alone tracks every
    // way focusedOutput()'s result can change, without hooking windowActivated
    // too. Forwarded to the pager over D-Bus so it can mute the highlight on
    // every screen except the focused one.
    connect(ws, &Workspace::activeOutputChanged, this, &Ki3Tiler::focusedOutputChanged);
    // WorkspaceController::desktopsChanged() and ShortcutController::
    // resizeModeChanged() are plain internal signals (the D-Bus-visible ones
    // must be emitted from this, the registered /Ki3 object) -- see their
    // doc comments.
    connect(m_workspace.get(), &WorkspaceController::desktopsChanged, this, &Ki3Tiler::desktopsChanged);
    connect(m_shortcuts.get(), &ShortcutController::resizeModeChanged, this, &Ki3Tiler::resizeModeChanged);

    // i3/sway model: each output independently shows one workspace. Unlike
    // Plasma's default (one desktop spanning all outputs), a desktop number here
    // exists on exactly one screen, and only desktops that are either shown or
    // occupied by windows exist at all — empty desktops are removed on the fly.
    VirtualDesktopManager::self()->setPerOutputVirtualDesktops(true);
    m_workspace->assignInitialDesktops();

    // Registering shortcuts must happen after m_sessionGuard->backupIfNeeded()
    // above: every key ShortcutController steals gets captured against that
    // backup first (see Ki3SessionGuard::noteShortcutGrab()), so registering
    // any earlier would silently lose the reversible-session guarantee.
    m_shortcuts->registerShortcuts();
    m_shortcuts->registerResizeModeShortcuts();
    m_shortcuts->disableOverviewHotCorner();

    // Adopt any windows that already exist when the plugin loads.
    for (Window *window : ws->windows()) {
        handleWindowAdded(window);
    }
    m_decoration->updateSplitIndicator();

    // Export /Ki3 on KWin's own "org.kde.KWin" service (already owned by this
    // process via its DBusInterface, alongside /Effects, /Compositor, ...) so
    // ki3-pager can show/drive per-output desktops without linking KWin internals.
    QDBusConnection::sessionBus().registerObject(
        QStringLiteral("/Ki3"), this,
        QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals);
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::currentChanged,
            this, &Ki3Tiler::desktopsChanged);
    // Notify the pager whenever the set of desktops changes (dynamic add/remove).
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::desktopAdded,
            this, [this](VirtualDesktop *) {
        Q_EMIT desktopsChanged();
    });
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::desktopRemoved,
            this, [this](VirtualDesktop *) {
        Q_EMIT desktopsChanged();
    });
}

Ki3Tiler::~Ki3Tiler()
{
    teardownManagedState();
    m_sessionGuard->restoreOnCleanExit();
}

void Ki3Tiler::teardownManagedState()
{
    // Order matters: destroy floating chrome first (self-contained on
    // m_decoration), then hand back every floating/tiled window's pre-ki3
    // decoration/keep-above state and detach every tile (self-contained on
    // m_tileTree), then drop ki3's own split structure.
    m_decoration->teardownFloatChrome();
    m_tileTree->detachAllManagedWindows();
    m_tileTree->dropManagedRoots();
}

void Ki3Tiler::handleWindowAdded(Window *window)
{
    m_decoration->watchWindow(window);

    if (m_tileTree->isManaged(window)) {
        return;
    }
    if (!m_tileTree->shouldManage(window)) {
        if (window && m_tileTree->isNonTileable(window)) {
            qCDebug(KWIN_KI3) << "not tiling" << window->resourceClass()
                              << "- matched a non-tileable rule";
        } else if (window && !window->isDeleted() && window->desktops().size() != 1) {
            qCDebug(KWIN_KI3) << "not tiling" << window->resourceClass()
                              << "- sticky/multi-desktop window, treating as floating (i3 policy)";
        }
        return;
    }
    m_tileTree->insertWindow(window);
}

void Ki3Tiler::handleWindowRemoved(Window *window)
{
    m_tileTree->removeFloating(window);
    m_decoration->destroyFloatChrome(window);
    m_tileTree->forgetWindow(window);
    m_tileTree->dropPresentationBaseline(window); // window is gone; drop its baseline
    // Nothing left to resize (e.g. the last window on a desktop just closed):
    // leaving resize mode on would silently do nothing on the next keypress
    // and strand the border indicator's "mode is active" implication.
    if (m_shortcuts->resizeModeActive() && !m_tileTree->currentLeaf()) {
        m_shortcuts->setResizeMode(false);
    }
    // Deferred: let KWin finish destroying the window before we check whether
    // its desktop became empty (the window may still appear in workspace()->windows()).
    m_workspace->schedulePrune();
}

void Ki3Tiler::toggleFloating()
{
    Window *window = workspace()->activeWindow();
    if (!window) {
        return;
    }
    if (m_tileTree->isFloating(window)) {
        // Re-tile it.
        m_tileTree->removeFloating(window);
        m_decoration->destroyFloatChrome(window);
        m_tileTree->restoreKeepAbove(window); // back to whatever it was before ki3 floated it
        qCDebug(KWIN_KI3) << "unfloat" << window->caption();
        m_tileTree->insertWindow(window);
    } else if (m_tileTree->isManaged(window)) {
        // Detach from the tree; it keeps its current geometry and floats.
        m_tileTree->addFloating(window);
        qCDebug(KWIN_KI3) << "float" << window->caption();
        m_tileTree->forgetWindow(window); // restores its pre-ki3 decoration policy; createFloatChrome() below re-hides it
        window->requestTile(nullptr);
        window->setNoBorder(true); // ki3 draws its own title bar instead of the native SSD
        // i3/sway: floating windows always stay above tiled ones, even across
        // focus changes -- KWin's default focus-follows-raise would otherwise
        // sink this behind a tiled window the user clicks on next.
        window->setKeepAbove(true);
        m_decoration->createFloatChrome(window);
    }
}

void Ki3Tiler::handleWindowActivated(Window *window)
{
    // Track the focused leaf so the *next* window splits the right container.
    m_tileTree->noteWindowActivated(window);
    m_decoration->updateSplitIndicator();
    // Reposition/hide group headers (covers desktop switches, which activate a
    // window on the newly shown desktop).
    m_tileTree->refreshAllGroups();
    // A floating window's chrome border tracks focus the same way; see
    // DecorationController::updateFloatChromeBorder().
    m_decoration->updateAllFloatChromeBorders();
}

// D-Bus forwarders: the /Ki3 object registered on the session bus must be
// this Ki3Tiler instance (see the class doc comment), so every Q_SCRIPTABLE
// slot backed by WorkspaceController/ShortcutController is a thin one-line
// forwarder.

QStringList Ki3Tiler::outputNames() const
{
    return m_workspace->outputNames();
}

int Ki3Tiler::currentDesktopNumber(const QString &outputName) const
{
    return m_workspace->currentDesktopNumber(outputName);
}

int Ki3Tiler::desktopCount() const
{
    return m_workspace->desktopCount();
}

QList<int> Ki3Tiler::liveDesktopNumbers() const
{
    return m_workspace->liveDesktopNumbers();
}

QList<int> Ki3Tiler::desktopNumbersForOutput(const QString &outputName) const
{
    return m_workspace->desktopNumbersForOutput(outputName);
}

QString Ki3Tiler::focusedOutputName() const
{
    return m_workspace->focusedOutputName();
}

void Ki3Tiler::dbusSwitchToWorkspace(int number)
{
    m_workspace->switchToWorkspace(number);
}

void Ki3Tiler::dbusMoveActiveToWorkspace(int number)
{
    m_workspace->moveActiveToWorkspace(number);
}

bool Ki3Tiler::resizeModeActive() const
{
    return m_shortcuts->resizeModeActive();
}

QStringList Ki3Tiler::tiledWindowGeometries() const
{
    return m_workspace->tiledWindowGeometries();
}

QStringList Ki3Tiler::floatingWindowGeometries() const
{
    return m_workspace->floatingWindowGeometries();
}

bool Ki3Tiler::activeWindowNoBorder() const
{
    return m_workspace->activeWindowNoBorder();
}

bool Ki3Tiler::activeWindowKeepAbove() const
{
    return m_workspace->activeWindowKeepAbove();
}

QString Ki3Tiler::dbusAddTestOutput()
{
    return m_workspace->addTestOutput();
}

QString Ki3Tiler::dbusRemoveTestOutput()
{
    return m_workspace->removeTestOutput();
}

bool Ki3Tiler::dbusSetActiveWindowNoBorder(bool noBorder)
{
    return m_workspace->setActiveWindowNoBorder(noBorder);
}

bool Ki3Tiler::dbusSetActiveWindowKeepAbove(bool keepAbove)
{
    return m_workspace->setActiveWindowKeepAbove(keepAbove);
}

bool Ki3Tiler::dbusSetActiveWindowOnAllDesktops(bool onAllDesktops)
{
    return m_workspace->setActiveWindowOnAllDesktops(onAllDesktops);
}

} // namespace KWin
