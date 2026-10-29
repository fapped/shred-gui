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
      passed to shred as bytes, shown with `\xNN` (Dolphin converts such names, see below)
- [x] Progress in the Plasma task manager and as a job in the notification area (D-Bus, no KF6),
      with a cancel button, a speed graph and a summary at the end
- [x] Consistent preset names: the same name (with the deciding flags, e.g. *-n 3 -z -u*) in
      *Load preset*, the options title and the Dolphin menu
- [x] "Close when finished" checkbox (remembered window preference, only after a full success)
- [x] Recently used settings files (*File → Recent settings*)
- [x] The log starts every line with the time; the date is in the start line and after a change of day
- [x] *Shred* submenu in Dolphin checked by the user (files: 2 entries, directories: 1 entry)
- [x] Warnings about storage that does not overwrite in place (flash, copy-on-write, log-structured,
      network, overlay, FUSE) and about files with other hard links
- [x] Scanning in the background with the number of entries and cancellation
- [x] Files without permissions: no built-in elevation (pkexec) - the tool is started with sudo;
      the home directory of `SUDO_USER` stays protected
- [x] Speed graph in the application window: behind the progress bar (time on the x axis, the scale in
      the corner), `SpeedHistory` in the core library with tests
- [x] Built-in file dialog for names that are not valid UTF-8 (KDE's dialog shows them with U+FFFD and
      then reports that the file does not exist): the ⋮ menu next to *Add directory…*, optionally for
      all file dialogs of the sources; `FileListing` in the core library with tests
- [x] Dolphin (KIO) passes names that are not valid UTF-8 with U+FFFD in place of the bytes (context menu,
      drag & drop, clipboard; checked with *Copy* in Dolphin: `caf%EF%BF%BD`): such a path that does not exist
      is matched against the directory listing (each U+FFFD stands for one or more bytes) - one match: the
      exact name, several: only they are listed in the built-in dialog to choose from;
      `FileListing::replacedBytesMatches` with tests

## To do

- [ ] Application icon (currently the `edit-delete` theme icon)
- [ ] Translations (`tr()` is used in the code, there are no `.ts` files yet)
- [ ] Nautilus / Nemo integration (scripts or an extension)
- [ ] `.deb` package / AppImage, CI (Gitea Actions: build + tests)
