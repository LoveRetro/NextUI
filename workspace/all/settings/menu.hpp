#pragma once

extern "C"
{
#include "sdl.h"
#include "defines.h"
#include "api.h"
}

#include <cassert>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <any>
#include <numeric>
#include <shared_mutex>

// leftovers to port
#define OPTION_PADDING 8

// c++ compat/convenience
// MinUI is passing a lot of temp value ptrs, which is not very c++
inline bool rectIsNull(const SDL_Rect &rect)
{
    return rect.x + rect.y + rect.h + rect.w == 0;
}

inline SDL_Rect *rectOrNull(const SDL_Rect &rect)
{
    if (rectIsNull(rect))
        return nullptr;
    return const_cast<SDL_Rect *>(&rect);
}

inline void GFX_blitAssetCPP(int asset, const SDL_Rect &src_rect, SDL_Surface *dst, const SDL_Rect &dst_rect)
{
    GFX_blitAsset(asset, rectOrNull(src_rect), dst, rectOrNull(dst_rect));
}

inline void GFX_blitTextCPP(TTF_Font *font, const char *str, int leading, SDL_Color color, SDL_Surface *dst, const SDL_Rect &src_rect)
{
    GFX_blitText(font, str, leading, color, dst, rectOrNull(src_rect));
}

inline void GFX_blitPillDarkCPP(int asset, SDL_Surface *dst, const SDL_Rect &dst_rect)
{
    GFX_blitPillDark(asset, dst, rectOrNull(dst_rect));
}

inline void GFX_blitPillLightCPP(int asset, SDL_Surface *dst, const SDL_Rect &dst_rect)
{
    GFX_blitPillLight(asset, dst, rectOrNull(dst_rect));
}

inline void GFX_blitMessageCPP(TTF_Font *font, const std::string &msg, SDL_Surface *dst, const SDL_Rect &dst_rect)
{
    GFX_blitMessage(font, const_cast<char *>(msg.c_str()), dst, rectOrNull(dst_rect));
}

inline int SDL_BlitSurfaceCPP(SDL_Surface *src, const SDL_Rect &srcrect, SDL_Surface *dst, const SDL_Rect &dstrect)
{
    return SDL_BlitSurface(src, rectOrNull(srcrect), dst, rectOrNull(dstrect));
}

inline SDL_Rect dx(const SDL_Rect &rect, int dx)
{
    return {rect.x + dx, rect.y, rect.w - dx, rect.h};
}

inline SDL_Rect dy(const SDL_Rect &rect, int dy)
{
    return {rect.x, rect.y + dy, rect.w, rect.h - dy};
}

enum class MenuItemType
{
    // eg. save and main menu
    // small font, centered list of "buttons"
    List,
    // eg. frontend
    // small font, centered list of options with a value
    Var,
    // eg. emulator settings
    // small font, full width list of options with a value, scrollable
    Fixed,
    // e.g. button mapping
    // renders like Var but handles input differently
    Input,
    // "big" MinUI menu
    Main,
    // refer/defer to sublass
    Custom,
};

// This should probably just be polymorphism, but this is fine for now.
enum class ListItemType
{
    // generic list item, could be anything and any type
    Generic,
    // hex color, typically as uint32_t
    Color,
    // no option values, only title and BTN_CONFIRM
    Button,
    // used by custom items, defer to MenuItem::drawCustomItem
    Custom,
};

enum InputReactionHint
{
    // Bubble up handling to caller. 
    // \note All other hints imply the event has been handled.
    Unhandled,
    // No specific hint available
    NoOp,
    // Caller should quit
    Exit,
    // Caller should step to the next list item
    NextItem,
    // Caller should reset items to default
    ResetAllItems
};

class AbstractMenuItem;
class MenuList;
// typedef InputReactionHint (*MenuListCallback)(MenuList* list, int i);
//using ValueGetCallback = std::any (*)(void);
//using ValueSetCallback = void (*)(const std::any &);
//using ValueResetCallback = void (*)(void);
using MenuListCallback = std::function<InputReactionHint(AbstractMenuItem &item)>;
using ValueGetCallback = std::function<std::any(void)>;
using ValueSetCallback = std::function<void(const std::any& value)>;
using ValueResetCallback = std::function<void(void)>;

class AbstractMenuItem
{
protected:
    friend class MenuList;

    ListItemType type;
    std::string name, desc;

    MenuListCallback on_confirm; // handling drill-down and other custom item stuff
    ValueGetCallback on_get;
    ValueSetCallback on_set;
    ValueResetCallback on_reset;
    // MenuListCallback on_change;

    MenuList *submenu{nullptr};
    bool deferred{false};

    virtual void initSelection() {}
    virtual bool nextValue() { return next(1); };
    virtual bool prevValue() { return prev(1); };
    virtual bool next(int n) { return false; /* unchanged */ };
    virtual bool prev(int n) { return false; /* unchanged */ };

public:
    AbstractMenuItem(ListItemType type, const std::string &name, const std::string &desc,
                    ValueGetCallback on_get = nullptr, ValueSetCallback on_set = nullptr,
                     ValueResetCallback on_reset = nullptr, MenuListCallback on_confirm = nullptr,
                     MenuList *submenu = nullptr)
        : type(type), name(name), desc(desc), on_get(on_get), on_set(on_set), 
        on_reset(on_reset), on_confirm(on_confirm), submenu(submenu) {}
    ~AbstractMenuItem() {
         // delete submenu;
    }

    virtual const std::any getValue() const = 0;
    virtual const std::string getLabel() const = 0;

    virtual InputReactionHint handleInput(int &dirty) { return Unhandled; };

    const std::string &getName() const { return name; }
    const std::string &getDesc() const { return desc; }
    void setDesc(const std::string &d) { desc = d; }
    const ListItemType getType() const { return type; }

    virtual void drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const {}

    virtual const std::vector<std::any> getValues() const { return {getValue()}; };
    virtual const std::vector<std::string> getLabels() const { return {getLabel()}; };

    bool isDeferred() const { return deferred; }
    void defer(bool on) { deferred = on; }
    MenuList *getSubMenu() { return submenu; }
};

// A simple menu item visualizing a fixed label and value that is read-only.
class StaticMenuItem : public AbstractMenuItem
{
public:
    StaticMenuItem(ListItemType type, const std::string &name, const std::string &desc,
        ValueGetCallback on_get)
        : AbstractMenuItem(type, name, desc, on_get) {}

    const std::any getValue() const override {
        assert(on_get);
        if(on_get)
            return on_get();
        return {};
    }
    const std::string getLabel() const override {
        auto v = getValue();
        assert(v.has_value());
        return std::any_cast<const std::string>(v);
    }
};

// A generic menu item visualizing a list if values and the currently selected value.
class MenuItem : public AbstractMenuItem
{
    std::vector<std::any> values;
    std::vector<std::string> labels;
    int valueIdx;

    void generateDefaultLabels(const std::string& suffix = "");
    virtual void initSelection() override;
    bool next(int n) override;
    bool prev(int n) override;

public:
    MenuItem(ListItemType type, const std::string &name, const std::string &desc,
             const std::vector<std::any> &values, const std::vector<std::string> &labels,
             ValueGetCallback on_get = nullptr, ValueSetCallback on_set = nullptr, 
             ValueResetCallback on_reset = nullptr, MenuListCallback on_confirm = nullptr, 
             MenuList *submenu = nullptr);

    MenuItem(ListItemType type, const std::string &name, const std::string &desc, const std::vector<std::any> &values,
             ValueGetCallback on_get = nullptr, ValueSetCallback on_set = nullptr, 
             ValueResetCallback on_reset = nullptr, MenuListCallback on_confirm = nullptr, 
             MenuList *submenu = nullptr);

    MenuItem(ListItemType type, const std::string &name, const std::string &desc, int min, int max, const std::string suffix,
             ValueGetCallback on_get = nullptr, ValueSetCallback on_set = nullptr, 
             ValueResetCallback on_reset = nullptr, MenuListCallback on_confirm = nullptr, 
             MenuList *submenu = nullptr);

    MenuItem(ListItemType type, const std::string &name, const std::string &desc,
            MenuListCallback on_confirm = nullptr, MenuList *submenu = nullptr);

    virtual InputReactionHint handleInput(int &dirty) override;

    const std::any getValue() const override
    {
        assert(valueIdx >= 0);
        return values[valueIdx];
    }
    const std::string getLabel() const override
    {
        assert(valueIdx >= 0);
        return labels[valueIdx];
    }
    const std::vector<std::any> getValues() const override{ return values; }
    const std::vector<std::string> getLabels() const override { return labels; }
};

class MenuList
{
protected:
    MenuItemType type;
    std::string desc;
    std::vector<AbstractMenuItem*> items;
    int max_width{0}; // cached on first draw
    bool layout_called{false};

    struct Scope
    {
        int start;
        int end;
        int count;
        int visible_rows;
        int max_visible_options;
        int selected;
    } scope;

    MenuListCallback on_change;
    MenuListCallback on_confirm;

    std::shared_mutex itemLock;

public:
    MenuList(MenuItemType type, const std::string &desc, std::vector<AbstractMenuItem*> items, MenuListCallback on_change = nullptr, MenuListCallback on_confirm = nullptr);
    ~MenuList();
    MenuList(MenuList &) = delete;

    void performLayout(const SDL_Rect &dst);
    bool selectNext();
    bool selectPrev();
    void resetAllItems();

    // returns true if the input was handled
    virtual InputReactionHint handleInput(int &dirty, int &quit);

    SDL_Rect itemSizeHint(const AbstractMenuItem &item);

    void draw(SDL_Surface *surface, const SDL_Rect &dst);
    void drawList(SDL_Surface *surface, const SDL_Rect &dst);
    void drawListItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected);
    void drawFixed(SDL_Surface *surface, const SDL_Rect &dst);
    void drawFixedItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected);
    void drawInput(SDL_Surface *surface, const SDL_Rect &dst);
    void drawInputItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected);
    void drawMain(SDL_Surface *surface, const SDL_Rect &dst);
    void drawMainItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected);
    virtual void drawCustom(SDL_Surface *surface, const SDL_Rect &dst) {};
};

const MenuListCallback DeferToSubmenu = [](AbstractMenuItem &itm) -> InputReactionHint
{
    if (itm.getSubMenu())
        itm.defer(true);
    return InputReactionHint::NoOp;
};

const MenuListCallback ResetCurrentMenu = [](AbstractMenuItem &itm) -> InputReactionHint
{
    return InputReactionHint::ResetAllItems;
};