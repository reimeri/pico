#include "scrollbar.h"

#include "pico/app.h"
#include "pico/theme.h"

#include "clay/clay.h"
#include "raylib.h"

#define PICO_SCROLLBAR_MIN_THUMB 16.0f
#define PICO_SCROLLBAR_OVERLAY_Z 80
#define PICO_SCROLLBAR_OVERLAY_INSET_X 4.0f
#define PICO_SCROLLBAR_OVERLAY_INSET_Y 10.0f

static bool s_dragging;

void PicoScrollbar_BeginFrame(void)
{
    s_dragging = false;
}

bool PicoScrollbar_AnyDragging(void)
{
    return s_dragging;
}

bool PicoScrollbar_Overflows(Clay_String container_id)
{
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(container_id));
    return data.found && data.contentDimensions.height > data.scrollContainerDimensions.height + 0.5f;
}

PicoScrollbarThumb PicoScrollbar_Metrics(float track_h, float content_h, float scroll_y)
{
    PicoScrollbarThumb thumb = {0};
    if (track_h <= 0.0f)
    {
        return thumb;
    }
    if (content_h <= track_h)
    {
        thumb.height = track_h;
        thumb.y = 0.0f;
        return thumb;
    }
    float thumb_h = (track_h / content_h) * track_h;
    if (thumb_h < PICO_SCROLLBAR_MIN_THUMB)
    {
        thumb_h = PICO_SCROLLBAR_MIN_THUMB;
    }
    if (thumb_h > track_h)
    {
        thumb_h = track_h;
    }
    float max_scroll = content_h - track_h;
    float travel = track_h - thumb_h;
    float thumb_y = 0.0f;
    if (max_scroll > 0.0f && travel > 0.0f)
    {
        thumb_y = (-scroll_y / max_scroll) * travel;
        if (thumb_y < 0.0f)
        {
            thumb_y = 0.0f;
        }
        if (thumb_y > travel)
        {
            thumb_y = travel;
        }
    }
    thumb.height = thumb_h;
    thumb.y = thumb_y;
    return thumb;
}

static float OverlayTrackHeight(float viewport_h)
{
    float track_h = viewport_h - 2.0f * PICO_SCROLLBAR_OVERLAY_INSET_Y;
    return track_h > PICO_SCROLLBAR_MIN_THUMB ? track_h : viewport_h;
}

static PicoScrollbarThumb ThumbFor(Clay_String container_id)
{
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(container_id));
    float track_h = data.found ? data.scrollContainerDimensions.height : 0.0f;
    float content_h = data.found ? data.contentDimensions.height : 1.0f;
    float scroll_y = (data.found && data.scrollPosition) ? data.scrollPosition->y : 0.0f;
    return PicoScrollbar_Metrics(track_h, content_h, scroll_y);
}

static PicoScrollbarThumb OverlayThumb(Clay_ScrollContainerData data)
{
    float viewport_h = data.found ? data.scrollContainerDimensions.height : 0.0f;
    float content_h = data.found ? data.contentDimensions.height : 1.0f;
    float scroll_y = (data.found && data.scrollPosition) ? data.scrollPosition->y : 0.0f;
    float track_h = OverlayTrackHeight(viewport_h);
    PicoScrollbarThumb thumb = {0};
    if (track_h <= 0.0f)
    {
        return thumb;
    }
    if (content_h <= viewport_h || viewport_h <= 0.0f)
    {
        thumb.height = track_h;
        return thumb;
    }
    float thumb_h = (viewport_h / content_h) * track_h;
    if (thumb_h < PICO_SCROLLBAR_MIN_THUMB)
    {
        thumb_h = PICO_SCROLLBAR_MIN_THUMB;
    }
    if (thumb_h > track_h)
    {
        thumb_h = track_h;
    }
    float max_scroll = content_h - viewport_h;
    float travel = track_h - thumb_h;
    float thumb_y = 0.0f;
    if (max_scroll > 0.0f && travel > 0.0f)
    {
        thumb_y = (-scroll_y / max_scroll) * travel;
        if (thumb_y < 0.0f)
        {
            thumb_y = 0.0f;
        }
        if (thumb_y > travel)
        {
            thumb_y = travel;
        }
    }
    thumb.height = thumb_h;
    thumb.y = thumb_y;
    return thumb;
}

static void RenderThumb(Clay_String handle_id, PicoScrollbarThumb thumb)
{
    Clay_ElementId handle = Clay_GetElementId(handle_id);
    if (thumb.y > 0.5f)
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                            .height = CLAY_SIZING_FIXED(thumb.y)}}})
        {
        }
    }
    CLAY(CLAY_SID(handle_id),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                .height = CLAY_SIZING_FIXED(thumb.height)}},
          .backgroundColor = Clay_PointerOver(handle) ? COLOR_SCROLLBAR_HOVER : COLOR_SCROLLBAR,
          .cornerRadius = CLAY_CORNER_RADIUS((float)SCROLLBAR_WIDTH / 2.0f)})
    {
    }
}

void PicoScrollbar_Render(Clay_String container_id, Clay_String track_id, Clay_String handle_id)
{
    PicoScrollbarThumb thumb = ThumbFor(container_id);
    CLAY(CLAY_SID(track_id),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                .height = CLAY_SIZING_GROW(0)}}})
    {
        RenderThumb(handle_id, thumb);
    }
}

void PicoScrollbar_RenderOverlay(Clay_String container_id, Clay_String track_id, Clay_String handle_id)
{
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(container_id));
    float viewport_h = data.found ? data.scrollContainerDimensions.height : 0.0f;
    float track_h = OverlayTrackHeight(viewport_h);
    PicoScrollbarThumb thumb = OverlayThumb(data);
    Clay_ElementId parent = Clay_GetElementId(container_id);
    CLAY(CLAY_SID(track_id),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                       .parentId = parent.id,
                       .zIndex = PICO_SCROLLBAR_OVERLAY_Z,
                       .offset = {.x = -PICO_SCROLLBAR_OVERLAY_INSET_X, .y = PICO_SCROLLBAR_OVERLAY_INSET_Y},
                       .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_TOP,
                                        .parent = CLAY_ATTACH_POINT_RIGHT_TOP},
                       .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                .height = CLAY_SIZING_FIXED(track_h)}}})
    {
        RenderThumb(handle_id, thumb);
    }
}

static void UpdateDrag(PicoScrollbar *drag, Clay_String container_id, Clay_String handle_id, float y_inset)
{
    if (!drag)
    {
        return;
    }
    Clay_Vector2 mouse = {.x = GetMousePosition().x, .y = GetMousePosition().y};
    if (!IsMouseButtonDown(0))
    {
        drag->mouse_down = false;
    }
    Clay_ElementId handle = Clay_GetElementId(handle_id);
    Clay_ElementId container = Clay_GetElementId(container_id);
    if (IsMouseButtonDown(0) && !drag->mouse_down && Clay_PointerOver(handle))
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(container);
        if (data.found && data.scrollPosition)
        {
            drag->click_origin = mouse;
            drag->position_origin = *data.scrollPosition;
            drag->mouse_down = true;
        }
    }
    else if (drag->mouse_down)
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(container);
        if (data.found && data.scrollPosition)
        {
            float viewport_h = data.scrollContainerDimensions.height;
            float content_h = data.contentDimensions.height;
            float track_h = y_inset > 0.0f ? OverlayTrackHeight(viewport_h) : viewport_h;
            PicoScrollbarThumb thumb = y_inset > 0.0f ? OverlayThumb(data)
                                                      : PicoScrollbar_Metrics(viewport_h, content_h, 0.0f);
            float travel = track_h - thumb.height;
            float max_scroll = content_h - viewport_h;
            if (travel > 0.0f && max_scroll > 0.0f)
            {
                data.scrollPosition->y =
                    drag->position_origin.y - (mouse.y - drag->click_origin.y) * (max_scroll / travel);
            }
        }
    }
    if (drag->mouse_down)
    {
        s_dragging = true;
    }
}

void PicoScrollbar_UpdateDrag(PicoScrollbar *drag, Clay_String container_id, Clay_String handle_id)
{
    UpdateDrag(drag, container_id, handle_id, 0.0f);
}

void PicoScrollbar_UpdateDragOverlay(PicoScrollbar *drag, Clay_String container_id, Clay_String handle_id)
{
    UpdateDrag(drag, container_id, handle_id, PICO_SCROLLBAR_OVERLAY_INSET_Y);
}
