#!/bin/sh
# Adds ImMidi (with ImMidi Bridge and ImMidi Conversion Editor) to the application menu of the current
# user, or removes them again:
#
#   linux-desktop.sh install [PROGRAM]
#       Installs packaging/linux/org.immidi.ImMidi.desktop, with Exec pointing at PROGRAM (by default
#       bin/linux-Release/ImMidi in this repository), into ~/.local/share/applications, and the icon
#       into ~/.local/share/icons/hicolor/256x256/apps. When ImMidiBridge or ImMidiConvEditor is beside
#       PROGRAM, also its entry (org.immidi.ImMidiBridge.desktop, org.immidi.ImMidiConvEditor.desktop)
#       and icon. Run it again after moving the programs.
#   linux-desktop.sh uninstall
#       Removes these files.
#
# The desktop entries are also where Wayland desktops find the windows' icons when the compositor
# cannot take them from the windows (GNOME, KDE Plasma before 6.3): they match a window's app ID,
# org.immidi.ImMidi, org.immidi.ImMidiBridge or org.immidi.ImMidiConvEditor, with the entry's file
# name. X11 and KDE Plasma 6.3 or later show the icons without them.

set -e

root=$(cd "$(dirname "$0")/.." && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
apps=$data/applications
icons=$data/icons/hicolor/256x256/apps

# install_entry ID PROGRAM ARGUMENTS: the entry packaging/linux/ID.desktop starting PROGRAM by its
# full path (quoted as desktop entries quote arguments), and the icon ID.png.
install_entry() {
    cp "$root/packaging/linux/$1.png" "$icons/$1.png"
    sed "s|^Exec=.*|Exec=\"$2\"$3|" "$root/packaging/linux/$1.desktop" > "$apps/$1.desktop"
    echo "Installed $apps/$1.desktop"
    echo "      and $icons/$1.png"
}

case "$1" in
install)
    program=${2:-$root/bin/linux-Release/ImMidi}
    if [ ! -x "$program" ]; then
        echo "linux-desktop.sh: no program at $program (build ImMidi first, or name it)" >&2
        exit 1
    fi
    dir=$(cd "$(dirname "$program")" && pwd)
    mkdir -p "$apps" "$icons"
    install_entry org.immidi.ImMidi "$dir/$(basename "$program")" " %F"
    if [ -x "$dir/ImMidiBridge" ]; then install_entry org.immidi.ImMidiBridge "$dir/ImMidiBridge" ""; fi
    if [ -x "$dir/ImMidiConvEditor" ]; then install_entry org.immidi.ImMidiConvEditor "$dir/ImMidiConvEditor" ""; fi
    # Menus and file associations pick the entries up by themselves; these only make it quicker.
    if command -v update-desktop-database > /dev/null 2>&1; then update-desktop-database -q "$apps" || true; fi
    if command -v gtk-update-icon-cache > /dev/null 2>&1; then gtk-update-icon-cache -q -t "$data/icons/hicolor" || true; fi
    ;;
uninstall)
    rm -f "$apps/org.immidi.ImMidi.desktop" "$icons/org.immidi.ImMidi.png" \
          "$apps/org.immidi.ImMidiBridge.desktop" "$icons/org.immidi.ImMidiBridge.png" \
          "$apps/org.immidi.ImMidiConvEditor.desktop" "$icons/org.immidi.ImMidiConvEditor.png"
    if command -v update-desktop-database > /dev/null 2>&1; then update-desktop-database -q "$apps" || true; fi
    echo "Removed the entries and icons of ImMidi, ImMidi Bridge and ImMidi Conversion Editor from $apps and $icons"
    ;;
*)
    echo "usage: $0 install [PROGRAM] | uninstall" >&2
    exit 2
    ;;
esac
