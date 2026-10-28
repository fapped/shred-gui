# TODO

## Done

- [x] Git repository (user `fapped`), MIT license
- [x] CMake + Qt 6 Widgets, `shredcore` library, QtTest tests (`ctest`)
- [x] Selecting / pasting / dropping a file or directory path
- [x] All `shred` options (`-f -n -s -u --remove=HOW -v -x -z --random-source`) + program choice
- [x] Recursive directory shredding and removing directories deepest first
- [x] Safe handling of symlinks, mount points and protected paths
- [x] Command preview, confirmation, progress, log, cancellation, remembered options
- [x] `.desktop` file and `cmake --install`
- [x] Whole project in English (GUI, source code, documentation)
- [x] Export/import of settings to/from JSON files (+ `--config`), remembered options kept
- [x] Presets `shred-delete` / `shred-overwrite` (`--preset`, `--list-presets`, *Load preset* menu), session only
- [x] Target list (many files/directories at once, pasting many paths, duplicates/nesting)
- [x] Dolphin context menu (*Shred* submenu), installed by CMake
- [x] Installed for the user into `~/.local` (2026-10-28); the older service menu removed

- [x] File names that are not valid UTF-8: kept exactly (command line, URLs, drag & drop, clipboard),
      passed to shred as bytes, shown with `\xNN`
- [x] Progress in the Plasma task manager and as a job in the notification area (D-Bus, no KF6),
      with a cancel button, a speed graph and a summary at the end
- [x] Consistent preset names: the same name (with the deciding flags, e.g. *-n 3 -z -u*) in
      *Load preset*, the options title and the Dolphin menu
- [x] "Close when finished" checkbox (remembered window preference, only after a full success)
- [x] Recently used settings files (*File → Recent settings*)

## To do

- [ ] Scan large directories in the background (scanning currently blocks the GUI), with cancellation
- [ ] Check the *Shred* submenu in Dolphin by hand (files: 2 entries, directories: 1 entry)
- [ ] Application icon (currently the `edit-delete` theme icon)
- [ ] Translations (`tr()` is used in the code, there are no `.ts` files yet)
- [ ] Warning about ineffectiveness on SSD / btrfs / ZFS (detect the target's file system type)
- [ ] Warning about hard links (`st_nlink > 1`) in the list of files
- [ ] File dialogs cannot return names that are not valid UTF-8 (Qt converts them) - a KIO/portal-based
      dialog or a custom one would be needed
- [ ] Optional: running as root (pkexec) for files without permissions
- [ ] Nautilus / Nemo integration (scripts or an extension)
- [ ] `.deb` package / AppImage, CI (GitHub Actions: build + tests)
