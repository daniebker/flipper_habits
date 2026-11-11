#include "habit_view.h"

#include <furi.h>
#include <gui/elements.h>

/*
 * Year grid layout (128x64 screen):
 *   y 0..7   header: label on the left, "<count>/<target> <year total>d" on the right
 *   y 9..35  weeks 1..27   (columns are weeks, rows are Sun..Sat)
 *   y 37..63 weeks 28..53  (current week is the last column)
 * Cells are 3x3 px on a 4 px pitch:
 *   empty    -> single centre dot
 *   partial  -> hollow square (multi-per-day habits)
 *   complete -> filled square
 */
#define GRID_WEEKS     53
#define GRID_TOP_WEEKS 27
#define GRID_PITCH     4
#define GRID_CELL      3
#define GRID_X         10
#define GRID_TOP_Y     9
#define GRID_BOTTOM_Y  37

struct HabitView {
    View* view;
};

typedef struct {
    Habit* habit;
    uint32_t today;
    uint32_t cursor;
} HabitViewModel;

static const char* const month_names[] =
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char* const weekday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static uint32_t grid_first_day(uint32_t today) {
    uint32_t back = habit_weekday(today) + (GRID_WEEKS - 1) * 7;
    return today > back ? today - back : 0;
}

static void grid_cell_pos(uint32_t first_day, uint32_t day, int32_t* x, int32_t* y) {
    uint32_t offset = day - first_day;
    uint32_t week = offset / 7;
    uint32_t row = offset % 7;
    if(week < GRID_TOP_WEEKS) {
        *x = GRID_X + week * GRID_PITCH;
        *y = GRID_TOP_Y + row * GRID_PITCH;
    } else {
        *x = GRID_X + (week - GRID_TOP_WEEKS) * GRID_PITCH;
        *y = GRID_BOTTOM_Y + row * GRID_PITCH;
    }
}

static void habit_view_draw(Canvas* canvas, void* model_ptr) {
    HabitViewModel* model = model_ptr;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    Habit* habit = model->habit;
    if(!habit) {
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "No habit selected");
        return;
    }

    /* Header */
    char right[24];
    snprintf(
        right,
        sizeof(right),
        "%u/%u  %ud",
        habit_get(habit, model->cursor),
        habit->target,
        habit_year_total(habit, model->today));
    canvas_draw_str_aligned(canvas, 128, 0, AlignRight, AlignTop, right);

    FuriString* label = furi_string_alloc();
    if(model->cursor == model->today) {
        furi_string_set(label, habit->name);
    } else {
        int32_t year;
        uint32_t month, day;
        habit_civil_from_days(model->cursor, &year, &month, &day);
        furi_string_printf(
            label,
            "%s %s %lu",
            weekday_names[habit_weekday(model->cursor)],
            month_names[month - 1],
            day);
    }
    elements_string_fit_width(canvas, label, 128 - canvas_string_width(canvas, right) - 4);
    canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, furi_string_get_cstr(label));
    furi_string_free(label);

    /* Grid */
    uint32_t first = grid_first_day(model->today);
    for(uint32_t day = first; day <= model->today; day++) {
        int32_t x, y;
        grid_cell_pos(first, day, &x, &y);
        uint8_t count = habit_get(habit, day);
        if(count == 0) {
            canvas_draw_dot(canvas, x + 1, y + 1);
        } else if(count >= habit->target) {
            canvas_draw_box(canvas, x, y, GRID_CELL, GRID_CELL);
        } else {
            canvas_draw_frame(canvas, x, y, GRID_CELL, GRID_CELL);
        }
    }

    /* Cursor: invert a 5x5 block around the selected cell. */
    int32_t cx, cy;
    grid_cell_pos(first, model->cursor, &cx, &cy);
    canvas_set_color(canvas, ColorXOR);
    canvas_draw_box(canvas, cx - 1, cy - 1, GRID_CELL + 2, GRID_CELL + 2);
    canvas_set_color(canvas, ColorBlack);
}

static void habit_view_move_cursor(HabitViewModel* model, int32_t delta) {
    uint32_t first = grid_first_day(model->today);
    int64_t next = (int64_t)model->cursor + delta;
    if(next < (int64_t)first) next = first;
    if(next > (int64_t)model->today) next = model->today;
    model->cursor = (uint32_t)next;
}

static bool habit_view_input(InputEvent* event, void* context) {
    HabitView* habit_view = context;
    bool consumed = false;

    if(event->key == InputKeyBack) return false;

    with_view_model(
        habit_view->view,
        HabitViewModel * model,
        {
            Habit* habit = model->habit;
            if(habit) {
                bool press = event->type == InputTypeShort || event->type == InputTypeRepeat;
                if(press && event->key == InputKeyUp) {
                    habit_view_move_cursor(model, -1);
                    consumed = true;
                } else if(press && event->key == InputKeyDown) {
                    habit_view_move_cursor(model, 1);
                    consumed = true;
                } else if(press && event->key == InputKeyLeft) {
                    habit_view_move_cursor(model, -7);
                    consumed = true;
                } else if(press && event->key == InputKeyRight) {
                    habit_view_move_cursor(model, 7);
                    consumed = true;
                } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
                    uint8_t count = habit_get(habit, model->cursor);
                    if(count < habit->target) habit_set(habit, model->cursor, count + 1);
                    consumed = true;
                } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
                    uint8_t count = habit_get(habit, model->cursor);
                    if(count > 0) habit_set(habit, model->cursor, count - 1);
                    consumed = true;
                }
            }
        },
        consumed);

    return consumed;
}

HabitView* habit_view_alloc(void) {
    HabitView* habit_view = malloc(sizeof(HabitView));
    habit_view->view = view_alloc();
    view_allocate_model(habit_view->view, ViewModelTypeLocking, sizeof(HabitViewModel));
    view_set_context(habit_view->view, habit_view);
    view_set_draw_callback(habit_view->view, habit_view_draw);
    view_set_input_callback(habit_view->view, habit_view_input);
    return habit_view;
}

void habit_view_free(HabitView* habit_view) {
    view_free(habit_view->view);
    free(habit_view);
}

View* habit_view_get_view(HabitView* habit_view) {
    return habit_view->view;
}

void habit_view_set_habit(HabitView* habit_view, Habit* habit, uint32_t today) {
    with_view_model(
        habit_view->view,
        HabitViewModel * model,
        {
            model->habit = habit;
            model->today = today;
            model->cursor = today;
        },
        true);
}
