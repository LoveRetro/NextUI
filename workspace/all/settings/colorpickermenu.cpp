#include "colorpickermenu.hpp"

#include <cmath>

///////////////////////////////////////////////////////////
// ColorPickerMenu

ColorPickerMenu::ColorPickerMenu(uint32_t initialColor, ValueSetCallback on_set,
                                 std::vector<ColorPreset> presets)
    : MenuList(MenuItemType::Custom, "", {}), on_set(on_set),
      presets(std::move(presets)), r(0), g(0), b(0), selected(0)
{
    r = (initialColor >> 16) & 0xFF;
    g = (initialColor >> 8) & 0xFF;
    b = initialColor & 0xFF;
}

void ColorPickerMenu::reset(uint32_t color, std::vector<ColorPreset> newPresets)
{
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
    selected = 0;
    presets = std::move(newPresets);
}

uint32_t ColorPickerMenu::currentColor() const
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void ColorPickerMenu::applyColor()
{
    if (on_set)
        on_set(currentColor());
}

InputReactionHint ColorPickerMenu::handleInput(int &dirty, int &quit)
{
    int total = 3 + (int)presets.size();

    if (PAD_justRepeated(BTN_UP))
    {
        if (selected > 0)
        {
            selected--;
            dirty = 1;
        }
        return NoOp;
    }
    else if (PAD_justRepeated(BTN_DOWN))
    {
        if (selected < total - 1)
        {
            selected++;
            dirty = 1;
        }
        return NoOp;
    }
    else if (PAD_justPressed(BTN_B))
    {
        quit = 1;
        return NoOp;
    }

    if (selected < 3)
    {
        int *channel = (selected == 0) ? &r : (selected == 1) ? &g : &b;
        if (PAD_justRepeated(BTN_LEFT))
        {
            *channel = std::max(0, *channel - 1);
            applyColor();
            dirty = 1;
            return NoOp;
        }
        else if (PAD_justRepeated(BTN_RIGHT))
        {
            *channel = std::min(255, *channel + 1);
            applyColor();
            dirty = 1;
            return NoOp;
        }
        else if (PAD_justRepeated(BTN_L1))
        {
            *channel = std::max(0, *channel - 16);
            applyColor();
            dirty = 1;
            return NoOp;
        }
        else if (PAD_justRepeated(BTN_R1))
        {
            *channel = std::min(255, *channel + 16);
            applyColor();
            dirty = 1;
            return NoOp;
        }
    }
    else
    {
        int presetIdx = selected - 3;
        if (PAD_justPressed(BTN_A) && presetIdx < (int)presets.size())
        {
            const auto &preset = presets[presetIdx];
            r = (preset.color >> 16) & 0xFF;
            g = (preset.color >> 8) & 0xFF;
            b = preset.color & 0xFF;
            applyColor();
            selected = 0;
            dirty = 1;
            return NoOp;
        }
    }

    return Unhandled;
}

static void drawSolidRoundedRect(SDL_Surface *surface, const SDL_Rect &rect, int radius, uint32_t color)
{
    int r = std::max(0, std::min(radius, std::min(rect.w, rect.h) / 2));
    for (int row = 0; row < rect.h; row++)
    {
        int x_clip = 0;
        if (row < r) {
            float dx = sqrtf((float)(2 * r * row - row * row));
            x_clip = r - (int)dx;
        } else if (row >= rect.h - r) {
            int i = rect.h - 1 - row;
            float dx = sqrtf((float)(2 * r * i - i * i));
            x_clip = r - (int)dx;
        }
        int x0 = rect.x + x_clip;
        int span = rect.w - 2 * x_clip;
        if (span <= 0) continue;
        SDL_Rect l = {x0, rect.y + row, span, 1};
        SDL_FillRect(surface, &l, color);
    }
}

static void drawRoundedRect(SDL_Surface *surface, const SDL_Rect &rect, int radius,
                             uint32_t outer, uint32_t middle, uint32_t fill)
{
    drawSolidRoundedRect(surface, rect, radius, outer);
    if (rect.w > 2 && rect.h > 2) {
        SDL_Rect r1 = {rect.x+1, rect.y+1, rect.w-2, rect.h-2};
        drawSolidRoundedRect(surface, r1, std::max(0, radius-1), middle);
    }
    if (rect.w > 4 && rect.h > 4) {
        SDL_Rect r2 = {rect.x+2, rect.y+2, rect.w-4, rect.h-4};
        drawSolidRoundedRect(surface, r2, std::max(0, radius-2), fill);
    }
}

static void drawGradientCapsule(SDL_Surface *surface, const SDL_Rect &bar,
                                 int r, int g, int b, int channel)
{
    if (bar.w <= 0 || bar.h <= 0) return;
    const int R = bar.h / 2;

    for (int x = 0; x < bar.w; x++)
    {
        int ch_val = (bar.w > 1) ? (int)((float)x / (bar.w - 1) * 255.0f) : 128;
        uint8_t cr = (channel == 0) ? (uint8_t)ch_val : (uint8_t)r;
        uint8_t cg = (channel == 1) ? (uint8_t)ch_val : (uint8_t)g;
        uint8_t cb = (channel == 2) ? (uint8_t)ch_val : (uint8_t)b;
        uint32_t col = SDL_MapRGB(surface->format, cr, cg, cb);

        int y_inset = 0;
        if (R > 0) {
            if (x < R) {
                int dx = R - x;
                float dy = sqrtf((float)(R * R - dx * dx));
                y_inset = R - (int)dy;
            } else if (x >= bar.w - R) {
                int dx = x - (bar.w - 1 - R);
                float inner = (float)(R * R - dx * dx);
                if (inner < 0.0f) continue;
                y_inset = R - (int)sqrtf(inner);
            }
        }

        int col_h = bar.h - 2 * y_inset;
        if (col_h <= 0) continue;
        SDL_Rect col_rect = {bar.x + x, bar.y + y_inset, 1, col_h};
        SDL_FillRect(surface, &col_rect, col);
    }
}

void ColorPickerMenu::drawSlider(SDL_Surface *surface, const SDL_Rect &row,
                                 const char *label, int value, bool is_selected, int channel)
{
    if (is_selected)
        GFX_blitPillLightCPP(ASSET_BUTTON, surface, row);

    SDL_Color text_color = is_selected ? uintToColour(THEME_COLOR5_255) : uintToColour(THEME_COLOR4_255);

    // Channel label ("R", "G", "B")
    SDL_Surface *label_surf = TTF_RenderUTF8_Blended(font.small, label, text_color);
    int label_w = label_surf->w;
    SDL_BlitSurfaceCPP(label_surf, {}, surface,
                       {row.x + SCALE1(OPTION_PADDING), row.y + (row.h - label_surf->h) / 2});
    SDL_FreeSurface(label_surf);

    // Hex value right-aligned ("FF", "00", etc.)
    char hex_str[3];
    snprintf(hex_str, sizeof(hex_str), "%02X", value);
    SDL_Surface *hex_surf = TTF_RenderUTF8_Blended(font.tiny, hex_str, text_color);
    int hex_w = hex_surf->w;
    SDL_BlitSurfaceCPP(hex_surf, {}, surface,
                       {row.x + row.w - hex_w - SCALE1(OPTION_PADDING),
                        row.y + (row.h - hex_surf->h) / 2});
    SDL_FreeSurface(hex_surf);

    // Slider bar
    int bar_x = row.x + SCALE1(OPTION_PADDING) + label_w + SCALE1(OPTION_PADDING);
    int bar_w = row.w - SCALE1(OPTION_PADDING) - label_w - SCALE1(OPTION_PADDING * 3) - hex_w;
    int bar_h = SCALE1(6);
    int bar_y = row.y + (row.h - bar_h) / 2;

    if (bar_w > 0)
    {
        uint32_t white = SDL_MapRGB(surface->format, 255, 255, 255);
        uint32_t black = SDL_MapRGB(surface->format, 0, 0, 0);

        // Capsule border: black outer ring, white inner ring (mirrors color preview style)
        SDL_Rect border_outer = {bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4};
        drawSolidRoundedRect(surface, border_outer, bar_h / 2 + 2, black);
        SDL_Rect border_inner = {bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2};
        drawSolidRoundedRect(surface, border_inner, bar_h / 2 + 1, white);

        // Gradient capsule
        drawGradientCapsule(surface, {bar_x, bar_y, bar_w, bar_h}, this->r, this->g, this->b, channel);

        // Circle thumb indicator
        const int circ_d      = bar_h + SCALE1(4);   // slightly larger than bar
        const int circ_border = SCALE1(2);            // black ring thickness (each side)

        int cx = bar_x + (int)((float)value / 255.0f * (bar_w - 1));
        int cy = bar_y + bar_h / 2;
        // clamp so circle stays inside bar bounds
        cx = std::max(bar_x + circ_d / 2, std::min(bar_x + bar_w - 1 - circ_d / 2, cx));

        SDL_Rect outer_rect = {cx - circ_d / 2, cy - circ_d / 2, circ_d, circ_d};
        drawSolidRoundedRect(surface, outer_rect, circ_d / 2, black);
        int inner_d = circ_d - circ_border * 2;
        SDL_Rect inner_rect = {cx - inner_d / 2, cy - inner_d / 2, inner_d, inner_d};
        drawSolidRoundedRect(surface, inner_rect, inner_d / 2, white);
    }
}

void ColorPickerMenu::drawPreset(SDL_Surface *surface, const SDL_Rect &row,
                                 const ColorPreset &preset, bool is_selected)
{
    if (is_selected)
        GFX_blitPillLightCPP(ASSET_BUTTON, surface, row);

    // Small color square with white border
    int sq = SCALE1(FONT_TINY);
    SDL_Rect sq_rect = {row.x + SCALE1(OPTION_PADDING), row.y + (row.h - sq) / 2, sq, sq};
    SDL_FillRect(surface, &sq_rect, SDL_MapRGB(surface->format, 255, 255, 255));
    SDL_Rect sq_inner = {sq_rect.x + 1, sq_rect.y + 1, sq_rect.w - 2, sq_rect.h - 2};
    SDL_FillRect(surface, &sq_inner, SDL_MapRGB(surface->format,
        (preset.color >> 16) & 0xFF, (preset.color >> 8) & 0xFF, preset.color & 0xFF));

    SDL_Color text_color = is_selected ? uintToColour(THEME_COLOR5_255) : uintToColour(THEME_COLOR4_255);

    // Hex value "#RRGGBB"
    char hex_str[8];
    snprintf(hex_str, sizeof(hex_str), "#%06X", preset.color);
    SDL_Surface *hex_surf = TTF_RenderUTF8_Blended(font.tiny, hex_str, text_color);
    int hex_x = sq_rect.x + sq + SCALE1(OPTION_PADDING / 2 + 2);
    SDL_BlitSurfaceCPP(hex_surf, {}, surface,
                       {hex_x, row.y + (row.h - hex_surf->h) / 2});
    int hex_w = hex_surf->w;
    SDL_FreeSurface(hex_surf);

    // Name label
    SDL_Surface *name_surf = TTF_RenderUTF8_Blended(font.tiny, preset.label.c_str(), text_color);
    SDL_BlitSurfaceCPP(name_surf, {}, surface,
                       {hex_x + hex_w + SCALE1(OPTION_PADDING),
                        row.y + (row.h - name_surf->h) / 2});
    SDL_FreeSurface(name_surf);
}

void ColorPickerMenu::drawCustom(SDL_Surface *surface, const SDL_Rect &dst)
{
    const int TOP_OFFSET    = SCALE1(4);
    const int PREVIEW_SIZE  = SCALE1(BUTTON_SIZE * 2 + PADDING);
    const int SLIDER_AREA_W = dst.w - PREVIEW_SIZE - SCALE1(PADDING);
    const int PREVIEW_RADIUS = SCALE1(4);

    uint32_t white = SDL_MapRGB(surface->format, 255, 255, 255);
    uint32_t black = SDL_MapRGB(surface->format, 0, 0, 0);
    uint32_t col = currentColor();
    uint32_t col_mapped = SDL_MapRGB(surface->format, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);

    // R, G, B slider rows
    const char *channel_labels[] = {"R", "G", "B"};
    int channel_values[] = {r, g, b};
    for (int i = 0; i < 3; i++)
    {
        SDL_Rect row = {dst.x, dst.y + TOP_OFFSET + SCALE1(i * BUTTON_SIZE), SLIDER_AREA_W, SCALE1(BUTTON_SIZE)};
        drawSlider(surface, row, channel_labels[i], channel_values[i], selected == i, i);
    }

    // Color preview square to the right of sliders, centered vertically in the 3-slider area
    SDL_Rect preview = {
        dst.x + SLIDER_AREA_W + SCALE1(PADDING),
        dst.y + TOP_OFFSET + (SCALE1(BUTTON_SIZE * 3) - PREVIEW_SIZE) / 2,
        PREVIEW_SIZE,
        PREVIEW_SIZE
    };
    drawRoundedRect(surface, preview, PREVIEW_RADIUS, white, black, col_mapped);

    // Preset rows below the sliders
    for (int i = 0; i < (int)presets.size(); i++)
    {
        SDL_Rect row = {
            dst.x,
            dst.y + TOP_OFFSET + SCALE1(BUTTON_SIZE * 3) + SCALE1(i * BUTTON_SIZE),
            dst.w,
            SCALE1(BUTTON_SIZE)
        };
        drawPreset(surface, row, presets[i], selected == 3 + i);
    }
}
