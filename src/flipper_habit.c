#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/popup.h>
#include <notification/notification_messages.h>

#include "habit_data.h"
#include "habit_view.h"
#include "habit_list_view.h"
#include "habit_nfc.h"

/* ---------------------------------------------------------------------------
 * App state
 * ------------------------------------------------------------------------- */

typedef enum {
    HabitViewIdSubmenu,
    HabitViewIdTextInput,
    HabitViewIdConfig,
    HabitViewIdDialog,
    HabitViewIdHabit,
    HabitViewIdList,
    HabitViewIdPopup,
} HabitViewId;

typedef enum {
    HabitSceneStart,
    HabitSceneHabit,
    HabitSceneName,
    HabitSceneConfig,
    HabitSceneEditList,
    HabitSceneDelete,
    HabitSceneNfcWrite,
    HabitSceneCount,
} HabitScene;

typedef enum {
    /* 0..HABIT_MAX-1 are habit indexes selected from a submenu */
    HabitEventAdd = 100,
    HabitEventEdit,
    HabitEventNameDone,
    HabitEventRename,
    HabitEventDelete,
    HabitEventDialogLeft,
    HabitEventDialogRight,
    HabitEventHabitChanged,
    HabitEventWriteNfc,
    HabitEventNfcUnknownTag,
    HabitEventNfcWriteOk,
    HabitEventNfcWriteFail,
    HabitEventNfcUnsupportedTag,
    HabitEventPopupDone,
    /* Habit list actions: HabitEventListBase + (HabitListAction << 8) + habit index */
    HabitEventListBase = 0x1000,
    /* A habit tag was scanned: HabitEventNfcTagBase + habit id */
    HabitEventNfcTagBase = 0x10000,
} HabitEvent;

#define HABIT_LIST_EVENT(action, index) (HabitEventListBase + ((action) << 8) + (index))

typedef enum {
    ConfigItemInterval,
    ConfigItemWriteNfc,
    ConfigItemRename,
    ConfigItemDelete,
} ConfigItem;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Submenu* submenu;
    TextInput* text_input;
    VariableItemList* config_list;
    DialogEx* dialog;
    HabitView* habit_view;
    HabitListView* habit_list;
    Popup* popup;
    HabitNfc* nfc;
    NotificationApp* notifications;

    HabitStore* store;
    uint8_t selected;
    /* True while walking through the "add habit" flow (name -> interval). */
    bool adding;
    char text_buf[HABIT_NAME_LEN];
    char popup_text[96]; /* habit name + two lines of instructions */
} HabitApp;

/* Persist to the SD card right away so progress survives crashes and reboots. */
static void habit_app_save(HabitApp* app) {
    if(!habit_store_save(app->store)) {
        FURI_LOG_E("HabitApp", "Could not save habits to SD card");
    }
}

static void habit_interval_text(uint8_t target, char* out, size_t size) {
    if(target <= 1) {
        snprintf(out, size, "Daily");
    } else {
        snprintf(out, size, "%ux a day", target);
    }
}

/* ---------------------------------------------------------------------------
 * Scene: Start (habit list)
 * ------------------------------------------------------------------------- */

static void habit_submenu_callback(void* context, uint32_t index) {
    HabitApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

/* Runs on the NFC worker thread: just forward to the app's event queue. */
static void habit_nfc_callback(void* context, HabitNfcEvent event, uint16_t habit_id) {
    HabitApp* app = context;
    uint32_t custom = 0;
    switch(event) {
    case HabitNfcEventHabitTag:
        custom = HabitEventNfcTagBase + habit_id;
        break;
    case HabitNfcEventUnknownTag:
        custom = HabitEventNfcUnknownTag;
        break;
    case HabitNfcEventWriteOk:
        custom = HabitEventNfcWriteOk;
        break;
    case HabitNfcEventWriteFail:
        custom = HabitEventNfcWriteFail;
        break;
    case HabitNfcEventUnsupportedTag:
        custom = HabitEventNfcUnsupportedTag;
        break;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, custom);
}

static void habit_list_callback(void* context, HabitListAction action, uint8_t index) {
    HabitApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HABIT_LIST_EVENT(action, index));
}

static void scene_start_refresh(HabitApp* app) {
    uint32_t today = habit_today();
    habit_store_sync(app->store, today);
    habit_list_view_set_store(app->habit_list, app->store, today);
}

static void scene_start_on_enter(void* context) {
    HabitApp* app = context;
    scene_start_refresh(app);
    habit_list_view_set_selected(
        app->habit_list, scene_manager_get_scene_state(app->scene_manager, HabitSceneStart));
    view_dispatcher_switch_to_view(app->view_dispatcher, HabitViewIdList);
    /* Scan for habit tags the whole time the list is on screen. */
    habit_nfc_start_scan(app->nfc);
}

/*
 * Log (+1) or remove (-1) one completion for today, straight from the list.
 * Returns false if nothing changed (already complete, or nothing to remove).
 */
static bool scene_start_track(HabitApp* app, uint8_t index, bool add) {
    if(index >= app->store->count) return false;
    uint32_t today = habit_today();
    Habit* habit = &app->store->habits[index];
    habit_sync(habit, today);

    uint8_t count = habit_get(habit, today);
    if(add && count < habit->target) {
        habit_set(habit, today, count + 1);
        notification_message(app->notifications, &sequence_blink_green_10);
        if(count + 1 == habit->target) {
            notification_message(app->notifications, &sequence_single_vibro);
        }
    } else if(!add && count > 0) {
        habit_set(habit, today, count - 1);
    } else {
        return false;
    }
    habit_app_save(app);
    scene_start_refresh(app);
    return true;
}

/* A habit tag was scanned: log it and give feedback that works without looking. */
static void scene_start_nfc_tag(HabitApp* app, uint16_t habit_id) {
    int index = habit_store_find_by_id(app->store, habit_id);
    if(index < 0) {
        /* Tag belongs to a habit that no longer exists (or to another Flipper). */
        notification_message(app->notifications, &sequence_blink_red_100);
        notification_message(app->notifications, &sequence_double_vibro);
        return;
    }

    Habit* habit = &app->store->habits[index];
    bool was_last = habit_get(habit, habit_today()) + 1 == habit->target;
    if(scene_start_track(app, index, true)) {
        /* Completing the day already vibrates once; make every scan do so. */
        if(!was_last) notification_message(app->notifications, &sequence_single_vibro);
    } else {
        /* Already complete for today. */
        notification_message(app->notifications, &sequence_blink_blue_100);
        notification_message(app->notifications, &sequence_double_vibro);
    }
    habit_list_view_set_selected(app->habit_list, index);
    scene_manager_set_scene_state(app->scene_manager, HabitSceneStart, index);
}

static bool scene_start_on_event(void* context, SceneManagerEvent event) {
    HabitApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event >= HabitEventNfcTagBase) {
        scene_start_nfc_tag(app, event.event - HabitEventNfcTagBase);
        return true;
    }
    if(event.event == HabitEventNfcUnknownTag) {
        notification_message(app->notifications, &sequence_blink_red_100);
        return true;
    }
    if(event.event < HabitEventListBase) return false;

    uint32_t action = (event.event - HabitEventListBase) >> 8;
    uint8_t index = (event.event - HabitEventListBase) & 0xFF;
    scene_manager_set_scene_state(
        app->scene_manager, HabitSceneStart, habit_list_view_get_selected(app->habit_list));

    switch(action) {
    case HabitListActionTrack:
        scene_start_track(app, index, true);
        break;
    case HabitListActionUntrack:
        scene_start_track(app, index, false);
        break;
    case HabitListActionOpen:
        app->selected = index;
        scene_manager_next_scene(app->scene_manager, HabitSceneHabit);
        break;
    case HabitListActionAdd:
        app->adding = true;
        app->text_buf[0] = '\0';
        scene_manager_next_scene(app->scene_manager, HabitSceneName);
        break;
    case HabitListActionEdit:
        scene_manager_next_scene(app->scene_manager, HabitSceneEditList);
        break;
    default:
        break;
    }
    return true;
}

static void scene_start_on_exit(void* context) {
    HabitApp* app = context;
    habit_nfc_stop(app->nfc);
}

/* ---------------------------------------------------------------------------
 * Scene: Habit (year grid + tracking)
 * ------------------------------------------------------------------------- */

static void scene_habit_on_enter(void* context) {
    HabitApp* app = context;
    uint32_t today = habit_today();
    Habit* habit = &app->store->habits[app->selected];
    habit_sync(habit, today);
    habit_view_set_habit(app->habit_view, habit, today);
    view_dispatcher_switch_to_view(app->view_dispatcher, HabitViewIdHabit);
}

static bool scene_habit_on_event(void* context, SceneManagerEvent event) {
    HabitApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == HabitEventHabitChanged) {
        habit_app_save(app);
        return true;
    }
    return false;
}

static void scene_habit_on_exit(void* context) {
    HabitApp* app = context;
    habit_view_set_habit(app->habit_view, NULL, 0);
}

/* ---------------------------------------------------------------------------
 * Scene: Name (text input for add / rename)
 * ------------------------------------------------------------------------- */

static void habit_text_input_callback(void* context) {
    HabitApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HabitEventNameDone);
}

static void scene_name_on_enter(void* context) {
    HabitApp* app = context;
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, app->adding ? "New habit name" : "Rename habit");
    text_input_set_minimum_length(app->text_input, 1);
    text_input_set_result_callback(
        app->text_input,
        habit_text_input_callback,
        app,
        app->text_buf,
        sizeof(app->text_buf),
        app->adding);
    view_dispatcher_switch_to_view(app->view_dispatcher, HabitViewIdTextInput);
}

static bool scene_name_on_event(void* context, SceneManagerEvent event) {
    HabitApp* app = context;
    if(event.type != SceneManagerEventTypeCustom || event.event != HabitEventNameDone) {
        return false;
    }

    if(app->adding) {
        int index = habit_store_add(app->store, app->text_buf, 1, habit_today());
        if(index < 0) {
            app->adding = false;
            scene_manager_search_and_switch_to_previous_scene(app->scene_manager, HabitSceneStart);
            return true;
        }
        app->selected = index;
        scene_manager_set_scene_state(app->scene_manager, HabitSceneStart, index);
        habit_app_save(app);
        /* Continue to interval selection. */
        scene_manager_next_scene(app->scene_manager, HabitSceneConfig);
    } else {
        strlcpy(app->store->habits[app->selected].name, app->text_buf, HABIT_NAME_LEN);
        habit_app_save(app);
        scene_manager_previous_scene(app->scene_manager);
    }
    return true;
}

static void scene_name_on_exit(void* context) {
    HabitApp* app = context;
    text_input_reset(app->text_input);
}

/* ---------------------------------------------------------------------------
 * Scene: Config (interval / rename / delete)
 * ------------------------------------------------------------------------- */

static void habit_interval_changed(VariableItem* item) {
    HabitApp* app = variable_item_get_context(item);
    Habit* habit = &app->store->habits[app->selected];
    habit->target = variable_item_get_current_value_index(item) + 1;
    char text[16];
    habit_interval_text(habit->target, text, sizeof(text));
    variable_item_set_current_value_text(item, text);
    habit_app_save(app);
}

static void habit_config_enter_callback(void* context, uint32_t index) {
    HabitApp* app = context;
    if(index == ConfigItemWriteNfc) {
        view_dispatcher_send_custom_event(app->view_dispatcher, HabitEventWriteNfc);
    } else if(index == ConfigItemRename) {
        view_dispatcher_send_custom_event(app->view_dispatcher, HabitEventRename);
    } else if(index == ConfigItemDelete) {
        view_dispatcher_send_custom_event(app->view_dispatcher, HabitEventDelete);
    }
}

static void scene_config_on_enter(void* context) {
    HabitApp* app = context;
    Habit* habit = &app->store->habits[app->selected];
    VariableItemList* list = app->config_list;
    variable_item_list_reset(list);

    VariableItem* item = variable_item_list_add(
        list, "Interval", HABIT_TARGET_MAX, habit_interval_changed, app);
    variable_item_set_current_value_index(item, habit->target - 1);
    char text[16];
    habit_interval_text(habit->target, text, sizeof(text));
    variable_item_set_current_value_text(item, text);

    variable_item_list_add(list, "Write NFC Tag", 0, NULL, NULL);
    variable_item_list_add(list, "Rename", 0, NULL, NULL);
    variable_item_list_add(list, "Delete", 0, NULL, NULL);
    variable_item_list_set_enter_callback(list, habit_config_enter_callback, app);
    variable_item_list_set_selected_item(list, ConfigItemInterval);

    view_dispatcher_switch_to_view(app->view_dispatcher, HabitViewIdConfig);
}

static bool scene_config_on_event(void* context, SceneManagerEvent event) {
    HabitApp* app = context;

    if(event.type == SceneManagerEventTypeBack) {
        /* Return to whichever list opened us, skipping the add-flow name input. */
        static const uint32_t targets[] = {HabitSceneEditList, HabitSceneStart};
        app->adding = false;
        scene_manager_search_and_switch_to_previous_scene_one_of(
            app->scene_manager, targets, COUNT_OF(targets));
        return true;
    }

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == HabitEventWriteNfc) {
        scene_manager_next_scene(app->scene_manager, HabitSceneNfcWrite);
        return true;
    } else if(event.event == HabitEventRename) {
        app->adding = false;
        strlcpy(app->text_buf, app->store->habits[app->selected].name, sizeof(app->text_buf));
        scene_manager_next_scene(app->scene_manager, HabitSceneName);
        return true;
    } else if(event.event == HabitEventDelete) {
        scene_manager_next_scene(app->scene_manager, HabitSceneDelete);
        return true;
    }
    return false;
}

static void scene_config_on_exit(void* context) {
    HabitApp* app = context;
    variable_item_list_reset(app->config_list);
}

/* ---------------------------------------------------------------------------
 * Scene: Edit list (pick a habit to configure)
 * ------------------------------------------------------------------------- */

static void scene_edit_list_on_enter(void* context) {
    HabitApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Edit Habit");
    for(uint8_t i = 0; i < app->store->count; i++) {
        submenu_add_item(app->submenu, app->store->habits[i].name, i, habit_submenu_callback, app);
    }
    submenu_set_selected_item(
        app->submenu, scene_manager_get_scene_state(app->scene_manager, HabitSceneEditList));
    view_dispatcher_switch_to_view(app->view_dispatcher, HabitViewIdSubmenu);
}

static bool scene_edit_list_on_event(void* context, SceneManagerEvent event) {
    HabitApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event < app->store->count) {
        scene_manager_set_scene_state(app->scene_manager, HabitSceneEditList, event.event);
        app->selected = event.event;
        app->adding = false;
        scene_manager_next_scene(app->scene_manager, HabitSceneConfig);
        return true;
    }
    return false;
}

static void scene_edit_list_on_exit(void* context) {
    HabitApp* app = context;
    submenu_reset(app->submenu);
}

/* ---------------------------------------------------------------------------
 * Scene: Delete confirmation
 * ------------------------------------------------------------------------- */

static void habit_dialog_callback(DialogExResult result, void* context) {
    HabitApp* app = context;
    if(result == DialogExResultLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, HabitEventDialogLeft);
    } else if(result == DialogExResultRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, HabitEventDialogRight);
    }
}

static void scene_delete_on_enter(void* context) {
    HabitApp* app = context;
    DialogEx* dialog = app->dialog;
    dialog_ex_reset(dialog);
    dialog_ex_set_header(dialog, "Delete habit?", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog, app->store->habits[app->selected].name, 64, 30, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(dialog, "Cancel");
    dialog_ex_set_right_button_text(dialog, "Delete");
    dialog_ex_set_context(dialog, app);
    dialog_ex_set_result_callback(dialog, habit_dialog_callback);
    view_dispatcher_switch_to_view(app->view_dispatcher, HabitViewIdDialog);
}

static bool scene_delete_on_event(void* context, SceneManagerEvent event) {
    HabitApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == HabitEventDialogLeft) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    if(event.event == HabitEventDialogRight) {
        habit_store_remove(app->store, app->selected);
        habit_app_save(app);
        app->adding = false;
        app->selected = 0;
        scene_manager_set_scene_state(app->scene_manager, HabitSceneStart, 0);
        scene_manager_set_scene_state(app->scene_manager, HabitSceneEditList, 0);

        if(app->store->count > 0 &&
           scene_manager_has_previous_scene(app->scene_manager, HabitSceneEditList)) {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, HabitSceneEditList);
        } else {
            scene_manager_search_and_switch_to_previous_scene(app->scene_manager, HabitSceneStart);
        }
        return true;
    }
    return false;
}

static void scene_delete_on_exit(void* context) {
    HabitApp* app = context;
    dialog_ex_reset(app->dialog);
}

/* ---------------------------------------------------------------------------
 * Scene: NFC write (put a habit on a tag)
 * ------------------------------------------------------------------------- */

static void habit_popup_callback(void* context) {
    HabitApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HabitEventPopupDone);
}

static void scene_nfc_write_show(HabitApp* app, const char* header, const char* text) {
    strlcpy(app->popup_text, text, sizeof(app->popup_text));
    popup_set_header(app->popup, header, 64, 4, AlignCenter, AlignTop);
    popup_set_text(app->popup, app->popup_text, 64, 38, AlignCenter, AlignCenter);
}

static void scene_nfc_write_on_enter(void* context) {
    HabitApp* app = context;
    Habit* habit = &app->store->habits[app->selected];

    popup_reset(app->popup);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, habit_popup_callback);
    snprintf(
        app->popup_text,
        sizeof(app->popup_text),
        "%s\nHold a tag or card to the\nback of the Flipper",
        habit->name);
    popup_set_header(app->popup, "Write NFC Tag", 64, 4, AlignCenter, AlignTop);
    popup_set_text(app->popup, app->popup_text, 64, 38, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, HabitViewIdPopup);

    notification_message(app->notifications, &sequence_blink_start_cyan);
    habit_nfc_start_write(app->nfc, habit->id, habit->name);
}

static bool scene_nfc_write_on_event(void* context, SceneManagerEvent event) {
    HabitApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == HabitEventNfcWriteOk) {
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_success);
        scene_nfc_write_show(app, "Tag Written", "Scan it on the habit\nlist to log this habit");
        popup_set_timeout(app->popup, 2000);
        popup_enable_timeout(app->popup);
        return true;
    }
    if(event.event == HabitEventNfcWriteFail) {
        notification_message(app->notifications, &sequence_error);
        scene_nfc_write_show(
            app, "Write Failed", "Tag is locked or moved.\nTake it away and retry");
        return true;
    }
    if(event.event == HabitEventNfcUnsupportedTag) {
        notification_message(app->notifications, &sequence_error);
        scene_nfc_write_show(
            app, "Unsupported Tag", "Use NTAG, Ultralight,\nMIFARE Classic or ICODE");
        return true;
    }
    if(event.event == HabitEventPopupDone) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

static void scene_nfc_write_on_exit(void* context) {
    HabitApp* app = context;
    habit_nfc_stop(app->nfc);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
}

/* ---------------------------------------------------------------------------
 * Scene table
 * ------------------------------------------------------------------------- */

static void (*const habit_on_enter_handlers[])(void*) = {
    scene_start_on_enter,
    scene_habit_on_enter,
    scene_name_on_enter,
    scene_config_on_enter,
    scene_edit_list_on_enter,
    scene_delete_on_enter,
    scene_nfc_write_on_enter,
};

static bool (*const habit_on_event_handlers[])(void*, SceneManagerEvent) = {
    scene_start_on_event,
    scene_habit_on_event,
    scene_name_on_event,
    scene_config_on_event,
    scene_edit_list_on_event,
    scene_delete_on_event,
    scene_nfc_write_on_event,
};

static void (*const habit_on_exit_handlers[])(void*) = {
    scene_start_on_exit,
    scene_habit_on_exit,
    scene_name_on_exit,
    scene_config_on_exit,
    scene_edit_list_on_exit,
    scene_delete_on_exit,
    scene_nfc_write_on_exit,
};

static const SceneManagerHandlers habit_scene_handlers = {
    .on_enter_handlers = habit_on_enter_handlers,
    .on_event_handlers = habit_on_event_handlers,
    .on_exit_handlers = habit_on_exit_handlers,
    .scene_num = HabitSceneCount,
};

/* ---------------------------------------------------------------------------
 * App lifecycle
 * ------------------------------------------------------------------------- */

static void habit_view_changed_callback(void* context) {
    HabitApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HabitEventHabitChanged);
}

static bool habit_custom_event_callback(void* context, uint32_t event) {
    HabitApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool habit_back_event_callback(void* context) {
    HabitApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static HabitApp* habit_app_alloc(void) {
    HabitApp* app = malloc(sizeof(HabitApp));
    memset(app, 0, sizeof(HabitApp));

    app->store = habit_store_alloc();
    habit_store_load(app->store);
    /* Habits saved by older versions have no id yet: assign and persist them. */
    if(habit_store_ensure_ids(app->store)) habit_store_save(app->store);
    habit_store_sync(app->store, habit_today());

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&habit_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, habit_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, habit_back_event_callback);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HabitViewIdSubmenu, submenu_get_view(app->submenu));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HabitViewIdTextInput, text_input_get_view(app->text_input));

    app->config_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HabitViewIdConfig, variable_item_list_get_view(app->config_list));

    app->dialog = dialog_ex_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HabitViewIdDialog, dialog_ex_get_view(app->dialog));

    app->habit_view = habit_view_alloc();
    habit_view_set_changed_callback(app->habit_view, habit_view_changed_callback, app);

    app->habit_list = habit_list_view_alloc();
    habit_list_view_set_callback(app->habit_list, habit_list_callback, app);
    view_dispatcher_add_view(
        app->view_dispatcher, HabitViewIdList, habit_list_view_get_view(app->habit_list));

    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, HabitViewIdPopup, popup_get_view(app->popup));

    app->nfc = habit_nfc_alloc();
    habit_nfc_set_callback(app->nfc, habit_nfc_callback, app);

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    view_dispatcher_add_view(
        app->view_dispatcher, HabitViewIdHabit, habit_view_get_view(app->habit_view));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    return app;
}

static void habit_app_free(HabitApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, HabitViewIdSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, HabitViewIdTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, HabitViewIdConfig);
    view_dispatcher_remove_view(app->view_dispatcher, HabitViewIdDialog);
    view_dispatcher_remove_view(app->view_dispatcher, HabitViewIdHabit);
    view_dispatcher_remove_view(app->view_dispatcher, HabitViewIdList);
    view_dispatcher_remove_view(app->view_dispatcher, HabitViewIdPopup);

    submenu_free(app->submenu);
    text_input_free(app->text_input);
    variable_item_list_free(app->config_list);
    dialog_ex_free(app->dialog);
    habit_view_free(app->habit_view);
    habit_list_view_free(app->habit_list);
    popup_free(app->popup);
    habit_nfc_free(app->nfc);
    furi_record_close(RECORD_NOTIFICATION);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    habit_store_free(app->store);
    free(app);
}

int32_t flipper_habit_app(void* p) {
    UNUSED(p);
    HabitApp* app = habit_app_alloc();
    scene_manager_next_scene(app->scene_manager, HabitSceneStart);
    view_dispatcher_run(app->view_dispatcher);
    habit_app_free(app);
    return 0;
}
