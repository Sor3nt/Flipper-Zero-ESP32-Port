#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define furi_check assert
#define VIEW_NONE UINT32_MAX
#define VIEW_IGNORE (UINT32_MAX - 1)
enum { ProtoPirateViewSubmenu, ProtoPirateViewVariableItemList, ReceiverView, ViewCount };
typedef int ViewPortOrientation;
static const ViewPortOrientation view_dispatcher_view_port_orientation_table[] = {0};
typedef struct { int orientation; bool freed; } View;
typedef struct { bool stopped; unsigned stop_count; } Loop;
typedef struct { int orientation; bool enabled; } Port;
typedef struct { View* views[ViewCount]; View* current_view; View* ongoing_input_view;
    void* gui; Loop* event_loop; Port* view_port; unsigned ongoing_input; } ViewDispatcher;
typedef struct { ViewDispatcher* view_dispatcher; void* variable_item_list;
    void* plugin_flipper_application; const void* config_plugin; } ProtoPirateApp;
static void furi_event_loop_stop(Loop* l) { l->stopped = true; l->stop_count++; }
static void gui_lock(void* g) {}
static void gui_unlock(void* g) {}
static View** ViewDict_get(View** views, uint32_t id) {
    assert(id < ViewCount); return views[id] ? &views[id] : NULL;
}
static bool ViewDict_erase(View** views, uint32_t id) {
    assert(views[id]); views[id] = NULL; return true;
}
static void view_exit(View* v) { assert(!v->freed); }
static void view_enter(View* v) { assert(!v->freed); }
static int view_port_get_orientation(Port* p) { return p->orientation; }
static void view_port_set_orientation(Port* p, int o) { p->orientation = o; }
static void view_port_enabled_set(Port* p, bool e) { p->enabled = e; }
static void view_port_update(Port* p) {}
static void view_set_update_callback(View* v, void* cb) { assert(!v->freed); }
static void view_set_update_callback_context(View* v, void* ctx) { assert(!v->freed); }
static void protopirate_variable_item_list_free(ProtoPirateApp* app);
static void shared_plugin_unload(void** plugin, const void** api);
#include "production.inc"
static unsigned freed, unloaded;
static void protopirate_variable_item_list_free(ProtoPirateApp* app) {
    if(!app->variable_item_list) return;
    View* view = app->view_dispatcher->views[ProtoPirateViewVariableItemList];
    view_dispatcher_remove_view(app->view_dispatcher, ProtoPirateViewVariableItemList);
    view->freed = true; app->variable_item_list = NULL; freed++;
}
static void shared_plugin_unload(void** plugin, const void** api) {
    if(*plugin) { assert(freed > unloaded); unloaded++; }
    *plugin = NULL; *api = NULL;
}
int main(void) {
    Loop loop = {0}; Port port = {0}; View menu = {0}, config = {0}, receiver = {0};
    ViewDispatcher dispatcher = {.event_loop = &loop, .view_port = &port};
    ProtoPirateApp app = {.view_dispatcher = &dispatcher};
    dispatcher.views[ProtoPirateViewSubmenu] = &menu;
    dispatcher.views[ReceiverView] = &receiver;
    // Negative control: old teardown stops even after opening the parent.
    dispatcher.views[ProtoPirateViewVariableItemList] = &config;
    app.variable_item_list = &config;
    view_dispatcher_switch_to_view(&dispatcher, ProtoPirateViewVariableItemList);
    protopirate_variable_item_list_free(&app);
    view_dispatcher_switch_to_view(&dispatcher, ReceiverView);
    assert(loop.stopped && loop.stop_count == 1); freed = 0;
    // Both launch paths, including the outstanding Back key release.
    for(unsigned i = 0; i < 100; i++) {
        loop = (Loop){0}; config = (View){0};
        dispatcher.views[ProtoPirateViewVariableItemList] = &config;
        app.variable_item_list = &config;
        app.plugin_flipper_application = &config; app.config_plugin = &config;
        view_dispatcher_switch_to_view(&dispatcher, ProtoPirateViewVariableItemList);
        dispatcher.ongoing_input_view = &config;
        protopirate_scene_receiver_config_on_exit(&app);
        assert(!loop.stopped && !loop.stop_count);
        assert(dispatcher.current_view == &menu && !dispatcher.ongoing_input_view);
        assert(!app.variable_item_list && !app.config_plugin && config.freed);
        view_dispatcher_switch_to_view(&dispatcher, i % 2 ? ReceiverView : ProtoPirateViewSubmenu);
        assert(!loop.stopped && port.enabled);
    }
    assert(freed == 100 && unloaded == 100);
    // Failed plugin entry has no settings view to remove.
    protopirate_scene_receiver_config_on_exit(&app);
    assert(!loop.stopped && freed == 100);
    view_dispatcher_stop(&dispatcher); assert(loop.stopped);
    puts("PASS: old exit reproduced; 100 Configuration exits preserve event loop, clear input, free before unload; explicit exit works");
}
