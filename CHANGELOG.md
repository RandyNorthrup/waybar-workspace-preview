# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-11-09

### Added

- Thumbnail of each Hyprland workspace in the tooltip of its `hyprland/workspaces` button, for any
  button label format (names, ids, icons).
- Windows on hidden workspaces are captured with `grim -T` (ext-foreign-toplevel-image-capture-source)
  and drawn where they sit on the monitor, over an optional wallpaper or background colour.
- A workspace is captured when you switch away from it and again when its button is hovered; the
  cached thumbnail shows at once and is replaced when the new one is ready.
- Optional list of the open windows (icon, app name, title) under the thumbnail (`"windows": true`).
- One capture and one Hyprland connection shared by all bars (multi-monitor), sharp on HiDPI outputs,
  rotated monitors supported; follows Hyprland restarts.
- `install.sh`, an Arch Linux `PKGBUILD`, unit tests and CI (gcc and clang, sanitizers).

[0.1.0]: https://github.com/RandyNorthrup/waybar-workspace-preview/releases/tag/v0.1.0
