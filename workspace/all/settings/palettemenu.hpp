#pragma once

#include "menu.hpp"

// Builds the "Color Palette" appearance-menu row: cycling Left/Right steps through
// "Custom" plus every enumerated ColorPalette and applies immediately; Confirm opens
// a submenu with the same choices for direct selection. The row's displayed value
// tracks CFG_getPaletteName() live, so it falls back to "Custom" the moment an
// individual color is edited elsewhere in the menu.
AbstractMenuItem* buildPaletteMenuItem();
