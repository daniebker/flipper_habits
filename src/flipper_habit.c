#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/dialog_ex.h>

#include "habit_data.h"
#include "habit_view.h"

/* ---------------------------------------------------------------------------
 * App state
 * ------------------------------------------------------------------------- */

typedef enum {
    HabitViewIdSubmenu,
    HabitViewIdTextInput,
    HabitViewIdConfig,
    HabitViewIdDialog,
    HabitViewIdHabit,
} HabitViewId;

typedef enum {
    HabitSceneStart,
    HabitSceneHabit,
    HabitSceneName,
    HabitSceneConfig,
    HabitSceneEditList,
    HabitSceneDelete,
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
} HabitEvent;

typedef enum {
    ConfigItemInterval,
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

    HabitStore* store;
    uint8_t selected;
    /* True while walking through the "add habit" flow (name -> interval). */
    bool adding;
    char text_buf[HABIT_NAME_LEN];
} HabitApp;

static void habit_app_save(HabitApp* app) {
    habit_store_save(app->store);
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

static void scene_start_on_enter(void* context) {
    HabitApp* app = context;
    uint32_t today = habit_today();
    habit_store_sync(app->store, today);

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Habits");

    FuriString* label = furi_string_alloc();
    for(uint8_t i = 0; i < app->store->count; i++) {
        Habit* habit = &app->store->habits[i];
        uint8_t done = habit_get(habit, today);
        furi_string_printf(
            label,
            "%s %s %u/%u",
            done >= habit->target ? "[x]" : "[ ]",
            habit->name,
            done,
            habit->target);
        submenu_add_item(app->submenu, furi_string_get_cstr(label), i, habit_submenu_callback, app);
    }
    furi_string_free(label);

    if(app->store->count < HABIT_MAX) {
        submenu_add_item(app->submenu, "+ Add Habit", HabitEventAdd, habit_submenu_callback, app);
    }
    if(app->store->count > 0) {
        submenu_add_item(app->submenu, "Edit Habits", HabitEventEdit, habit_submenu_callback, app);
    }

    submenu_set_selected_item(
        app->submenu, scene_manager_get_scene_state(app->scene_manager, HabitSceneStart));
    view_dispatcher_switch_to_view(app->view_dispatcher, HabitViewIdSubmenu);
}

static bool scene_start_on_event(void* context, SceneManagerEvent event) {
    HabitApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    scene_manager_set_scene_state(app->scene_manager, HabitSceneStart, event.event);
    if(event.event < HABIT_MAX) {
        app->selected = event.event;
        scene_manager_next_scene(app->scene_manager, HabitSceneHabit);
    } else if(event.event == HabitEventAdd) {
        app->adding = true;
        app->text_buf[0] = '\0';
        scene_manager_next_scene(app->scene_manager, HabitSceneName);
    } else if(event.event == HabitEventEdit) {
        scene_manager_next_scene(app->scene_manager, HabitSceneEditList);
    }
    return true;
}

static void scene_start_on_exit(void* context) {
    HabitApp* app = context;
    submenu_reset(app->submenu);
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
    UNUSED(context);
    UNUSED(event);
    return false;
}

static void scene_habit_on_exit(void* context) {
    HabitApp* app = context;
    habit_view_set_habit(app->habit_view, NULL, 0);
    habit_app_save(app);
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
}

static void habit_config_enter_callback(void* context, uint32_t index) {
    HabitApp* app = context;
    if(index == ConfigItemRename) {
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

    if(event.event == HabitEventRename) {
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
    habit_app_save(app);
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
 * Scene table
 * ------------------------------------------------------------------------- */

static void (*const habit_on_enter_handlers[])(void*) = {
    scene_start_on_enter,
    scene_habit_on_enter,
    scene_name_on_enter,
    scene_config_on_enter,
    scene_edit_list_on_enter,
    scene_delete_on_enter,
};

static bool (*const habit_on_event_handlers[])(void*, SceneManagerEvent) = {
    scene_start_on_event,
    scene_habit_on_event,
    scene_name_on_event,
    scene_config_on_event,
    scene_edit_list_on_event,
    scene_delete_on_event,
};

static void (*const habit_on_exit_handlers[])(void*) = {
    scene_start_on_exit,
    scene_habit_on_exit,
    scene_name_on_exit,
    scene_config_on_exit,
    scene_edit_list_on_exit,
    scene_delete_on_exit,
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

    submenu_free(app->submenu);
    text_input_free(app->text_input);
    variable_item_list_free(app->config_list);
    dialog_ex_free(app->dialog);
    habit_view_free(app->habit_view);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    habit_store_save(app->store);
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
