#include "wardriver.h"
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <dialogs/dialogs.h>
#include <string.h>
#include <stdio.h>
typedef struct { ViewDispatcher* dispatcher; bool accepted,name; } Editor;
static void saved(void* context) { Editor* e=context; e->accepted=true; view_dispatcher_stop(e->dispatcher); }
static bool back(void* context) { Editor* e=context; view_dispatcher_stop(e->dispatcher); return true; }
static bool validate(const char* value,FuriString* error,void* context) {
    Editor* e=context;
    if(wardriver_wigle_value_valid(value,e->name)) return true;
    furi_string_set_str(error,"Use printable API credentials\nwithout spaces or newlines"); return false;
}
void wardriver_wigle_dialog(WardriverApp* a,unsigned action) {
    /* Called by the app's input loop, never from a drawing callback or worker.
     * The native modal owns input until it returns. Secrets are not in snapshots. */
    view_port_enabled_set(a->view,false);
    bool changed=false;
    if(action==1 || action==2) {
        char value[WARD_WIGLE_TOKEN_SIZE]={0};
        if(action==1) {
            furi_mutex_acquire(a->lock,FuriWaitForever);
            snprintf(value,sizeof(value),"%s",a->wigle_credentials.name);
            furi_mutex_release(a->lock);
        } /* Editing a token starts empty; never reveal the saved token. */
        Editor e={.dispatcher=view_dispatcher_alloc(),.name=action==1};
        TextInput* input=text_input_alloc();
        view_dispatcher_set_event_callback_context(e.dispatcher,&e);
        view_dispatcher_set_navigation_event_callback(e.dispatcher,back);
        text_input_set_header_text(input,action==1?"WiGLE API Name":"New WiGLE API Token");
        text_input_set_minimum_length(input,1);
        text_input_set_validator(input,validate,&e);
        text_input_set_result_callback(input,saved,&e,value,action==1?WARD_WIGLE_NAME_SIZE:sizeof(value),false);
        view_dispatcher_add_view(e.dispatcher,0,text_input_get_view(input));
        view_dispatcher_attach_to_gui(e.dispatcher,a->gui,ViewDispatcherTypeFullscreen);
        view_dispatcher_switch_to_view(e.dispatcher,0); view_dispatcher_run(e.dispatcher);
        view_dispatcher_remove_view(e.dispatcher,0); text_input_free(input); view_dispatcher_free(e.dispatcher);
        if(e.accepted) {
            furi_mutex_acquire(a->lock,FuriWaitForever);
            strcpy(action==1?a->wigle_credentials.name:a->wigle_credentials.token,value);
            a->model.wigle_name_set=a->wigle_credentials.name[0]!=0;
            a->model.wigle_token_set=a->wigle_credentials.token[0]!=0;
            a->model.page=WardPageUploadStatus; strcpy(a->model.wigle_status,"Saving credentials to SD");
            furi_mutex_release(a->lock); changed=true;
        }
        wardriver_wigle_erase(value,sizeof(value));
    } else if(action==3) {
        DialogsApp* dialogs=furi_record_open(RECORD_DIALOGS);
        FuriString* path=furi_string_alloc_set_str(WARD_WIGLE_DIR);
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options,"_wigle.csv",NULL);
        options.base_path=WARD_WIGLE_DIR; options.hide_ext=false;
        if(dialog_file_browser_show(dialogs,path,path,&options)) {
            const char* selected=furi_string_get_cstr(path);
            furi_mutex_acquire(a->lock,FuriWaitForever);
            if(wardriver_wigle_path_valid(selected)) {
                snprintf(a->model.wigle_path,sizeof(a->model.wigle_path),"%s",selected);
            } else { a->model.page=WardPageUploadStatus; strcpy(a->model.wigle_status,"Select a wardrive WiGLE CSV"); }
            furi_mutex_release(a->lock);
        }
        furi_string_free(path); furi_record_close(RECORD_DIALOGS);
    }
    InputEvent discard; while(furi_message_queue_get(a->inputs,&discard,0)==FuriStatusOk) {}
    furi_mutex_acquire(a->lock,FuriWaitForever); a->model.wigle_busy=changed; furi_mutex_release(a->lock);
    if(changed) __atomic_store_n(&a->wigle_command,WardWigleSave,__ATOMIC_RELEASE);
    view_port_enabled_set(a->view,true); view_port_update(a->view);
}
