#include "habit_list_view.h"

#include <furi.h>
#include <gui/elements.h>

/*
 * Layout: "Habits" header, then 4 visible rows of 13 px.
 * Habit rows: [checkbox] name ........ 2/3 >
 *   checkbox empty = not started, dot = partly done, filled = done today.
 */
#define LIST_HEADER_H     12
#define LIST_ROW_H        13
#define LIST_VISIBLE_ROWS 4
#define LIST_WIDTH        123 /* leaves room for the scrollbar */

struct HabitListView {
    View* view;
    HabitListCallback callback;
    void* context;
};

typedef struct {
    HabitStore* store;
    uint32_t today;
    uint8_t selected;
    uint8_t offset;
} HabitListModel;

typedef enum {
    RowHabit,
    RowAdd,
    RowEdit,
} RowKind;

static uint8_t list_row_count(const HabitListModel* model) {
    if(!model->store) return 0;
    uint8_t count = model->store->count;
    uint8_t rows = count;
    if(count < HABIT_MAX) rows++; /* + Add Habit */
    if(count > 0) rows++; /* Edit Habits */
    return rows;
}

static RowKind list_row_kind(const HabitListModel* model, uint8_t row) {
    uint8_t count = model->store->count;
    if(row < count) return RowHabit;
    if(row == count && count < HABIT_MAX) return RowAdd;
    return RowEdit;
}

static void list_clamp(HabitListModel* model) {
    uint8_t rows = list_row_count(model);
    if(rows == 0) {
        model->selected = 0;
        model->offset = 0;
        return;
    }
    if(model->selected >= rows) model->selected = rows - 1;
    if(model->selected < model->offset) model->offset = model->selected;
    if(model->selected >= model->offset + LIST_VISIBLE_ROWS) {
        model->offset = model->selected - LIST_VISIBLE_ROWS + 1;
    }
    if(rows <= LIST_VISIBLE_ROWS) {
        model->offset = 0;
    } else if(model->offset > rows - LIST_VISIBLE_ROWS) {
        model->offset = rows - LIST_VISIBLE_ROWS;
    }
}

static void list_draw_habit_row(
    Canvas* canvas,
    const HabitListModel* model,
    const Habit* habit,
    int32_t y,
    bool selected) {
    uint8_t done = habit_get(habit, model->today);

    /* Checkbox */
    if(done >= habit->target) {
        canvas_draw_box(canvas, 3, y + 3, 7, 7);
    } else {
        canvas_draw_frame(canvas, 3, y + 3, 7, 7);
        if(done > 0) canvas_draw_box(canvas, 5, y + 5, 3, 3);
    }

    /* Count and open hint on the right */
    char count[8];
    snprintf(count, sizeof(count), "%u/%u", done, habit->target);
    int32_t right = LIST_WIDTH - 9;
    canvas_draw_str_aligned(canvas, right, y + 10, AlignRight, AlignBottom, count);
    if(selected) canvas_draw_str(canvas, LIST_WIDTH - 6, y + 10, ">");

    /* Name, truncated to the space left */
    FuriString* name = furi_string_alloc_set(habit->name);
    size_t width = right - canvas_string_width(canvas, count) - 4 - 13;
    elements_string_fit_width(canvas, name, width);
    canvas_draw_str(canvas, 13, y + 10, furi_string_get_cstr(name));
    furi_string_free(name);
}

static void habit_list_draw(Canvas* canvas, void* model_ptr) {
    HabitListModel* model = model_ptr;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Habits");
    /* The app scans for habit tags while this list is shown. */
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, "or scan NFC tag");

    if(!model->store) return;

    canvas_set_font(canvas, FontSecondary);
    uint8_t rows = list_row_count(model);
    for(uint8_t i = 0; i < LIST_VISIBLE_ROWS; i++) {
        uint8_t row = model->offset + i;
        if(row >= rows) break;
        int32_t y = LIST_HEADER_H + i * LIST_ROW_H;
        bool selected = row == model->selected;

        if(selected) {
            canvas_set_color(canvas, ColorBlack);
            elements_slightly_rounded_box(canvas, 0, y, LIST_WIDTH, LIST_ROW_H);
            canvas_set_color(canvas, ColorWhite);
        }

        switch(list_row_kind(model, row)) {
        case RowHabit:
            list_draw_habit_row(canvas, model, &model->store->habits[row], y, selected);
            break;
        case RowAdd:
            canvas_draw_str(canvas, 4, y + 10, "+ Add Habit");
            break;
        case RowEdit:
            canvas_draw_str(canvas, 4, y + 10, "Edit Habits");
            break;
        }
        canvas_set_color(canvas, ColorBlack);
    }

    if(rows > LIST_VISIBLE_ROWS) {
        elements_scrollbar(canvas, model->selected, rows);
    }
}

static bool habit_list_input(InputEvent* event, void* context) {
    HabitListView* list = context;
    if(event->key == InputKeyBack) return false;

    bool consumed = false;
    bool fire = false;
    HabitListAction action = HabitListActionTrack;
    uint8_t index = 0;

    with_view_model(
        list->view,
        HabitListModel * model,
        {
            uint8_t rows = list_row_count(model);
            bool press = event->type == InputTypeShort || event->type == InputTypeRepeat;
            if(rows == 0) {
                /* nothing to do */
            } else if(press && event->key == InputKeyUp) {
                model->selected = model->selected == 0 ? rows - 1 : model->selected - 1;
                list_clamp(model);
                consumed = true;
            } else if(press && event->key == InputKeyDown) {
                model->selected = model->selected + 1 >= rows ? 0 : model->selected + 1;
                list_clamp(model);
                consumed = true;
            } else {
                RowKind kind = list_row_kind(model, model->selected);
                index = kind == RowHabit ? model->selected : 0;
                if(event->key == InputKeyOk && event->type == InputTypeShort) {
                    fire = true;
                    action = kind == RowHabit ? HabitListActionTrack :
                             kind == RowAdd   ? HabitListActionAdd :
                                                HabitListActionEdit;
                    consumed = true;
                } else if(
                    event->key == InputKeyOk && event->type == InputTypeLong &&
                    kind == RowHabit) {
                    fire = true;
                    action = HabitListActionUntrack;
                    consumed = true;
                } else if(
                    event->key == InputKeyRight && event->type == InputTypeShort &&
                    kind == RowHabit) {
                    fire = true;
                    action = HabitListActionOpen;
                    consumed = true;
                }
            }
        },
        consumed);

    /* Outside the model lock: the handler may redraw the list or switch scenes. */
    if(fire && list->callback) list->callback(list->context, action, index);
    return consumed;
}

HabitListView* habit_list_view_alloc(void) {
    HabitListView* list = malloc(sizeof(HabitListView));
    list->callback = NULL;
    list->context = NULL;
    list->view = view_alloc();
    view_allocate_model(list->view, ViewModelTypeLocking, sizeof(HabitListModel));
    view_set_context(list->view, list);
    view_set_draw_callback(list->view, habit_list_draw);
    view_set_input_callback(list->view, habit_list_input);
    return list;
}

void habit_list_view_free(HabitListView* list) {
    view_free(list->view);
    free(list);
}

View* habit_list_view_get_view(HabitListView* list) {
    return list->view;
}

void habit_list_view_set_callback(HabitListView* list, HabitListCallback callback, void* context) {
    list->callback = callback;
    list->context = context;
}

void habit_list_view_set_store(HabitListView* list, HabitStore* store, uint32_t today) {
    with_view_model(
        list->view,
        HabitListModel * model,
        {
            model->store = store;
            model->today = today;
            list_clamp(model);
        },
        true);
}

void habit_list_view_set_selected(HabitListView* list, uint8_t row) {
    with_view_model(
        list->view,
        HabitListModel * model,
        {
            model->selected = row;
            list_clamp(model);
        },
        true);
}

uint8_t habit_list_view_get_selected(HabitListView* list) {
    uint8_t selected = 0;
    with_view_model(
        list->view, HabitListModel * model, { selected = model->selected; }, false);
    return selected;
}
