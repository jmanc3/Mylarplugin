# Manually building Mylarplugin

Install the build dependencies first (see the [README](../README.md#install-build-dependencies)).

Download the latest release:

```bash
git clone --depth 1 https://github.com/jmanc3/Mylarplugin
cd Mylarplugin
make release
sudo make install
```

That will generate `mylar-desktop.so` inside of the `out` folder.

Now you can load the plugin on Hyprland by running `hyprctl plugin load /full/qualified/path/to/mylar-desktop.so`.

To auto start it on login with Hyprland, add the following to `$HOME/.config/hypr/hyprland.lua`

```lua
hl.exec_cmd("hyprctl plugin load /full/qualified/path/to/mylar-desktop.so")
```
