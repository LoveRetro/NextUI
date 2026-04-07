#pragma once

#include "menu.hpp"

struct ColorPreset {
    uint32_t color;   // 0xRRGGBB
    std::string label;
};

class ColorPickerMenu : public MenuList
{
    int r, g, b;
    int selected;
    ValueSetCallback on_set;
    std::vector<ColorPreset> presets;
    std::string label_;
    uint32_t originalColor_;

    uint32_t currentColor() const;
    void applyColor();
    void drawSlider(SDL_Surface *surface, const SDL_Rect &row,
                    const char *label, int value, bool is_selected, int channel);
    void drawPreset(SDL_Surface *surface, const SDL_Rect &row,
                    const ColorPreset &preset, bool is_selected);

public:
    ColorPickerMenu(uint32_t initialColor, ValueSetCallback on_set,
                    std::vector<ColorPreset> presets, std::string label);

    void reset(uint32_t color, std::vector<ColorPreset> newPresets, std::string label);

    InputReactionHint handleInput(int &dirty, int &quit) override;
    void drawCustom(SDL_Surface *surface, const SDL_Rect &dst, const SDL_Rect &dstTitle) override;
};
