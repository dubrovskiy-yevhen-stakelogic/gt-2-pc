GT2 for Linux
=============

Start the game with:

    bin/gt2launcher

The launcher asks for a disc on first start and keeps discs and saves in
~/.local/share/GT2 (or $XDG_DATA_HOME/GT2). Set GT2_DATA_ROOT to use another
folder.

This build uses the system libraries instead of bundling them. Install the
runtime packages for your distribution:

    Debian / Ubuntu
        sudo apt install libsdl2-2.0-0 libgtk-3-0t64 libvulkan1
        (Debian 12, Ubuntu 22.04 and older: libgtk-3-0 instead of libgtk-3-0t64)

    Fedora
        sudo dnf install SDL2 gtk3 vulkan-loader

    Arch / SteamOS
        sudo pacman -S sdl2 gtk3 vulkan-icd-loader

A Vulkan-capable graphics driver is also required. The precompiled binaries are
built on Ubuntu 24.04, so older distributions may report a GLIBC version error;
use the Flatpak there instead.
