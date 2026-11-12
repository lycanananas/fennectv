## How to run

Both programs work with a running `data2g-host` (from Data2G): its
command port (8300) and KISS port (8100) by default, `--host`, `--cmd`
and `--kiss` for others.

### Linux: the AppImage

One file holds both programs: `tx` sends, `rx` receives. Make it
executable once, then name the program first; the rest are its options.

```
chmod +x FennecTV-@VERSION@-x86_64.AppImage

./FennecTV-@VERSION@-x86_64.AppImage tx --source film.mp4 --call YOURCALL   # send
./FennecTV-@VERSION@-x86_64.AppImage rx --play                              # receive and watch
./FennecTV-@VERSION@-x86_64.AppImage tx --help                              # every option, the modes
./FennecTV-@VERSION@-x86_64.AppImage                                        # this, in short
```

Or put `fennectv-tx` and `fennectv-rx` on your PATH once, and use them
like any program:

```
./FennecTV-@VERSION@-x86_64.AppImage --install       # links in ~/.local/bin
fennectv-tx --source film.mp4 --call YOURCALL
fennectv-rx --play
```

The links point at the AppImage file: after moving it, or with a newer
one, run `--install` again from there; `--uninstall` removes them. Any
x86-64 distribution with glibc 2.39 or newer (Ubuntu 24.04, Debian 13,
Fedora 40, Arch). The sender runs on a server without a desktop too; the
receiver needs a desktop's graphics libraries. Without FUSE (some
containers): `APPIMAGE_EXTRACT_AND_RUN=1 ./FennecTV-...`.

### Windows

Unzip `fennectv-@VERSION@-win64.zip` anywhere and run, in that folder:

```
fennectv-tx.exe --source film.mp4 --call YOURCALL
fennectv-rx.exe --play
```

### Sending

```
fennectv-tx --source film.mp4 --call YOURCALL                 # Z8k, VVC: 160x120, 5 fps
fennectv-tx --source film.mp4 --call YOURCALL --mode q3k      # Q3k: 96x72, 3 fps, lower S/N
fennectv-tx --source film.mp4 --call YOURCALL --loop          # the film again at its end
fennectv-tx --source /dev/video0 --input-format v4l2 --call YOURCALL   # a camera (Windows: dshow)
```

Each burst prints a line when it's coded, and another when it has gone
on the air (Data2G's own Sent counter is for ARQ connections and doesn't
count broadcast). Receivers follow whatever the sender chooses: they need
no options but `--play`.
