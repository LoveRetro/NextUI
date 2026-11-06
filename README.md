A fork of NextUI to improve usability for myself and make the OS easier to use for my parents.

**This repo is a hobby project, it is not actively maintained.**

If you want to use NextUI for your own handheld, get it from [Love Retro](https://github.com/LoveRetro/NextUI).

## AI Disclaimer

In the interest of full transparency: this fork was developed using AI tooling, specifically Claude Code.

I've been a professional software developer since 2020 and AI agents are just part of my workflow. I have never developed C or C++ professionally (though I do like to use C for hobby projects) so while I've reviewed every code change, I am limited by my lack of experience, esp for C-specific things like safe memory management. (If you care, my primary languages are C# for backend/desktop and TypeScript for frontend.)

I have done my best to keep PRs and commits small for reviewability should anyone want to cherry pick features out of this fork.

## Context

I got my parents an ANBERNIC RG 40XXH for Christmas so they could play their childhood games, but I knew immediately I was not going to be using the stock OS. After trying a few options I landed on using NextUI because it was focused and booted in and out of games extremely fast. It still felt open and breakable though, so I started this personal fork to lock it down and remove some footguns that I shot myself with in the past using NextUI on my own ANBERNIC RG SP.

## Changes

Everything was developed as atomic features that could be toggled via the settings. I did my best to develop the features in one PR per feature-setting to make cherry-picking easier if other folks want to take some of these. Every PR is tagged with what feature it touched for easier navigation.

In order of smallest to largest:

| Feature | Setting | Description | Reason | Limitations |
|-|-|-|-|-|
| Screen margin | Appearance / Screen margin | Adjusts the margin between the edge of the screen and widgets like the battery and input prompts. | Reclaims space on low resolution devices | Overrides both low res and high res / external displays with the same value when by default they are different. |
| Quick menu title | Appearance / Show Quick Menu title | Shows a title for each quick setting button in the top left of the screen. | Remove ambiguity in what the icon-only buttons do in the quick menu. | |
| Quick menu tiles | Appearch / Show X in Quick Menu | A series of options to disable the tiles that appear in the quick menu, specifically recents, games, and tools. | Recents and games as options don't make sense for my needs and at that point it was just tools so I cut it too for a cleaner look. | |
| All games list | Appearance / All games view | Skips the system menu and just shows all games in a single alphabatized list. | With a small games library (10-12 games) the system menu was more a burden than anything. | Recently played doesn't have anywhere to live so only exists in the quick menu. |
| Resume with A | System / Resume with A | Swaps the A/X behavior so resume is mapped to A and open is mapped to X | A is the default way someone would try to launch a game so this effectively lets someone make resuming their default action. | |
| Save & Quit | System / Allow Save & Quit from menu | Adds the Save & Quit option to the in-game menu if the game is targeting a save slot already. | Remembering a hotkey is more annoying than using the menu. | |
| Grid view | Appearance / Content layout + Appearance / Grid rows | Show the list in a horizontal grid of icon buttons, rendering the game title in the top left corner. | A horizontal grid is more familiar to people trained on smartphones. | Entries that don't have an icon defined use their first letter as their icon and those icons have to be provided manually. |

## Testing

All testing has been done on device with the H700 powered ANBERNIC RG 40XXH. I don't have any devices running other NextUI supported architectures to test on them.


<div align="center">

  <img width=128px src="github/logo_outline.png">

</div>

<hr>

<h4 align="center">
  <a href="https://nextui.loveretro.games/usage/#getting-started" target="_blank">Installation Guide</a>
  ·
  <a href="https://nextui.loveretro.games/docs/" target="_blank">Documentation</a>
  ·
  <a href="https://discord.gg/HKd7wqZk3h" target="_blank">Discord</a>
</h4>

<div align="center"><p>
    <a href="https://github.com/LoveRetro/NextUI/releases/latest">
      <img alt="Latest release" src="https://img.shields.io/github/v/release/LoveRetro/NextUI?style=for-the-badge&logo=starship&include_prerelease=false&color=C9CBFF&logoColor=D9E0EE&labelColor=302D41&sort=semver" />
    </a>
    <a href="https://github.com/LoveRetro/NextUI/pulse">
      <img alt="Last commit" src="https://img.shields.io/github/last-commit/LoveRetro/NextUI?style=for-the-badge&logo=starship&color=8bd5ca&logoColor=D9E0EE&labelColor=302D41"/>
    </a>
    <a href="https://github.com/LoveRetro/NextUI/blob/main/LICENSE">
      <img alt="License" src="https://img.shields.io/github/license/LoveRetro/NextUI?style=for-the-badge&logo=starship&color=ee999f&logoColor=D9E0EE&labelColor=302D41" />
    </a>
    <a href="https://github.com/LoveRetro/NextUI/stargazers">
      <img alt="Stars" src="https://img.shields.io/github/stars/LoveRetro/NextUI?style=for-the-badge&logo=starship&color=c69ff5&logoColor=D9E0EE&labelColor=302D41" />
    </a>
    <a href="https://github.com/LoveRetro/NextUI/issues">
      <img alt="Issues" src="https://img.shields.io/github/issues/LoveRetro/NextUI?style=for-the-badge&logo=bilibili&color=F5E0DC&logoColor=D9E0EE&labelColor=302D41" />
    </a>
</div>

---

A CFW based of MinUI with a rebuild emulation engine and tons of added features for the TrimUI Brick and Smart Pro. 

For other devices we recommend checking out [MinUI](https://github.com/shauninman/MinUI)

## License

This project is licensed under [PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0).
Personal, educational, and non-commercial use is freely permitted. Commercial use is not.
See [LICENSE](LICENSE) and [NOTICE](NOTICE) for full details.

> **Note:** This project was previously licensed under GPL 3.0. As of August 15, 2026, the license
> has changed to PolyForm Noncommercial 1.0.0. The full transition history and rationale
> is documented in [issue #765](https://github.com/LoveRetro/NextUI/issues/765).

## Changes and additions

- Fixed both screen tearing and sync stutter problems of MinUI by rebuilding the emulator engine core
- Game switcher menu (Onion OS style) by [@frysee](https://github.com/frysee)
- High audio quality, due to advanced resampling engine using [libsamplerate](https://github.com/libsndfile/libsamplerate) (with quality/performance setting per emulator)
- Much lower latency (average 20ms, 1 frame of 60fps)
- Shaders!
- Fully OpenGL/GPU based for faster performance!
- native WiFi support
- Game art/media support
- Game time tracker!
- Cheats support
- Integrated WiFi
- Bluetooth Audio
- Overlays support!
- Dynamic cpu speed scaling (keeps your device cool and extends battery life, but gives the performance right when needed)
- Fully GPU OpenGL based for faster performance
- Custom bootlogos contributed by [@SolvalouArt](https://bsky.app/profile/solvalouart.bsky.social)
- Color and font settings to customize the NextUI interface
- Menu animations
- LED control, change colors, effects, brightness
- LED indicators, low battery, stand by, charging (brightness can be set seperately)
- Haptic feedback in the menu by [@ExonakiDev](https://github.com/exonakidev)
- Ambient LED mode, the LEDS act like Ambient light TV's for extra immersion, configurable per emulator
- Display controls (color temperature, brightness, contrast, saturation, exposure)
- Support for automatic NTP time synchronization with timezones and realtime clock (RTC)
- Deep Sleep mode, gives instant ON and avoids the overheat bug on the Brick by [@zhaofengli](https://github.com/zhaofengli)
- Battery Monitoring including history graph and time left prediction
- Scrolling animated titles for longer game names
- Updated and optimized build emulation cores
- Rumble strength fixed and is now variable as used by the games
- FBNeo Arcade screen rotation
- PAL Mode
- Next font supports CJK for JP/CN etc rom names
- Lot of other smaller fixes and optimizations

---

## Currently supported devices:
- Trimui Brick
- Trimui Brick Pro
- Trimui Smart Pro
- Trimui Smart Pro S
- Anbernic H700 Devices

---

## Installing

Follow our [installation guide](https://nextui.loveretro.games/usage/#getting-started).

---

## How to use

While in menu:

- Hold `start` and press `volume up` or `volume down` to adjust brighness
- Hold `select` and press `volume up` or `volume down` to adjust color temperature
- Short press `select` to open the game switcher

While in game:

- `Menu` opens in game options menu, adjusting controls, scaling and what not
- Hold `menu` and `select` at same time to open up the game switcher

Deep sleep:

When the device is left idle it will enter light sleep. Light sleep turns the screen off and pulses the leds 5 times.

After two minutes the device will go into deep sleep. The leds will turn completely off.

---

## What about X feature?

NextUI supports a variety of optional extras called Paks that are maintained by community members.

Our wiki has a [list of all available Paks](https://nextui.loveretro.games/pak-store/) that can be downloaded from our Pak Store straight on your device.

---

## Credits

- [josegonzalez](https://github.com/josegonzalez) for [minui-keyboard](https://github.com/josegonzalez/minui-keyboard)
- [knulli-cfw](https://github.com/knulli-cfw)/[Mikhailzrick](https://github.com/Mikhailzrick) for [fan-control-daemon](https://github.com/knulli-cfw/knulli-linux/commit/5c1b4724330b59cbe8af79d0a7f1dac86f1c221a)
- [Kenney](www.kenney.nl) for [Input Prompts](https://kenney.nl/assets/input-prompts)

---

## Play Retro's review of NextUI

[![image](https://github.com/user-attachments/assets/43217a30-e052-4f67-88a9-c4361f82e72f)](https://www.youtube.com/watch?v=m_7ObMHAFK4)

##  Retro Handhelds' coverage of NextUI

[![image](https://github.com/user-attachments/assets/5fd538ea-285b-46e9-add4-1ef99b2ee9e5)](https://www.youtube.com/watch?v=KlYVmtYDqRI)

---

## For all other info visit our website: [nextui.loveretro.games](https://nextui.loveretro.games)
