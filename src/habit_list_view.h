#pragma once

#include <gui/view.h>
#include "habit_data.h"

typedef struct HabitListView HabitListView;

typedef enum {
    HabitListActionTrack, /* OK on a habit: log one completion today */
    HabitListActionUntrack, /* Hold OK on a habit: remove one completion today */
    HabitListActionOpen, /* Right on a habit: open its tracker page */
    HabitListActionAdd, /* OK on "+ Add Habit" */
    HabitListActionEdit, /* OK on "Edit Habits" */
} HabitListAction;

/* `index` is the habit index for habit actions, 0 otherwise. */
typedef void (*HabitListCallback)(void* context, HabitListAction action, uint8_t index);

HabitListView* habit_list_view_alloc(void);
void habit_list_view_free(HabitListView* list);
View* habit_list_view_get_view(HabitListView* list);
void habit_list_view_set_callback(HabitListView* list, HabitListCallback callback, void* context);
/* Point the list at the store and redraw. Call again after the store changes. */
void habit_list_view_set_store(HabitListView* list, HabitStore* store, uint32_t today);
void habit_list_view_set_selected(HabitListView* list, uint8_t row);
uint8_t habit_list_view_get_selected(HabitListView* list);
