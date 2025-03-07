#include "MainWindow.h"

#include <utility>

#include <config-dev.h>
#include <config-features.h>
#include <config.h>
#include <gdk/gdk.h>

#include "control/Control.h"
#include "control/DeviceListHelper.h"
#include "control/layer/LayerController.h"
#include "gui/PdfFloatingToolbox.h"
#include "gui/inputdevices/InputEvents.h"
#include "gui/scroll/ScrollHandling.h"
#include "gui/toolbarMenubar/ToolMenuHandler.h"
#include "gui/toolbarMenubar/model/ToolbarData.h"
#include "gui/toolbarMenubar/model/ToolbarModel.h"
#include "gui/widgets/SpinPageAdapter.h"
#include "gui/widgets/XournalWidget.h"
#include "util/XojMsgBox.h"
#include "util/i18n.h"

#include "GladeSearchpath.h"
#include "Layout.h"
#include "MainWindowToolbarMenu.h"
#include "ToolbarDefinitions.h"
#include "ToolitemDragDrop.h"
#include "XournalView.h"

using std::string;

MainWindow::MainWindow(GladeSearchpath* gladeSearchPath, Control* control):
        GladeGui(gladeSearchPath, "main.glade", "mainWindow") {
    this->control = control;
    this->toolbarWidgets = new GtkWidget*[TOOLBAR_DEFINITIONS_LEN];
    this->toolbarSelectMenu = new MainWindowToolbarMenu(this);

    panedContainerWidget = GTK_WIDGET(get("panelMainContents"));
    boxContainerWidget = GTK_WIDGET(get("mainContentContainer"));
    mainContentWidget = GTK_WIDGET(get("boxContents"));
    sidebarWidget = GTK_WIDGET(get("sidebar"));
    g_object_ref(panedContainerWidget);
    g_object_ref(boxContainerWidget);
    g_object_ref(mainContentWidget);
    g_object_ref(sidebarWidget);

    GtkSettings* appSettings = gtk_settings_get_default();
    g_object_set(appSettings, "gtk-application-prefer-dark-theme", control->getSettings()->isDarkTheme(), NULL);

    loadMainCSS(gladeSearchPath, "xournalpp.css");

    GtkOverlay* overlay = GTK_OVERLAY(get("mainOverlay"));
    this->pdfFloatingToolBox = std::make_unique<PdfFloatingToolbox>(this, overlay);
    this->floatingToolbox = new FloatingToolbox(this, overlay);

    for (int i = 0; i < TOOLBAR_DEFINITIONS_LEN; i++) {
        GtkWidget* w = get(TOOLBAR_DEFINITIONS[i].guiName);
        g_object_ref(w);
        this->toolbarWidgets[i] = w;
    }

    initXournalWidget();

    setSidebarVisible(control->getSettings()->isSidebarVisible());

    // Window handler
    g_signal_connect(this->window, "delete-event", G_CALLBACK(deleteEventCallback), this->control);
    g_signal_connect(this->window, "window_state_event", G_CALLBACK(windowStateEventCallback), this);

    g_signal_connect(get("buttonCloseSidebar"), "clicked", G_CALLBACK(buttonCloseSidebarClicked), this);


    // "watch over" all events
    g_signal_connect(this->window, "key-press-event", G_CALLBACK(onKeyPressCallback), this);

    this->toolbar = new ToolMenuHandler(this->control, this, GTK_WINDOW(getWindow()));

    auto file = gladeSearchPath->findFile("", "toolbar.ini");

    ToolbarModel* tbModel = this->toolbar->getModel();

    if (!tbModel->parse(file, true)) {

        string msg = FS(_F("Could not parse general toolbar.ini file: {1}\n"
                           "No Toolbars will be available") %
                        file.u8string());
        XojMsgBox::showErrorToUser(control->getGtkWindow(), msg);
    }

    file = Util::getConfigFile(TOOLBAR_CONFIG);
    if (fs::exists(file)) {
        if (!tbModel->parse(file, false)) {
            string msg = FS(_F("Could not parse custom toolbar.ini file: {1}\n"
                               "Toolbars will not be available") %
                            file.u8string());
            XojMsgBox::showErrorToUser(control->getGtkWindow(), msg);
        }
    }

    createToolbarAndMenu();

    setToolbarVisible(control->getSettings()->isToolbarVisible());

    GtkWidget* menuViewSidebarVisible = get("menuViewSidebarVisible");
    g_signal_connect(menuViewSidebarVisible, "toggled", G_CALLBACK(viewShowSidebar), this);

    GtkWidget* menuViewToolbarsVisible = get("menuViewToolbarsVisible");
    g_signal_connect(menuViewToolbarsVisible, "toggled", G_CALLBACK(viewShowToolbar), this);

    updateScrollbarSidebarPosition();

    gtk_window_set_default_size(GTK_WINDOW(this->window), control->getSettings()->getMainWndWidth(),
                                control->getSettings()->getMainWndHeight());

    if (control->getSettings()->isMainWndMaximized()) {
        gtk_window_maximize(GTK_WINDOW(this->window));
    } else {
        gtk_window_unmaximize(GTK_WINDOW(this->window));
    }

    getSpinPageNo()->addListener(this->control->getScrollHandler());


    Util::execInUiThread([=]() {
        // Execute after the window is visible, else the check won't work
        initHideMenu();
    });

    // Drag and Drop
    g_signal_connect(this->window, "drag-data-received", G_CALLBACK(dragDataRecived), this);

    gtk_drag_dest_set(this->window, GTK_DEST_DEFAULT_ALL, nullptr, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(this->window);
    gtk_drag_dest_add_image_targets(this->window);
    gtk_drag_dest_add_text_targets(this->window);

    LayerCtrlListener::registerListener(control->getLayerController());
}

gboolean MainWindow::isKeyForClosure(GtkAccelKey* key, GClosure* closure, gpointer data) { return closure == data; }

gboolean MainWindow::invokeMenu(GtkWidget* widget) {
    // g_warning("invoke_menu %s", gtk_widget_get_name(widget));
    gtk_widget_activate(widget);
    return TRUE;
}

void MainWindow::rebindAcceleratorsMenuItem(GtkWidget* widget, gpointer user_data) {
    if (GTK_IS_MENU_ITEM(widget)) {
        GtkAccelGroup* newAccelGroup = reinterpret_cast<GtkAccelGroup*>(user_data);
        GList* menuAccelClosures = gtk_widget_list_accel_closures(widget);
        for (GList* l = menuAccelClosures; l != nullptr; l = l->next) {
            GClosure* closure = reinterpret_cast<GClosure*>(l->data);
            GtkAccelGroup* accelGroup = gtk_accel_group_from_accel_closure(closure);
            GtkAccelKey* key = gtk_accel_group_find(accelGroup, isKeyForClosure, closure);

            // g_warning("Rebind %s : %s", gtk_accelerator_get_label(key->accel_key, key->accel_mods),
            // gtk_widget_get_name(widget));

            gtk_accel_group_connect(newAccelGroup, key->accel_key, key->accel_mods, GtkAccelFlags(0),
                                    g_cclosure_new_swap(G_CALLBACK(MainWindow::invokeMenu), widget, nullptr));
        }

        MainWindow::rebindAcceleratorsSubMenu(widget, newAccelGroup);
    }
}

void MainWindow::rebindAcceleratorsSubMenu(GtkWidget* widget, gpointer user_data) {
    if (GTK_IS_MENU_ITEM(widget)) {
        GtkMenuItem* menuItem = reinterpret_cast<GtkMenuItem*>(widget);
        GtkWidget* subMenu = gtk_menu_item_get_submenu(menuItem);
        if (GTK_IS_CONTAINER(subMenu)) {
            gtk_container_foreach(reinterpret_cast<GtkContainer*>(subMenu), rebindAcceleratorsMenuItem, user_data);
        }
    }
}

// When the Menubar is hidden, accelerators no longer work so rebind them to the MainWindow
// It should be called after all plugins have been initialised so that their injected menu items are captured
void MainWindow::rebindMenubarAccelerators() {
    this->globalAccelGroup = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(this->getWindow()), this->globalAccelGroup);

    GtkMenuBar* menuBar = (GtkMenuBar*)this->get("mainMenubar");
    gtk_container_foreach(reinterpret_cast<GtkContainer*>(menuBar), rebindAcceleratorsSubMenu, this->globalAccelGroup);
}

MainWindow::~MainWindow() {
    for (int i = 0; i < TOOLBAR_DEFINITIONS_LEN; i++) { g_object_unref(this->toolbarWidgets[i]); }

    delete[] this->toolbarWidgets;
    this->toolbarWidgets = nullptr;

    delete this->toolbarSelectMenu;
    this->toolbarSelectMenu = nullptr;

    delete this->floatingToolbox;
    this->floatingToolbox = nullptr;

    delete this->xournal;
    this->xournal = nullptr;

    delete this->toolbar;
    this->toolbar = nullptr;

    delete scrollHandling;
    scrollHandling = nullptr;

    g_object_unref(panedContainerWidget);
    g_object_unref(boxContainerWidget);
    g_object_unref(mainContentWidget);
    g_object_unref(sidebarWidget);
}

/**
 * Topmost widgets, to check if there is a menu above
 */
const char* TOP_WIDGETS[] = {"tbTop1", "tbTop2", "mainContainerBox", nullptr};


void MainWindow::toggleMenuBar(MainWindow* win) {
    GtkWidget* menu = win->get("mainMenubar");
    if (gtk_widget_is_visible(menu)) {
        gtk_widget_hide(menu);
    } else {
        gtk_widget_show(menu);
    }
}

void MainWindow::updateColorscheme() {
    bool darkMode = control->getSettings()->isDarkTheme();
    GtkStyleContext* context = gtk_widget_get_style_context(GTK_WIDGET(this->window));

    if (darkMode) {
        gtk_style_context_add_class(context, "darkMode");
    } else {
        gtk_style_context_remove_class(context, "darkMode");
    }
}

void MainWindow::initXournalWidget() {
    GtkWidget* boxContents = get("boxContents");


    winXournal = gtk_scrolled_window_new(nullptr, nullptr);

    setGtkTouchscreenScrollingForDeviceMapping();

    gtk_container_add(GTK_CONTAINER(boxContents), winXournal);

    GtkWidget* vpXournal = gtk_viewport_new(nullptr, nullptr);

    gtk_container_add(GTK_CONTAINER(winXournal), vpXournal);

    scrollHandling = new ScrollHandling(GTK_SCROLLABLE(vpXournal));

    this->xournal = new XournalView(vpXournal, control, scrollHandling);

    control->getZoomControl()->initZoomHandler(this->window, winXournal, xournal, control);
    gtk_widget_show_all(winXournal);

    Layout* layout = gtk_xournal_get_layout(this->xournal->getWidget());
    scrollHandling->init(this->xournal->getWidget(), layout);

    updateColorscheme();
}

void MainWindow::setGtkTouchscreenScrollingForDeviceMapping() {
    InputDeviceClass touchscreenClass =
            DeviceListHelper::getSourceMapping(GDK_SOURCE_TOUCHSCREEN, this->getControl()->getSettings());

    setGtkTouchscreenScrollingEnabled(touchscreenClass == INPUT_DEVICE_TOUCHSCREEN &&
                                      !control->getSettings()->getTouchDrawingEnabled());
}

void MainWindow::setGtkTouchscreenScrollingEnabled(bool enabled) {
    if (!control->getSettings()->getGtkTouchInertialScrollingEnabled()) {
        enabled = false;
    }

    if (enabled == gtkTouchscreenScrollingEnabled.load() || winXournal == nullptr) {
        return;
    }

    gtkTouchscreenScrollingEnabled.store(enabled);

    Util::execInUiThread(
            [=]() {
                const bool touchScrollEnabled = gtkTouchscreenScrollingEnabled.load();

                gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(winXournal), touchScrollEnabled);
            },
            G_PRIORITY_HIGH);
}

/**
 * Allow to hide menubar, but only if global menu is not enabled
 */
void MainWindow::initHideMenu() {
    int top = -1;
    for (int i = 0; TOP_WIDGETS[i]; i++) {
        GtkWidget* w = get(TOP_WIDGETS[i]);
        GtkAllocation allocation;
        gtk_widget_get_allocation(w, &allocation);
        if (allocation.y != -1) {
            top = allocation.y;
            break;
        }
    }

    GtkWidget* menuItem = get("menuHideMenu");
    if (top < 5) {
        // There is no menu to hide, the menu is in the globalmenu!
        gtk_widget_hide(menuItem);
    } else {
        // Menu found, allow to hide it
        g_signal_connect(menuItem, "activate",
                         G_CALLBACK(+[](GtkMenuItem* menuitem, MainWindow* self) { toggleMenuBar(self); }), this);
    }

    // Hide menubar at startup if specified in settings
    Settings* settings = control->getSettings();
    if (settings && !settings->isMenubarVisible()) {
        toggleMenuBar(this);
    }
}

auto MainWindow::getLayout() -> Layout* { return gtk_xournal_get_layout(GTK_WIDGET(this->xournal->getWidget())); }

auto cancellable_cancel(GCancellable* cancel) -> bool {
    g_cancellable_cancel(cancel);

    g_warning("Timeout... Cancel loading URL");

    return false;
}

void MainWindow::dragDataRecived(GtkWidget* widget, GdkDragContext* dragContext, gint x, gint y, GtkSelectionData* data,
                                 guint info, guint time, MainWindow* win) {
    GtkWidget* source = gtk_drag_get_source_widget(dragContext);
    if (source && widget == gtk_widget_get_toplevel(source)) {
        gtk_drag_finish(dragContext, false, false, time);
        return;
    }

    guchar* text = gtk_selection_data_get_text(data);
    if (text) {
        win->control->clipboardPasteText(reinterpret_cast<const char*>(text));

        g_free(text);
        gtk_drag_finish(dragContext, true, false, time);
        return;
    }

    GdkPixbuf* image = gtk_selection_data_get_pixbuf(data);
    if (image) {
        win->control->clipboardPasteImage(image);

        g_object_unref(image);
        gtk_drag_finish(dragContext, true, false, time);
        return;
    }

    gchar** uris = gtk_selection_data_get_uris(data);
    if (uris) {
        for (int i = 0; uris[i] != nullptr && i < 3; i++) {
            const char* uri = uris[i];

            GCancellable* cancel = g_cancellable_new();
            int cancelTimeout = g_timeout_add(3000, reinterpret_cast<GSourceFunc>(cancellable_cancel), cancel);

            GFile* file = g_file_new_for_uri(uri);
            GError* err = nullptr;
            GFileInputStream* in = g_file_read(file, cancel, &err);
            if (g_cancellable_is_cancelled(cancel)) {
                continue;
            }

            g_object_unref(file);
            if (err == nullptr) {
                GdkPixbuf* pixbuf = gdk_pixbuf_new_from_stream(G_INPUT_STREAM(in), cancel, nullptr);
                if (g_cancellable_is_cancelled(cancel)) {
                    continue;
                }
                g_input_stream_close(G_INPUT_STREAM(in), cancel, nullptr);
                if (g_cancellable_is_cancelled(cancel)) {
                    continue;
                }

                if (pixbuf) {
                    win->control->clipboardPasteImage(pixbuf);

                    g_object_unref(pixbuf);
                }
            } else {
                g_error_free(err);
            }

            if (!g_cancellable_is_cancelled(cancel)) {
                g_source_remove(cancelTimeout);
            }
            g_object_unref(cancel);
        }

        gtk_drag_finish(dragContext, true, false, time);

        g_strfreev(uris);
    }

    gtk_drag_finish(dragContext, false, false, time);
}

void MainWindow::viewShowSidebar(GtkCheckMenuItem* checkmenuitem, MainWindow* win) {
    bool a = gtk_check_menu_item_get_active(checkmenuitem);
    if (win->control->getSettings()->isSidebarVisible() == a) {
        return;
    }
    win->setSidebarVisible(a);
}

void MainWindow::viewShowToolbar(GtkCheckMenuItem* checkmenuitem, MainWindow* win) {
    bool showToolbar = gtk_check_menu_item_get_active(checkmenuitem);
    if (win->control->getSettings()->isToolbarVisible() == showToolbar) {
        return;
    }
    win->setToolbarVisible(showToolbar);
}

auto MainWindow::getControl() -> Control* { return control; }

void MainWindow::updateScrollbarSidebarPosition() {
    GtkWidget* panelMainContents = get("panelMainContents");

    if (winXournal != nullptr) {
        GtkScrolledWindow* scrolledWindow = GTK_SCROLLED_WINDOW(winXournal);

        ScrollbarHideType type = this->getControl()->getSettings()->getScrollbarHideType();

        bool scrollbarOnLeft = control->getSettings()->isScrollbarOnLeft();
        if (scrollbarOnLeft) {
            gtk_scrolled_window_set_placement(scrolledWindow, GTK_CORNER_TOP_RIGHT);
        } else {
            gtk_scrolled_window_set_placement(scrolledWindow, GTK_CORNER_TOP_LEFT);
        }

        gtk_widget_set_visible(gtk_scrolled_window_get_hscrollbar(scrolledWindow), !(type & SCROLLBAR_HIDE_HORIZONTAL));
        gtk_widget_set_visible(gtk_scrolled_window_get_vscrollbar(scrolledWindow), !(type & SCROLLBAR_HIDE_VERTICAL));

        gtk_scrolled_window_set_overlay_scrolling(scrolledWindow,
                                                  !control->getSettings()->isScrollbarFadeoutDisabled());
    }

    // If the sidebar isn't visible, we can't change its position!
    if (!this->sidebarVisible) {
        return;
    }

    GtkWidget* sidebar = get("sidebar");
    GtkWidget* boxContents = get("boxContents");

    int divider = gtk_paned_get_position(GTK_PANED(panelMainContents));
    bool sidebarRight = control->getSettings()->isSidebarOnRight();
    if (sidebarRight == (gtk_paned_get_child2(GTK_PANED(panelMainContents)) == sidebar)) {
        // Already correct
        return;
    }


    GtkAllocation allocation;
    gtk_widget_get_allocation(panelMainContents, &allocation);
    divider = allocation.width - divider;


    g_object_ref(sidebar);
    g_object_ref(boxContents);

    gtk_container_remove(GTK_CONTAINER(panelMainContents), sidebar);
    gtk_container_remove(GTK_CONTAINER(panelMainContents), boxContents);

    if (sidebarRight) {
        gtk_paned_pack1(GTK_PANED(panelMainContents), boxContents, true, false);
        gtk_paned_pack2(GTK_PANED(panelMainContents), sidebar, false, false);
    } else {
        gtk_paned_pack1(GTK_PANED(panelMainContents), sidebar, false, false);
        gtk_paned_pack2(GTK_PANED(panelMainContents), boxContents, true, false);
    }

    gtk_paned_set_position(GTK_PANED(panelMainContents), divider);
    g_object_unref(sidebar);
    g_object_unref(boxContents);
}

void MainWindow::buttonCloseSidebarClicked(GtkButton* button, MainWindow* win) { win->setSidebarVisible(false); }

auto MainWindow::onKeyPressCallback(GtkWidget* widget, GdkEventKey* event, MainWindow* win) -> bool {

    if (win->getXournal()->getSelection()) {
        // something is selected - give that control
        return false;
    }
    if (win->getXournal()->getTextEditor()) {
        // editing text - give that control
        return false;
    }
    if (event->keyval == GDK_KEY_Escape) {
        win->getControl()->getSearchBar()->showSearchBar(false);
        return true;
    }


    return false;
}

auto MainWindow::deleteEventCallback(GtkWidget* widget, GdkEvent* event, Control* control) -> bool {
    control->quit();

    return true;
}

void MainWindow::setSidebarVisible(bool visible) {
    Settings* settings = control->getSettings();

    settings->setSidebarVisible(visible);
    if (!visible && (control->getSidebar() != nullptr)) {
        saveSidebarSize();
    }

    if (visible != this->sidebarVisible) {
        // Due to a GTK bug, we can't just hide the sidebar widget in the GtkPaned.
        // If we do this, we create a dead region where the pane separator was previously.
        // In this region, we can't use the touchscreen to start horizontal strokes.
        // As such:
        if (!visible) {
            gtk_container_remove(GTK_CONTAINER(panedContainerWidget), mainContentWidget);
            gtk_container_remove(GTK_CONTAINER(boxContainerWidget), GTK_WIDGET(panedContainerWidget));
            gtk_container_add(GTK_CONTAINER(boxContainerWidget), mainContentWidget);
            this->sidebarVisible = false;
        } else {
            gtk_container_remove(GTK_CONTAINER(boxContainerWidget), mainContentWidget);
            gtk_container_add(GTK_CONTAINER(panedContainerWidget), mainContentWidget);
            gtk_container_add(GTK_CONTAINER(boxContainerWidget), GTK_WIDGET(panedContainerWidget));
            this->sidebarVisible = true;

            updateScrollbarSidebarPosition();
        }
    }

    gtk_widget_set_visible(sidebarWidget, visible);

    if (visible) {
        gtk_paned_set_position(GTK_PANED(panedContainerWidget), settings->getSidebarWidth());
    }

    GtkWidget* w = get("menuViewSidebarVisible");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w), visible);
}

void MainWindow::setToolbarVisible(bool visible) {
    Settings* settings = control->getSettings();

    settings->setToolbarVisible(visible);
    for (int i = 0; i < TOOLBAR_DEFINITIONS_LEN; i++) {
        auto widget = this->toolbarWidgets[i];
        if (!visible || GTK_IS_CONTAINER(widget) && gtk_container_get_children(GTK_CONTAINER(widget))) {
            gtk_widget_set_visible(widget, visible);
        }
    }

    GtkWidget* w = get("menuViewToolbarsVisible");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w), visible);
}

void MainWindow::saveSidebarSize() {
    this->control->getSettings()->setSidebarWidth(gtk_paned_get_position(GTK_PANED(panedContainerWidget)));
}

void MainWindow::setMaximized(bool maximized) { this->maximized = maximized; }

auto MainWindow::isMaximized() const -> bool { return this->maximized; }

auto MainWindow::getXournal() -> XournalView* { return xournal; }

auto MainWindow::windowStateEventCallback(GtkWidget* window, GdkEventWindowState* event, MainWindow* win) -> bool {
    win->setMaximized(gtk_window_is_maximized(GTK_WINDOW(window)));

    return false;
}

void MainWindow::reloadToolbars() {
    bool inDragAndDrop = this->control->isInDragAndDropToolbar();

    ToolbarData* d = getSelectedToolbar();

    if (inDragAndDrop) {
        this->control->endDragDropToolbar();
    }

    this->clearToolbar();
    this->toolbarSelected(d);

    if (inDragAndDrop) {
        this->control->startDragDropToolbar();
    }
}

void MainWindow::toolbarSelected(ToolbarData* d) {
    if (!this->toolbarIntialized || this->selectedToolbar == d) {
        return;
    }

    Settings* settings = control->getSettings();
    settings->setSelectedToolbar(d->getId());

    this->clearToolbar();
    this->loadToolbar(d);
}

auto MainWindow::clearToolbar() -> ToolbarData* {
    if (this->selectedToolbar != nullptr) {
        for (int i = 0; i < TOOLBAR_DEFINITIONS_LEN; i++) { ToolMenuHandler::unloadToolbar(this->toolbarWidgets[i]); }

        this->toolbar->freeDynamicToolbarItems();
    }

    ToolbarData* oldData = this->selectedToolbar;

    this->selectedToolbar = nullptr;

    return oldData;
}

void MainWindow::loadToolbar(ToolbarData* d) {
    this->selectedToolbar = d;

    for (int i = 0; i < TOOLBAR_DEFINITIONS_LEN; i++) {
        this->toolbar->load(d, this->toolbarWidgets[i], TOOLBAR_DEFINITIONS[i].propName,
                            TOOLBAR_DEFINITIONS[i].horizontal);
    }

    this->floatingToolbox->flagRecalculateSizeRequired();
}

auto MainWindow::getSelectedToolbar() -> ToolbarData* { return this->selectedToolbar; }

auto MainWindow::getToolbarWidgets(int& length) -> GtkWidget** {
    length = TOOLBAR_DEFINITIONS_LEN;
    return this->toolbarWidgets;
}

auto MainWindow::getToolbarName(GtkToolbar* toolbar) -> const char* {
    for (int i = 0; i < TOOLBAR_DEFINITIONS_LEN; i++) {
        if (static_cast<void*>(this->toolbarWidgets[i]) == static_cast<void*>(toolbar)) {
            return TOOLBAR_DEFINITIONS[i].propName;
        }
    }

    return "";
}

void MainWindow::setControlTmpDisabled(bool disabled) {
    toolbar->setTmpDisabled(disabled);
    toolbarSelectMenu->setTmpDisabled(disabled);

    GtkWidget* menuFileRecent = get("menuFileRecent");
    gtk_widget_set_sensitive(menuFileRecent, !disabled);
}

void MainWindow::updateToolbarMenu() { createToolbarAndMenu(); }

void MainWindow::createToolbarAndMenu() {
    GtkMenuShell* menubar = GTK_MENU_SHELL(get("menuViewToolbar"));
    g_return_if_fail(menubar != nullptr);

    toolbarSelectMenu->updateToolbarMenu(menubar, control->getSettings(), toolbar);

    ToolbarData* td = toolbarSelectMenu->getSelectedToolbar();
    if (td) {
        this->toolbarIntialized = true;
        toolbarSelected(td);
    }

    if (!this->control->getAudioController()->isPlaying()) {
        this->getToolMenuHandler()->disableAudioPlaybackButtons();
    }

    this->control->getScheduler()->unblockRerenderZoom();
}

void MainWindow::setFontButtonFont(XojFont& font) { toolbar->setFontButtonFont(font); }

auto MainWindow::getFontButtonFont() -> XojFont { return toolbar->getFontButtonFont(); }

void MainWindow::updatePageNumbers(size_t page, size_t pagecount, size_t pdfpage) {
    SpinPageAdapter* spinPageNo = getSpinPageNo();

    size_t min = 0;
    size_t max = pagecount;

    if (pagecount == 0) {
        min = 0;
        page = 0;
    } else {
        min = 1;
        page++;
    }

    spinPageNo->setMinMaxPage(min, max);
    spinPageNo->setPage(page);

    if (pdfpage != npos) {
        toolbar->setPageInfo(pagecount, pdfpage + 1);
    } else {
        toolbar->setPageInfo(pagecount);
    }
}

void MainWindow::rebuildLayerMenu() { layerVisibilityChanged(); }

void MainWindow::layerVisibilityChanged() {
    LayerController* lc = control->getLayerController();

    auto layer = lc->getCurrentLayerId();
    auto maxLayer = lc->getLayerCount();

    control->fireEnableAction(ACTION_DELETE_LAYER, layer > 0);
    control->fireEnableAction(ACTION_MERGE_LAYER_DOWN, layer > 1);
    control->fireEnableAction(ACTION_GOTO_NEXT_LAYER, layer < maxLayer);
    control->fireEnableAction(ACTION_GOTO_PREVIOUS_LAYER, layer > 0);
    control->fireEnableAction(ACTION_GOTO_TOP_LAYER, layer < maxLayer);
}

void MainWindow::setRecentMenu(GtkWidget* submenu) {
    GtkWidget* menuitem = get("menuFileRecent");
    g_return_if_fail(menuitem != nullptr);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuitem), submenu);
}

void MainWindow::show(GtkWindow* parent) { gtk_widget_show(this->window); }

void MainWindow::setUndoDescription(const string& description) { toolbar->setUndoDescription(description); }

void MainWindow::setRedoDescription(const string& description) { toolbar->setRedoDescription(description); }

auto MainWindow::getSpinPageNo() -> SpinPageAdapter* { return toolbar->getPageSpinner(); }

auto MainWindow::getToolbarModel() -> ToolbarModel* { return this->toolbar->getModel(); }

auto MainWindow::getToolMenuHandler() -> ToolMenuHandler* { return this->toolbar; }

void MainWindow::disableAudioPlaybackButtons() {
    setAudioPlaybackPaused(false);

    this->getToolMenuHandler()->disableAudioPlaybackButtons();
}

void MainWindow::enableAudioPlaybackButtons() { this->getToolMenuHandler()->enableAudioPlaybackButtons(); }

void MainWindow::setAudioPlaybackPaused(bool paused) { this->getToolMenuHandler()->setAudioPlaybackPaused(paused); }

void MainWindow::loadMainCSS(GladeSearchpath* gladeSearchPath, const gchar* cssFilename) {
    auto filepath = gladeSearchPath->findFile("", cssFilename);
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, filepath.u8string().c_str(), nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

PdfFloatingToolbox* MainWindow::getPdfToolbox() { return this->pdfFloatingToolBox.get(); }
