/* ============================================================================
 *  BoardLvgl - display MIPI-DSI, touch GT911 e task LVGL
 *  Basato sullo sketch gia' validato su questa scheda.
 * ========================================================================== */
#pragma once

#include <Arduino.h>

bool        initBoardLvgl();
bool        lvglLock(int timeoutMs);
void        lvglUnlock();
bool        isLvglReady();
bool        boardTouchReady();
const char *boardTouchStatus();
void        boardResetScreenTimeout();
bool        boardIsScreenOn();

/* luminosita' 5..100 % e standby (0 = mai), regolabili a runtime */
void        boardSetBrightness(uint8_t percent);
uint8_t     boardGetBrightness();
void        boardSetScreenTimeoutMs(uint32_t ms);
uint32_t    boardGetScreenTimeoutMs();
