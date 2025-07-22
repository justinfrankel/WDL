## Introduction

This project extends SWELL to support Linux input methods (IM), implementing pre-edit text rendering and candidate positioning.

All added code is contained within the `SWELL_SUPPORT_IM` macro definition.

If this project is useful to you, I'd appreciate a GitHub star or [Buy Me A Coffee](https://www.buymeacoffee.com/teenux).

### Main Feature

<img src="https://github.com/user-attachments/assets/5183b588-2d5d-4e21-bde5-5d4464a23da9" width="350">

- Type and commit text by using Fcitx or IBus
- Candidate box follows cursor position
- Auto-scrolls horizontally when typing exceeds box width(pre-edit text as well)
- Display pre-edit text


## Installation

### Pre-built file

> The pre-built BIN file only supports x86_64 systems.

If you installed REAPER manually, please backup your `libSwell.so` in the installation directory.

Then download the [release](https://github.com/wastee/REAPER-IM-SWELL/releases) version of `libSwell.so` and place it in your REAPER installation path.

Example commands (may require `sudo`):
```bash
mv libSwell.{so,so.bak}
cp libSwell.so /path/to/REAPER/
```

> Please replace with your actual REAPER installation path

If you're using Archlinux and installed REAPER via the package manager (`pacman`), your REAPER installation path is `/usr/lib/REAPER/`. 

Follow the same backup and replacement steps for `libSwell.so`.

### Build yourself

```bash
git clone https://github.com/wastee/REAPER-IM-SWELL.git
cd REAPER-IM-SWELL/WDL/swell/
make PRELOAD_GDK=1 SWELL_SUPPORT_GTK=1 SWELL_SUPPORT_IM=1
```
Then copy the new `libSwell.so` file to your REAPER installation path.

Example commands (may require `sudo`):
```bash
mv libSwell.{so,so.bak}
cp libSwell.so /path/to/REAPER/
```

## Usage Notes

### Environment variables

**IMPORTANT**: Make sure your system's input method (fcitx, ibus, etc.) is properly configured

Include the needed [environment variables settings](https://wiki.archlinux.org/title/Fcitx5#IM_modules).

### Show CJK characters

REAPER can't show CJK characters by default. However you can add a "patch" in your fontconfig file.

The fontconfig file is in `~/.config/fontconfig/font.conf`. If it doesn't exist, you need to create it.

Remember to replace the font name with your preferred choice. In my case, i'm using `MiSans VF`. 

Example file: https://gist.github.com/wastee/09d935c40387079bd27a03e196e7e62a#file-fonts-conf-L525

## Tested Environments

| OS  | Input Method | Status |
|--|---|---------|
| Archlinux | Fcitx5 | ✅ Working |
| Fedora | IBus | ✅ Working |

| Tested in REAPER       | State          |
|------------------------|----------------|
| action list            | ✅ Working     |
| find shortcut          | ❌ Exclude     |
| TCP & MCP             | ✅ Working     |
| Media explorer        | ✅ Working     |
| Load / Save Project   | ✅ Working     |
| MIDI Editor key rename | ✅ Working     |
| MIDI Editor track rename | ✅ Working |
| project settings note | ✅ Working     |
| FX window / FX rename | ✅ Working     |
| Settings              | ✅ Working     |
| IDE                   | ❌ Not work    |
| Video Processor       | ❌ Not work    |

## Known Issues

1. Depending on your resolution and DPI settings, the candidate window may not align properly on the Y-axis. You can modify [this line](https://github.com/wastee/REAPER-IM-SWELL/blob/main/WDL/swell/swell-im.cpp#L241) and rebuild `libSwell.so` yourself.
2. If your window auto-boots with REAPER, the candidate position may not work (no working m_oswindow). I currently don't have a solution for this issue yet.
3. Cursor in edit field won't move follow pre-edit back.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.
