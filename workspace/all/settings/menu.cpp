#include "menu.hpp"

extern "C"
{
#include "defines.h"
#include "api.h"
#include "utils.h"
}

#include <cmath>
#include <mutex>
#include <shared_mutex>
typedef std::shared_mutex Lock;
typedef std::unique_lock< Lock >  WriteLock;
typedef std::shared_lock< Lock >  ReadLock;

static std::string overlayMessage;
static bool overlayVisible = false;
static OverlayDismissMode overlayDismissMode = OverlayDismissMode::None;
static SDL_Surface* overlaySurface = nullptr;
static Lock overlayLock;

static void drawOverlayLocal(SDL_Surface* screen);

///////////////////////////////////////////////////////////

MenuItem::MenuItem(ListItemType type, const std::string &name, const std::string &desc,
                   const std::vector<std::any> &values, const std::vector<std::string> &labels,
                   ValueGetCallback on_get, ValueSetCallback on_set, ValueResetCallback on_reset, 
                   MenuListCallback on_confirm, MenuList *submenu)
    : AbstractMenuItem(type, name, desc, on_get, on_set, on_reset, on_confirm, submenu), 
    values(values), labels(labels)
{
    initSelection();
}

MenuItem::MenuItem(ListItemType type, const std::string &name, const std::string &desc, const std::vector<std::any> &values,
                   ValueGetCallback on_get, ValueSetCallback on_set, ValueResetCallback on_reset,
                   MenuListCallback on_confirm, MenuList *submenu)
    : AbstractMenuItem(type, name, desc, on_get, on_set, on_reset, on_confirm, submenu), 
    values(values) /*labels({}),*/
{
    generateDefaultLabels();
    initSelection();
}

MenuItem::MenuItem(ListItemType type, const std::string &name, const std::string &desc, 
                   int min, int max, const std::string suffix,
                   ValueGetCallback on_get, ValueSetCallback on_set, ValueResetCallback on_reset,
                   MenuListCallback on_confirm, MenuList *submenu)
    : AbstractMenuItem(type, name, desc, on_get, on_set, on_reset, on_confirm, submenu)
{
    const int step = 1; // until we need it
    const int num = (max - min) / step + 1;
    for (int i = 0; i < num; i++)
        values.push_back(min + i * step);

    generateDefaultLabels(suffix);
    initSelection();
    assert(valueIdx >= 0);
}

MenuItem::MenuItem(ListItemType type, const std::string &name, const std::string &desc,
    MenuListCallback on_confirm, MenuList *submenu)
    : MenuItem(type, name, desc, 0,0, "", nullptr, nullptr, nullptr, on_confirm, submenu)
{}

void MenuItem::generateDefaultLabels(const std::string& suffix)
{
    labels.clear();
    for (auto v : values)
    {
        if (v.type() == typeid(std::string))
            labels.push_back(std::any_cast<std::string>(v) + suffix);
        else if (v.type() == typeid(float))
            labels.push_back(std::to_string(std::any_cast<float>(v)) + suffix);
        else if (v.type() == typeid(int))
            labels.push_back(std::to_string(std::any_cast<int>(v)) + suffix);
        else if (v.type() == typeid(uint32_t))
            labels.push_back(std::to_string(std::any_cast<uint32_t>(v)) + suffix);
        else if (v.type() == typeid(bool))
            labels.push_back((std::any_cast<bool>(v) ? "On" : "Off") + suffix);
        else
            assert(false); // needs more string conversion
    }
}

void MenuItem::initSelection()
{
    int oldValueIdx = valueIdx;
    valueIdx = -1;
    if (!values.empty())
    {
        valueIdx = 0;
        if (on_get)
        {
            // we know we can convert both std::any values to the same type
            const auto initialVal = on_get();
            try
            {
                for (int i = 0; i < values.size(); i++)
                {
                    const auto &v = values[i];
                    if (v.type() != initialVal.type())
                        LOG_error("type mismatch: %s vs. %s", v.type().name(), initialVal.type().name());

                    assert(v.type() == initialVal.type());

                    if (v.type() == typeid(float))
                    {
                        if (std::any_cast<float>(initialVal) == std::any_cast<float>(v))
                        {
                            valueIdx = i;
                            break;
                        }
                    }
                    else if (v.type() == typeid(int))
                    {
                        if (std::any_cast<int>(initialVal) == std::any_cast<int>(v))
                        {
                            valueIdx = i;
                            break;
                        }
                    }
                    else if (v.type() == typeid(unsigned int))
                    {
                        if (std::any_cast<unsigned int>(initialVal) == std::any_cast<unsigned int>(v))
                        {
                            valueIdx = i;
                            break;
                        }
                    }
                    else if (v.type() == typeid(uint32_t))
                    {
                        if (std::any_cast<uint32_t>(initialVal) == std::any_cast<uint32_t>(v))
                        {
                            valueIdx = i;
                            break;
                        }
                    }
                    else if (v.type() == typeid(bool))
                    {
                        if (std::any_cast<bool>(initialVal) == std::any_cast<bool>(v))
                        {
                            valueIdx = i;
                            break;
                        }
                    }
                    else if (v.type() == typeid(std::string))
                    {
                        if (std::any_cast<std::string>(initialVal) == std::any_cast<std::string>(v))
                        {
                            //LOG_info("Found equal strings %s and %s\n", initialVal, v);
                            valueIdx = i;
                            break;
                        }
                    }
                    else if (v.type() == typeid(std::basic_string<char>))
                    {
                        if (std::any_cast<std::basic_string<char>>(initialVal) == std::any_cast<std::basic_string<char>>(v))
                        {
                            //LOG_info("Found equal basic strings %s and %s\n", initialVal, v);
                            valueIdx = i;
                            break;
                        }
                    }
                    else
                    {
                        LOG_warn("Cant initialize selection for %s from unknown type %s\n", this->getLabel(), v.type().name());
                        // assert(false);
                    }
                }
            }
            catch (...)
            {
                // bad any cast
                LOG_warn("Bad any cast for %s\n", this->getLabel());
            }
            // this sadly doesnt work with std::any
            // auto it = std::find(values.cbegin(), values.cend(), on_get());
            // if (it == std::end(values))
            //    valueIdx = -1;
            // else
            //    valueIdx = std::distance(values.cbegin(), it);
        }
        assert(valueIdx >= 0);
    }
}

InputReactionHint MenuItem::handleInput(int &dirty)
{
    InputReactionHint hint = Unhandled;

    if (deferred)
    {
        assert(submenu);
        int subMenuJustClosed = 0;
        hint = submenu->handleInput(dirty, subMenuJustClosed);
        if (subMenuJustClosed) {
            defer(false);
            dirty = 1;
        }
        return hint;
    }

    // handle our custom behavior and return true if the input was handled
    if (PAD_justRepeated(BTN_LEFT))
    {
        hint = NoOp;
        if (prevValue())
        {
            if (on_set)
                on_set(getValue());
            dirty = 1;
        }
    }
    else if (PAD_justRepeated(BTN_RIGHT))
    {
        hint = NoOp;
        if (nextValue())
        {
            if (on_set)
                on_set(getValue());
            dirty = 1;
        }
    }
    if (PAD_justRepeated(BTN_L1))
    {
        hint = NoOp;
        if (prev(10))
        {
            if (on_set)
                on_set(getValue());
            dirty = 1;
        }
    }
    else if (PAD_justRepeated(BTN_R1))
    {
        hint = NoOp;
        if (next(10))
        {
            if (on_set)
                on_set(getValue());
            dirty = 1;
        }
    }
    else if (PAD_justPressed(BTN_A))
    {
        hint = NoOp; // not really, should check on_confirm
        if (on_confirm)
            hint = on_confirm(*this);
        dirty = 1;
    }

    return hint;
}

bool MenuItem::next(int n)
{
    if (valueIdx < 0)
        return false;
    valueIdx = (valueIdx + n) % values.size();
    return true;
}

bool MenuItem::prev(int n)
{
    if (valueIdx < 0)
        return false;
    valueIdx = (valueIdx + values.size() - n) % values.size();
    return true;
}

///////////////////////////////////////////////////////////

MenuList::MenuList(MenuItemType type, const std::string &descp, std::vector<AbstractMenuItem*> items, MenuListCallback on_change, MenuListCallback on_confirm)
    : type(type), desc(descp), items(items), on_change(on_change), on_confirm(on_confirm)
{
    // best effort layout based on the platform defines, user should really call performLayout manually
    performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
    layout_called = false;
}

MenuList::~MenuList()
{
    WriteLock w(itemLock);
    for (auto item : items)
        delete item;
    items.clear();
}

void MenuList::performLayout(const SDL_Rect &dst)
{
    ReadLock r(itemLock);
    // TODO: consecutive calls to this should only update max_visible rows
    // and try to persist the current selection state
    // TODO: If we ever need to add menu entries dynamically, this potentially
    // needs to be called again after.
    scope.start = 0;
    scope.selected = 0;
    scope.count = items.size();
    if (type == MenuItemType::Main)
    {
        scope.max_visible_options = (dst.h - SCALE1(PILL_SIZE)) / SCALE1(PILL_SIZE);
    }
    else
    {
        // we are leaving some space to show the description label here, account for roughly two lines or one pill
        // also account for the scroll icon, in case we need it.
        // scope.max_visible_options = (dst.h - SCALE1(PILL_SIZE * 2)) / SCALE1(BUTTON_SIZE);
        scope.max_visible_options = (dst.h - SCALE1(PILL_SIZE)) / SCALE1(BUTTON_SIZE);
    }
    scope.end = std::min(scope.count, scope.max_visible_options);
    scope.visible_rows = scope.end;

    for (auto itm : items)
        if (itm->getSubMenu())
            itm->getSubMenu()->performLayout(dst);

    layout_called = true;
}

bool MenuList::selectNext()
{
    scope.selected++;
    if (scope.selected >= scope.count)
    {
        scope.selected = 0;
        scope.start = 0;
        scope.end = scope.visible_rows;
    }
    else if (scope.selected >= scope.end)
    {
        scope.start++;
        scope.end++;
    }

    return true;
}

bool MenuList::selectPrev()
{
    scope.selected--;
    if (scope.selected < 0)
    {
        scope.selected = scope.count - 1;
        scope.start = std::max(0, scope.count - scope.max_visible_options);
        scope.end = scope.count;
    }
    else if (scope.selected < scope.start)
    {
        scope.start--;
        scope.end--;
    }

    return true;
}

std::string MenuList::getSelectedItemName() const
{
    if(items.empty())
        return "";

    int selected_row = scope.selected - scope.start;
    return items.at(selected_row)->getName();
}

bool MenuList::selectByName(const std::string &name)
{
    if(name.empty())
        return false;

    int toSelect = -1;
    for (int i = 0; i < items.size() && toSelect < 0; i++)
        if(items.at(i)->getName() == name)
            toSelect = i;
    //LOG_info("Found element %s (%d) at pos %d\n", name.c_str(), scope.selected - scope.start, toSelect);
    if (toSelect >= 0)
    {
        performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
        while(toSelect > 0) {
            selectNext();
            toSelect--;
        }
        return true;
    }
    return false;
}

// returns true if the input was handled
InputReactionHint MenuList::handleInput(int &dirty, int &quit)
{
    bool tryDismiss = false;
    {
        ReadLock r(overlayLock);
        if (overlayVisible) {
             bool dismissed = false;
             if (overlayDismissMode == OverlayDismissMode::DismissOnA && PAD_justPressed(BTN_A)) dismissed = true;
             else if (overlayDismissMode == OverlayDismissMode::DismissOnB && PAD_justPressed(BTN_B)) dismissed = true;

             if (dismissed) {
                 tryDismiss = true;
             } else {
                 return NoOp;
             }
        }
    }

    if (tryDismiss) {
        hideOverlay();
        dirty = 1;
        return NoOp;
    }

    ReadLock r(itemLock);
    InputReactionHint handled = items.at(scope.selected)->handleInput(dirty);
    if(handled == ResetAllItems) {
        resetAllItems();
        dirty = 1;
        return NoOp;
    }
    else if (handled == Exit) {
        quit = 1;
        return NoOp;
    }
    else if (handled != Unhandled)
        return handled;

    if (PAD_justRepeated(BTN_UP))
    {
        if (scope.selected == 0 && !PAD_justPressed(BTN_UP))
            return NoOp; // stop at top
        if (selectPrev())
            dirty = 1;
        return NoOp;
    }
    else if (PAD_justRepeated(BTN_DOWN))
    {
        if (scope.selected == scope.count - 1 && !PAD_justPressed(BTN_DOWN))
            return NoOp; // stop at bottom
        if (selectNext())
            dirty = 1;
        return NoOp;
    }
    else if (on_change)
    {
        // do we even need this?
    }
    else if (on_confirm && PAD_justPressed(BTN_A))
    {
        // do we even need this?
    }
    else if (PAD_justPressed(BTN_B))
    {
        quit = 1;
        return NoOp;
    }

    return Unhandled;
}

SDL_Rect MenuList::itemSizeHint(const AbstractMenuItem &item)
{
    if (type == MenuItemType::Fixed)
    {
        return {0, 0, 0, SCALE1(PILL_SIZE)};
    }
    else if (type == MenuItemType::List)
    {
        // calculate the size of the list
        int w = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &w, NULL);
        w += SCALE1(OPTION_PADDING * 2);
        return {0, 0, w, SCALE1(PILL_SIZE)};
    }
    else if (type == MenuItemType::Input || type == MenuItemType::Var)
    {
        int w = 0;
        int lw = 0;
        int rw = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &lw, NULL);
        // get the width of the widest row
        int mrw = 0;
        // every value list in an input table is the same
        // so only calculate rw for the first item...
        if (!mrw || type != MenuItemType::Input)
        {
            for (int j = 0; item.getValues().size() > j && !item.getLabels()[j].empty(); j++)
            {
                TTF_SizeUTF8(font.tiny, item.getLabels()[j].c_str(), &rw, NULL);
                if (lw + rw > w)
                    w = lw + rw;
                if (rw > mrw)
                    mrw = rw;
            }
        }
        else
        {
            w = lw + mrw;
        }
        w += SCALE1(OPTION_PADDING * 4);
        return {0, 0, w, SCALE1(PILL_SIZE)};
    }
    else if (type == MenuItemType::Main)
    {
        int w = 0;
        TTF_SizeUTF8(font.large, item.getName().c_str(), &w, NULL);
        w += SCALE1(BUTTON_PADDING * 2);
        return {0, 0, w, SCALE1(PILL_SIZE)};
    }
    else
    {
        assert(false); // new enum value?
    }
}

void MenuList::draw(SDL_Surface *surface, const SDL_Rect &dst)
{
    assert(layout_called);
    ReadLock r(itemLock);

    auto cur = !items.empty() ? items.at(scope.selected) : nullptr;
    if (cur && cur->isDeferred())
    {
        assert(cur->getSubMenu());
        cur->getSubMenu()->draw(surface, dst);
    }
    else
    {
        // iterate all items and draw them vertically
        switch (type)
        {
        case MenuItemType::List:
            drawList(surface, dst);
            break;
        case MenuItemType::Fixed:
            drawFixed(surface, dst);
            break;
        case MenuItemType::Var:
        case MenuItemType::Input:
            drawInput(surface, dst);
            break;
        case MenuItemType::Main:
            drawMain(surface, dst);
            break;
        case MenuItemType::Custom:
            drawCustom(surface, dst);
            return; // no further drawing over custom
        default:
            assert(false && "Unknown list type");
        }

        // Handle overflow (anything but Main and Custom)
        if (type != MenuItemType::Main && items.size() > scope.max_visible_options)
        {
            const int SCROLL_WIDTH = 24;
            const int SCROLL_HEIGHT = 4;
            SDL_Rect rect = dst;
            rect = dx(rect, (rect.w - SCALE1(SCROLL_WIDTH)) / 2);
            rect = dy(rect, SCALE1((-SCROLL_HEIGHT) / 2));

            if (scope.start > 0)
                // assumes there is some padding above we can yoink
                GFX_blitAssetCPP(ASSET_SCROLL_UP, {}, surface, {rect.x, rect.y - SCALE1(PADDING)});
            if (scope.end < scope.count)
                // this is with 2 * pill_size bottom margin
                // GFX_blitAssetCPP(ASSET_SCROLL_DOWN, {}, surface, {rect.x, rect.h - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE) + rect.y});
                GFX_blitAssetCPP(ASSET_SCROLL_DOWN, {}, surface, {rect.x, rect.h - SCALE1(PADDING + PILL_SIZE) + rect.y});
        }

        if (cur && cur->getDesc().length() > 0)
        {
            int w, h;
            const auto description = cur->getDesc();
            GFX_sizeText(font.tiny, description.c_str(), SCALE1(FONT_SMALL), &w, &h);
            GFX_blitTextCPP(font.tiny, description.c_str(), SCALE1(FONT_SMALL), uintToColour(THEME_COLOR4_255), surface, {(dst.x + dst.w - w) / 2, dst.y + dst.h - h, w, h});
        }
    }

    {
        WriteLock w(overlayLock);
        overlaySurface = surface;
    }

    drawOverlayLocal(surface);
}

void MenuList::drawList(SDL_Surface *surface, const SDL_Rect &dst)
{
    // we ignore type here, it all paints the same.
    if (max_width == 0)
    {
        int mw = 0;
        for (auto item : items)
        {
            auto hintRect = itemSizeHint(*item);
            if (hintRect.w > mw)
                mw = hintRect.w;
        }
        // cache the result
        max_width = std::min(mw, dst.w);
    }

    SDL_Rect rect = dst;
    rect = dx(rect, (rect.w - max_width) / 2);

    int selected_row = scope.selected - scope.start;
    for (int i = scope.start, j = 0; i < scope.end; i++, j++)
    {
        auto pos = dy(rect, SCALE1(j * BUTTON_SIZE));
        pos.h = SCALE1(BUTTON_SIZE);
        drawListItem(surface, pos, *items[i], j == selected_row);
    }
}

void MenuList::drawListItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected)
{
    SDL_Color text_color = uintToColour(THEME_COLOR4_255);
    SDL_Surface *text;

    // int ox = (dst.w - w) / 2; // if we're centering these (but I don't think we should after seeing it)
    if (selected)
    {
        // move out of conditional if centering
        int w = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &w, NULL);
        w += SCALE1(OPTION_PADDING * 2);

        GFX_blitPillDarkCPP(ASSET_BUTTON, surface, {dst.x, dst.y, w, SCALE1(BUTTON_SIZE)});
        text_color = uintToColour(THEME_COLOR5_255);
    }
    text = TTF_RenderUTF8_Blended(font.small, item.getName().c_str(), text_color);
    SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + SCALE1(OPTION_PADDING), dst.y  + ((dst.h - text->h) / 2)});
    SDL_FreeSurface(text);
}

void MenuList::drawFixed(SDL_Surface *surface, const SDL_Rect &dst)
{
    // NOTE: no need to calculate max width
    int mw = dst.w;
    // int lw,rw;
    // lw = rw = mw / 2;
    max_width = mw; // not sure about this one

    SDL_Rect rect = dst;

    int selected_row = scope.selected - scope.start;
    for (int i = scope.start, j = 0; i < scope.end; i++, j++)
    {
        auto pos = dy(rect, SCALE1(j * BUTTON_SIZE));
        pos.h = SCALE1(BUTTON_SIZE);
        drawFixedItem(surface, pos, *items[i], j == selected_row);
    }
}

// TODO: expose API functions that do the same
namespace
{
    static inline void rgb_unpack(uint32_t col, int *r, int *g, int *b)
    {
        *r = (col >> 16) & 0xff;
        *g = (col >> 8) & 0xff;
        *b = col & 0xff;
    }

    static inline uint32_t mapUint(SDL_Surface *surface, uint32_t col)
    {
        int r, g, b;
        rgb_unpack(col, &r, &g, &b);
        return SDL_MapRGB(surface->format, r, g, b);
    }
}

void MenuList::drawFixedItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected)
{
    SDL_Color text_color = uintToColour(THEME_COLOR4_255);
    SDL_Color text_color_value = uintToColour(THEME_COLOR4_255);
    SDL_Surface *text;

    // hack - this should be correlated to max_width
    int mw = dst.w;

    if (selected)
    {
        // gray pill
        GFX_blitPillLightCPP(ASSET_BUTTON, surface, {dst.x, dst.y, mw, SCALE1(BUTTON_SIZE)});
    }

    if (item.getValue().has_value())
    {
        text = TTF_RenderUTF8_Blended(font.tiny, item.getLabel().c_str(), text_color_value);

        if (item.getType() == ListItemType::Color)
        {
            // Read the live color directly from on_get() so the swatch and hex
            // label always reflect the current value, even after the RGB picker
            // sets an arbitrary color that is not in the predefined palette.
            uint32_t rawColor = item.on_get
                ? std::any_cast<uint32_t>(item.on_get())
                : std::any_cast<uint32_t>(item.getValue());
            uint32_t color = mapUint(surface, rawColor);
            SDL_Rect rect = {
                dst.x + dst.w - SCALE1(OPTION_PADDING + FONT_TINY),
                dst.y + SCALE1(BUTTON_SIZE - FONT_TINY) / 2,
                SCALE1(FONT_TINY), SCALE1(FONT_TINY)};
            SDL_FillRect(surface, &rect, RGB_WHITE);
            rect = dy(dx(rect, 1), 1);
            rect.h -= 1;
            rect.w -= 1;
            SDL_FillRect(surface, &rect, color);
#define COLOR_PADDING 4
            // Rerender the label from the live hex value
            SDL_FreeSurface(text);
            char hexLabel[12];
            snprintf(hexLabel, sizeof(hexLabel), "0x%06X", rawColor);
            text = TTF_RenderUTF8_Blended(font.tiny, hexLabel, text_color_value);
            SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + mw - text->w - SCALE1(OPTION_PADDING + COLOR_PADDING + FONT_TINY), dst.y + ((dst.h - text->h) / 2)});
        }
        else if(item.getType() == ListItemType::Button) {
            // dont draw anything for now, could be a button hint later
        }
        else if(item.getType() == ListItemType::Custom) {
            item.drawCustomItem(surface, dst, item, selected);
        }
        else // Generic and fallback
            SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + mw - text->w - SCALE1(OPTION_PADDING), dst.y + ((dst.h - text->h) / 2)});
        SDL_FreeSurface(text);
    }

    // TODO: blit a black pill on unselected rows (to cover longer item->values?) or truncate longer item->values?
    if (selected)
    {
        // white pill
        int w = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &w, NULL);
        w += SCALE1(OPTION_PADDING * 2);
        GFX_blitPillDarkCPP(ASSET_BUTTON, surface, {dst.x, dst.y, w, SCALE1(BUTTON_SIZE)});
        text_color = uintToColour(THEME_COLOR5_255);
    }

    text = TTF_RenderUTF8_Blended(font.small, item.getName().c_str(), text_color);
    SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + SCALE1(OPTION_PADDING), dst.y + ((dst.h - text->h) / 2)});
    SDL_FreeSurface(text);
}

void MenuList::drawInput(SDL_Surface *surface, const SDL_Rect &dst)
{
    // TODO: handle type if we need it
    if (max_width == 0)
    {
        // get the width of the widest row
        int mw = 0;
        for (auto item : items)
        {
            auto hintRect = itemSizeHint(*item);
            if (hintRect.w > mw)
                mw = hintRect.w;
        }
        // cache the result
        max_width = std::min(mw, dst.w);
    }

    SDL_Rect rect = dst;
    rect = dx(rect, (rect.w - max_width) / 2);

    int selected_row = scope.selected - scope.start;
    for (int i = scope.start, j = 0; i < scope.end; i++, j++)
    {
        auto pos = dy(rect, SCALE1(j * BUTTON_SIZE));
        pos.h = SCALE1(BUTTON_SIZE);
        pos.w = max_width;
        drawInputItem(surface, pos, *items[i], j == selected_row);
    }
}

void MenuList::drawInputItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected)
{
    SDL_Color text_color = COLOR_WHITE;
    SDL_Surface *text;

    // hack
    int mw = dst.w;

    if (selected)
    {
        // gray pill
        GFX_blitPillLightCPP(ASSET_BUTTON, surface, {dst.x, dst.y, mw, SCALE1(BUTTON_SIZE)});

        // white pill
        int w = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &w, NULL);
        w += SCALE1(OPTION_PADDING * 2);
        GFX_blitPillDarkCPP(ASSET_BUTTON, surface, {dst.x, dst.y, w, SCALE1(BUTTON_SIZE)});
        text_color = COLOR_BLACK;
    }
    text = TTF_RenderUTF8_Blended(font.small, item.getName().c_str(), text_color);
    SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + SCALE1(OPTION_PADDING), dst.y + ((dst.h - text->h) / 2)});
    SDL_FreeSurface(text);

    if (/*await_input &&*/ selected)
    {
        // buh
    }
    else if (item.getValue().has_value())
    {
        text = TTF_RenderUTF8_Blended(font.tiny, item.getLabel().c_str(), COLOR_WHITE); // always white
        SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + mw - text->w - SCALE1(OPTION_PADDING), dst.y + ((dst.h - text->h) / 2)});
        SDL_FreeSurface(text);
    }
}

void MenuList::drawMain(SDL_Surface *surface, const SDL_Rect &dst)
{
    // we ignore type here, it all paints the same.
    // no size calc to do here, each line is as wide as needed.

    if (scope.count > 0)
    {
        int selected_row = scope.selected - scope.start;
        for (int i = scope.start, j = 0; i < scope.end; i++, j++)
        {
            auto pos = dy(dst, SCALE1(j * PILL_SIZE));
            pos.h = SCALE1(PILL_SIZE);
            // auto hintRect = itemSizeHint(items[i]);
            // pos.w = std::min(pos.w, hintRect.w);
            // pos.h = std::min(pos.h, hintRect.h);

            drawMainItem(surface, pos, *items[i], j == selected_row);
        }
    }
    else
    {
        GFX_blitMessageCPP(font.large, "Empty folder", surface, dst);
    }
}

void MenuList::drawMainItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected)
{
    SDL_Color text_color = COLOR_WHITE;
    SDL_Surface *text;

    // TODO: unique item handling (draws grey text)
    const bool unique = false;

    char truncated[256];
    int text_width = GFX_truncateText(font.large, item.getName().c_str(), truncated, dst.w, SCALE1(BUTTON_PADDING * 2));
    int max_width = std::min(dst.w, text_width);

    if (selected)
    {
        GFX_blitPillDarkCPP(ASSET_WHITE_PILL, surface, {dst.x, dst.y, max_width, dst.h});
        text_color = COLOR_BLACK;
    }
    else if (unique)
    {
        // TODO: port this over when needed. Its complete spaghetti code...
    }
    text = TTF_RenderUTF8_Blended(font.large, truncated, text_color);
    SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + SCALE1(BUTTON_PADDING), dst.y + ((dst.h - text->h) / 2)});
    SDL_FreeSurface(text);
}

void MenuList::resetAllItems()
{
    ReadLock r(itemLock);
    for(auto item : items) {
        if(item->on_reset) {
            item->on_reset();
            item->initSelection();
 
        }
    }
}

void MenuList::showOverlay(const std::string& message, OverlayDismissMode dismissMode)
{
    {
        WriteLock w(overlayLock);
        overlayMessage = message;
        overlayVisible = true;
        overlayDismissMode = dismissMode;
    }
    
    // We want to force a draw right now since usually we are about to block
    WriteLock w(overlayLock);
    if (overlaySurface) {
        drawOverlayLocal(overlaySurface);
        GFX_flip(overlaySurface);
    }
}

void MenuList::hideOverlay()
{
    WriteLock w(overlayLock);
    overlayVisible = false;
}

bool MenuList::isOverlayVisible()
{
    ReadLock r(overlayLock);
    return overlayVisible;
}

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

static void drawOverlayLocal(SDL_Surface* screen) {
    // ReadLock r(overlayLock); // Assumes caller held lock or is safe
    if (!overlayVisible) return;
    
    if (!screen) return;

    // Use a static shadow surface to create a semi-transparent overlay
    static SDL_Surface* shadow = nullptr;
    if (!shadow || shadow->w != screen->w || shadow->h != screen->h) {
        if (shadow) SDL_FreeSurface(shadow);
        shadow = SDL_CreateRGBSurface(SDL_SWSURFACE, screen->w, screen->h, 
                                      screen->format->BitsPerPixel, 
                                      screen->format->Rmask, screen->format->Gmask, screen->format->Bmask, 0);
        SDL_FillRect(shadow, NULL, SDL_MapRGB(shadow->format, 0, 0, 0));
        SDLX_SetAlpha(shadow, SDL_SRCALPHA, 200); // Semi-transparent black
    }
    SDL_BlitSurface(shadow, NULL, screen, NULL);

    SDL_Rect screenRect = {0, 0, screen->w, screen->h};

    GFX_blitMessageCPP(font.medium, overlayMessage, screen, screenRect);
    
    if (overlayDismissMode != OverlayDismissMode::None) {
        if (overlayDismissMode == OverlayDismissMode::DismissOnB) {
            char *hints[] = {(char *)("B"), (char *)("BACK"), NULL};
            GFX_blitButtonGroup(hints, 1, screen, 1);
        } else if (overlayDismissMode == OverlayDismissMode::DismissOnA) {
            char *hints[] = {(char *)("A"), (char *)("OK"), NULL};
            GFX_blitButtonGroup(hints, 1, screen, 1);
        }
    }
}
