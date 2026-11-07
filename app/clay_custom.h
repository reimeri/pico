#ifndef PICO_CLAY_CUSTOM_H
#define PICO_CLAY_CUSTOM_H

/* Private renderer payload, not part of the extension API. */
#include "raylib.h"
#include "clay/clay.h"

typedef enum
{
    CUSTOM_LAYOUT_ELEMENT_TYPE_3D_MODEL,
    CUSTOM_LAYOUT_ELEMENT_TYPE_CALLBACK
} CustomLayoutElementType;

typedef struct
{
    Model model;
    float scale;
    Vector3 position;
    Matrix rotation;
} CustomLayoutElement_3DModel;

typedef struct
{
    CustomLayoutElementType type;
    union {
        CustomLayoutElement_3DModel model;
        void (*draw)(Clay_BoundingBox bounds);
    } customData;
} CustomLayoutElement;

#endif
