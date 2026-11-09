#!/bin/sh
# Builds waybar-workspace-preview and installs it, then prints the lines to add to your Waybar config.
#
#   ./install.sh            for your user: ~/.local/lib/waybar/waybar-workspace-preview.so
#   ./install.sh --system   for everyone:  /usr/local/lib/waybar/waybar-workspace-preview.so (uses sudo)
#   ./install.sh --uninstall [--system]
set -eu

here=$(cd "$(dirname "$0")" && pwd)
prefix="$HOME/.local"
sudo=""
action=install
for arg in "$@"; do
    case $arg in
        --system) prefix=/usr/local; sudo=sudo ;;
        --uninstall) action=uninstall ;;
        -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done
plugin="$prefix/lib/waybar/waybar-workspace-preview.so"

if [ "$action" = uninstall ]; then
    $sudo rm -f "$plugin"
    $sudo rm -rf "$prefix/share/doc/waybar-workspace-preview"
    echo "Removed $plugin. Take \"cffi/workspace-preview\" out of your Waybar config and restart Waybar."
    exit 0
fi

missing=""
for tool in meson ninja pkg-config cc; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
for lib in gtk+-3.0 json-glib-1.0 gio-unix-2.0; do
    pkg-config --exists "$lib" 2>/dev/null || missing="$missing $lib"
done
if [ -n "$missing" ]; then
    echo "Missing:$missing" >&2
    echo "Arch: sudo pacman -S --needed base-devel meson gtk3 json-glib grim" >&2
    echo "Debian/Ubuntu: sudo apt install build-essential meson pkg-config libgtk-3-dev libjson-glib-dev grim" >&2
    echo "Fedora: sudo dnf install gcc meson gtk3-devel json-glib-devel grim" >&2
    exit 1
fi
command -v grim >/dev/null 2>&1 || echo "Note: grim (1.5 or later) is needed at runtime to capture windows." >&2

build="$here/build-install"
meson setup "$build" "$here" --prefix="$prefix" --libdir=lib --buildtype=release -Dtests=false --wipe >/dev/null 2>&1 \
    || meson setup "$build" "$here" --prefix="$prefix" --libdir=lib --buildtype=release -Dtests=false >/dev/null
meson compile -C "$build" >/dev/null
$sudo meson install -C "$build" >/dev/null

cat <<MSG
Installed $plugin

Add it next to hyprland/workspaces in your Waybar config (module_path must be an absolute path):

    "modules-left": ["hyprland/workspaces", "cffi/workspace-preview"],
    "cffi/workspace-preview": {
        "module_path": "$plugin"
    },

Then restart Waybar (a reload with SIGUSR2 keeps an already-loaded copy of the plugin):

    pkill -x waybar; waybar &
MSG
