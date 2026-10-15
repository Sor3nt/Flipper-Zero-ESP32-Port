#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "m-array.h"
#define furi_check(x) assert(x)
#define ENABLE_SUB_DECODE_SCENE
#define ENABLE_EMULATE_FEATURE
#include "types.inc"
#include "manager.inc"
typedef enum { InputKeyUp,InputKeyDown,InputKeyLeft,InputKeyRight,InputKeyOk,InputKeyBack } InputKey;
typedef enum {InputTypePress,InputTypeRelease,InputTypeShort,InputTypeLong,InputTypeRepeat} InputType;
typedef struct {InputKey key;InputType type;} InputEvent;
typedef enum {GuiButtonTypeLeft,GuiButtonTypeCenter,GuiButtonTypeRight} GuiButtonType;
typedef void (*ButtonCallback)(GuiButtonType,InputType,void*);
typedef struct {unsigned count;} Submenu;
typedef struct {int mode;} Widget;
typedef struct {char label[32];ButtonCallback callback;void* context;GuiButtonType button;uint32_t event;} ProtoPirateAction;
typedef struct {void* history;uint16_t idx_menu_chosen;} ProtoPirateTxRx;
typedef struct {ProtoPirateLock lock;bool sub_decode_mode;unsigned lock_count,history_item,count;bool dolphin_view;} ProtoPirateReceiverModel;
typedef struct {ProtoPirateReceiverModel* view;void (*callback)(ProtoPirateCustomEvent,void*);void* context;} ProtoPirateReceiver;
typedef struct {
 SceneManager* scene_manager;void* view_dispatcher;Submenu *submenu,*actions_menu;ProtoPirateAction actions[3];uint8_t action_count;bool actions_visible;uint32_t actions_return_view;
 ProtoPirateTxRx* txrx;ProtoPirateReceiver* protopirate_receiver;void* view_about;
} ProtoPirateApp;
enum {ProtoPirateSceneStart,ProtoPirateSceneReceiver,ProtoPirateSceneSaved,ProtoPirateSceneReceiverConfig,ProtoPirateSceneSubDecode,ProtoPirateSceneAbout,ProtoPirateSceneEmulate,ProtoPirateSceneNum};
static unsigned current_view,stopped,events,last_event,selected_calls;static GuiButtonType selected_button;
static void view_dispatcher_send_custom_event(void* v,uint32_t e){events++;last_event=e;}
static void view_dispatcher_switch_to_view(void* v,uint32_t id){current_view=id;}
static void view_dispatcher_stop(void* v){stopped++;}
static void submenu_reset(Submenu* s){s->count=0;}
static void submenu_set_header(Submenu* s,const char* text){}
static void submenu_set_selected_item(Submenu* s,uint32_t i){}
static void submenu_add_item(Submenu* s,const char* label,uint32_t index,void (*cb)(void*,uint32_t),void* c){s->count++;}
static void protopirate_release_shared_radio_state(ProtoPirateApp* app){}
static void protopirate_variable_item_list_free(ProtoPirateApp* app){}
static void protopirate_widget_free(ProtoPirateApp* app){}
static void widget_reset(Widget* w){w->mode=0;}
static void widget_add_button_element(Widget* w,GuiButtonType b,const char* t,ButtonCallback cb,void* ctx){w->mode=99;}
static Widget* widget_get_view(Widget* w){return w;}
#define ViewInputModeUpDown 0
static void view_set_input_mode(Widget* w,int mode){w->mode=mode;}
static unsigned protopirate_history_get_item(void* h){return *(unsigned*)h;}
static uint16_t protopirate_view_receiver_get_idx_menu(ProtoPirateReceiver* r){return r->view->history_item;}
static void protopirate_view_receiver_set_idx_menu(ProtoPirateReceiver* r,uint16_t i){r->view->history_item=i;}
#include "actions.inc"
#include "start.inc"
#include "navigation.inc"
#define UNLOCK_CNT 3
#define with_view_model(view,type,body,redraw) do {type=(view);body;} while(0)
static size_t protopirate_view_receiver_item_count(ProtoPirateReceiverModel* m){return m->count;}
static void protopirate_view_receiver_update_offset(ProtoPirateReceiver* r){}
#include "receiver.inc"
typedef const char FuriString;
typedef struct {bool replay_only,is_transmitting;unsigned rotary_button,original_button,current_counter;FuriString* protocol_name;void* flipper_format;} EmulateContext;
static EmulateContext* emulate_context;static InputKey mapped_key;
static const char* furi_string_get_cstr(FuriString* s){return s;}
static bool furi_string_equal(FuriString* s,const char* t){return !strcmp(s,t);}
#define RENAULT_PROTOCOL_V0_NAME "Renault V0"
static void view_commit_model(void* v,bool b){}
static uint8_t emu_button_for_protocol(const char* p,InputKey k,unsigned b,void* ff){mapped_key=k;return k;}
static void emulate_update_data(EmulateContext* ctx,uint8_t b){}
#include "emulate.inc"
#define furi_assert(x) assert(x)
typedef struct {bool locked;void* change_callback;} VariableItem;
typedef struct {VariableItem row;} TestItems;
typedef struct {bool editing,locked_message_visible;unsigned position;TestItems items;} VariableItemListModel;
typedef struct {VariableItemListModel* view;} VariableItemList;
static size_t VariableItemArray_size(TestItems items){return 1;}
static VariableItem* VariableItemArray_get_impl(TestItems* items,unsigned index){assert(index==0);return &items->row;}
#define VariableItemArray_get(items,index) VariableItemArray_get_impl(&(items),index)
static unsigned up_calls,down_calls,left_calls,right_calls,ok_calls;
static void variable_item_list_process_up(VariableItemList* v){up_calls++;}
static void variable_item_list_process_down(VariableItemList* v){down_calls++;}
static void variable_item_list_process_left(VariableItemList* v){left_calls++;}
static void variable_item_list_process_right(VariableItemList* v){right_calls++;}
static void variable_item_list_process_ok(VariableItemList* v){ok_calls++;}
#include "variable_input.inc"
static void enter(void* c){}
static void leave(void* c){}
static bool child_event(void* c,SceneManagerEvent e){last_event=e.event;return false;}
static void chosen(GuiButtonType b,InputType t,void* c){assert(t==InputTypeShort);selected_calls++;selected_button=b;}
static void receiver_callback(ProtoPirateCustomEvent e,void* c){view_dispatcher_send_custom_event(c,e);}
int main(void){
 Submenu menu={0},actions={0};unsigned count=4;
 ProtoPirateReceiverModel model={.count=4,.history_item=1};ProtoPirateReceiver receiver={&model,receiver_callback,NULL};
 ProtoPirateTxRx txrx={.history=&count};ProtoPirateApp app={.submenu=&menu,.actions_menu=&actions,.txrx=&txrx,.protopirate_receiver=&receiver};
 AppSceneOnEnterCallback enters[ProtoPirateSceneNum];AppSceneOnExitCallback exits[ProtoPirateSceneNum];AppSceneOnEventCallback handlers[ProtoPirateSceneNum];
 for(unsigned i=0;i<ProtoPirateSceneNum;i++){enters[i]=enter;exits[i]=leave;handlers[i]=child_event;}
 handlers[ProtoPirateSceneStart]=protopirate_scene_start_on_event;
 SceneManagerHandlers h={enters,handlers,exits,ProtoPirateSceneNum};app.scene_manager=scene_manager_alloc(&h,&app);
 for(unsigned i=0;i<ProtoPirateSceneNum;i++)scene_manager_set_scene_state(app.scene_manager,i,0);
 scene_manager_next_scene(app.scene_manager,ProtoPirateSceneStart);scene_manager_next_scene(app.scene_manager,ProtoPirateSceneReceiver);scene_manager_next_scene(app.scene_manager,ProtoPirateSceneReceiverConfig);
 assert(protopirate_app_back_event_callback(&app));assert(scene_manager_get_current_scene(app.scene_manager)==ProtoPirateSceneReceiver);
 assert(protopirate_app_back_event_callback(&app));assert(scene_manager_get_current_scene(app.scene_manager)==ProtoPirateSceneStart);
 for(unsigned i=0;i<5;i++) { assert(protopirate_app_back_event_callback(&app)); }
 assert(!stopped);
 Widget widget={0};protopirate_actions_widget_reset(&app,&widget);
 protopirate_widget_add_button_element(&widget,GuiButtonTypeLeft,"First",chosen,&app);
 protopirate_widget_add_button_element(&widget,GuiButtonTypeRight,"Second",chosen,&app);
 assert(widget.mode==ViewInputModeUpDown && !selected_calls);
 protopirate_actions_button_callback(GuiButtonTypeCenter,InputTypeShort,&app);
 protopirate_app_custom_event_callback(&app,last_event);assert(app.actions_visible && current_view==ProtoPirateViewActions);
 protopirate_actions_menu_callback(&app,1);protopirate_app_custom_event_callback(&app,last_event);
 assert(!app.actions_visible && selected_calls==1 && selected_button==GuiButtonTypeRight && current_view==ProtoPirateViewWidget);
 protopirate_app_custom_event_callback(&app,ProtoPirateCustomEventOpenActions);protopirate_app_back_event_callback(&app);
 assert(!app.actions_visible && current_view==ProtoPirateViewWidget && !stopped);
 scene_manager_next_scene(app.scene_manager,ProtoPirateSceneReceiver);
 protopirate_app_custom_event_callback(&app,ProtoPirateCustomEventOpenReceiverActions);assert(app.action_count==3);
 model.history_item=3; /* New RF capture while the menu is open. */
 protopirate_app_custom_event_callback(&app,ProtoPirateCustomEventAction0);
 assert(model.history_item==1 && last_event==ProtoPirateCustomEventViewReceiverOK);
 InputEvent event={InputKeyBack,InputTypeRepeat};events=0;
 assert(protopirate_view_receiver_input(&event,&receiver));assert(!events);
 event.type=InputTypeShort;protopirate_view_receiver_input(&event,&receiver);assert(events==1);
 event.key=InputKeyOk;events=0;protopirate_view_receiver_input(&event,&receiver);assert(last_event==ProtoPirateCustomEventOpenReceiverActions && events==1);
 EmulateContext ctx={.rotary_button=1,.protocol_name="Chrysler V0"};emulate_context=&ctx;events=0;
 event.key=InputKeyDown;event.type=InputTypePress;emulate_input_callback(&event,&app);
 event.type=InputTypeShort;emulate_input_callback(&event,&app);assert(ctx.rotary_button==2 && !ctx.is_transmitting && !events);
 event.key=InputKeyOk;event.type=InputTypePress;emulate_input_callback(&event,&app);
 assert(ctx.is_transmitting && mapped_key==InputKeyDown && last_event==ProtoPirateCustomEventEmulateTransmit);
 event.type=InputTypeRelease;emulate_input_callback(&event,&app);assert(last_event==ProtoPirateCustomEventEmulateStop);
 protopirate_scene_start_on_event(&app,(SceneManagerEvent){SceneManagerEventTypeCustom,SubmenuIndexProtoPirateExit});assert(stopped==1);
 VariableItemListModel vm={.items={{false,(void*)1}}};VariableItemList list={&vm};
 event=(InputEvent){InputKeyDown,InputTypeShort};variable_item_list_input_callback(&event,&list);assert(down_calls==1 && !right_calls);
 event.key=InputKeyOk;assert(variable_item_list_input_callback(&event,&list));assert(vm.editing && !ok_calls);
 event.key=InputKeyDown;variable_item_list_input_callback(&event,&list);assert(right_calls==1 && down_calls==1);
 event.key=InputKeyUp;event.type=InputTypeRepeat;variable_item_list_input_callback(&event,&list);assert(left_calls==1 && !up_calls);
 event.key=InputKeyBack;event.type=InputTypeShort;assert(variable_item_list_input_callback(&event,&list));assert(!vm.editing);
 assert(!variable_item_list_input_callback(&event,&list));
 event.key=InputKeyOk;event.type=InputTypeLong;assert(variable_item_list_input_callback(&event,&list));assert(ok_calls==1 && !vm.editing);
 scene_manager_free(app.scene_manager);
 puts("PASS settings encoder: click edits, rotation changes value, click/Back ends edit, next Back navigates, long-click preserves model reset");
 puts("PASS actual scene navigation: one-level Back, root stays, explicit Exit, Actions select/cancel and stable capture selection");
 puts("PASS actual input handlers: Back-repeat suppressed, rotary selection never transmits, encoder press/release activates/stops selected button");
}
