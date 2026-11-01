#include "input/game_context.h"

namespace kuf::input {
namespace {

constexpr uint32_t kDestroyedControlVtable = 0x8203ABF0;  // docs/MENU_ARCHITECTURE.md 2.4

// The in-game menus, dialogs and screens that take the input while the world may keep running (scene numbers of
// BsUiControlID.h). Not here: the HUD (0x005 player state is also a status menu page, but the status menu's focus is
// on 0x003 / 0x010), the gauges 0x006, messages 0x007, remote panels 0x00A / 0x030 / 0x031, act timer 0x00D,
// level-up notice 0x011, enemy bar 0x019, combo 0x01B, minimap 0x022, letterbox 0x023, the step roll 0x040.
constexpr uint16_t kMenuScenes[] = {
    0x003,  // SN_STATEMANAGE (status menu: equipment, inventory)
    0x004,  // SN_ITEM_CHANGE
    0x008,  // SN_RESISTANCE
    0x009,  // SN_INVEN_ITEMUSE
    0x00B,  // SN_REMOTE_SELECT
    0x00C,  // SN_REMOTE_EQUIPMENT
    0x00E,  // SN_GAMEOVER
    0x00F,  // SN_LEVELPOINT
    0x010,  // SN_LEVELUPBTN (the status menu's row 0)
    0x012,  // SN_ITEMTRADE_BUYING_MYITEM
    0x013,  // SN_PROFILE_OPTION_LIST (the options menu)
    0x015,  // SN_PROFILE_OPTION_QUEST
    0x016, 0x017,  // SN_ITEMTRADE_SELLING, _INFO
    0x018,  // SN_ABILITY_CHANGE
    0x01C, 0x01D,  // SN_LEARNABILITY, _INFO
    0x01E, 0x01F,  // SN_INVENBOX_INVEN, _BOX
    0x020, 0x021,  // SN_ITEMFUSION_INPUT, _OUTPUT
    0x024,  // SN_ITEMTRADE_BUYING
    0x025,  // SN_MSG_BOX (confirmations)
    0x026,  // SN_ITEMTRADE_BUYING_INFO
    0x027,  // SN_PROFILE_OPTION_CONFIG (Settings; the PC Settings and Key Bindings pages)
    0x028,  // SN_PROFILE_CONTROLLER (Controls)
    0x032,  // SN_ITEM_EXCHANGE_MINE
    0x033,  // SN_PROFILE_OPTION_MAP
    0x034,  // SN_ACT_CLEAR
    0x038,  // SN_ITEM_EXCHANGE_INFO
    0x03C,  // SN_ITEM_EXCHANGE_OTHER
    0x03E,  // SN_STATEMANAGE_DLC_ITEM_CHANGE
    0x03F,  // SN_PROFILE_OPTION_BOSSKILL
    0x041,  // SN_TITLE_OPTION
    0x042,  // SN_ITEMDESC
    0x043,  // SN_ITEM_INVEN
};

}  // namespace

bool IsInGameMenuScene(uint32_t scene_id) {
  if (scene_id & 0xFFFFF) return false;  // not a scene ID
  const uint32_t n = scene_id >> 20;
  for (const uint16_t s : kMenuScenes) {
    if (s == n) return true;
  }
  return false;
}

bool MenuHasFocus(const UiFocus& focus) {
  return focus.readable && focus.vtable != 0 && focus.vtable != kDestroyedControlVtable &&
         IsInGameMenuScene(focus.scene_id);
}

GameContext DecideGameContext(bool waiting_for_start, bool has_target, bool menu_focus) {
  if (waiting_for_start) return GameContext::kStartPrompt;
  return has_target && !menu_focus ? GameContext::kGameplay : GameContext::kMenu;
}

}  // namespace kuf::input
