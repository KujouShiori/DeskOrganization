#pragma once

#include <d2d1.h>
#include <dwrite.h>

bool RenderInit();
void RenderShutdown();

ID2D1Factory* TheD2D();
IDWriteFactory* TheDWrite();
