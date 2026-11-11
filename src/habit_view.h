#pragma once

#include <gui/view.h>
#include "habit_data.h"

typedef struct HabitView HabitView;

HabitView* habit_view_alloc(void);
void habit_view_free(HabitView* habit_view);
View* habit_view_get_view(HabitView* habit_view);
typedef void (*HabitViewChangedCallback)(void* context);

/* Show `habit` with the cursor placed on `today`. */
void habit_view_set_habit(HabitView* habit_view, Habit* habit, uint32_t today);
/* Called whenever the user logs or removes a completion. */
void habit_view_set_changed_callback(
    HabitView* habit_view,
    HabitViewChangedCallback callback,
    void* context);
