# kio-afp

Frontend and KIO Worker for the AFP (Apple Filing Protocol) for KDE 6+,
built for and depending on the afpfs-ng FUSE client.

This is a complete rewrite of *kioslave_afp* for KDE 3 by Alex deVries.

## Build Requirements
- Qt 6.5+ (Core, Widgets)
- KDE Frameworks 6.20+ (KIO, I18n)
- ECM (Extra CMake Modules)
- CMake 3.20+
 - libafpclient (via pkg-config) - development headers required for direct AFP client integration
 - afpfs-ng (mount tools like `mount_afpfs`) for helper-based mounts

## Build & Install

```shell
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build .
sudo cmake --install .
```

This installs:
- **Plugin**: `/usr/lib/plugins/kf6/kio/kio_afp.so`
- **Metadata**: `/usr/lib/plugins/kf6/kio/afp.json`

## Runtime

Once installed, Dolphin, Konqueror, and other KDE file managers will recognize `afp://` URLs. The worker currently returns "not implemented" for all operations and serves as a scaffold for full AFP support.

You can also launch the *afp_connect* app directly which can either operate as
a GUI for connecting to an AFP server, or used in CLI mode as a wrapper for *mount_afpfs*:

```shell
afp_connect --server localhost --share afp1 --user myuser --pass mypass mount ~/mnt
```

### Test

```shell
# Open Dolphin and try connecting to an AFP server
dolphin afp://localhost
```

Check logs for debug output:
```shell
QT_LOGGING_RULES="kio.*=true" dolphin afp://localhost
```

## Development Notes

- **Entry point**: `kdemain()` function in [src/kafp_worker.cpp](src/kafp_worker.cpp) initializes the worker and enters dispatch loop.
- **Worker class**: `AfpWorker` inherits from `KIO::WorkerBase` and implements core operations (`get()`, `stat()`, `listDir()`).
- **UI**: [src/afploginwidget.h/cpp](src/afploginwidget.h) provides login dialog widgets (currently minimal stubs).

⏳ **Next Steps:**
- Implement actual AFP protocol operations using a modern AFP client library (if available)
- Port connection/login logic from legacy `kafp.cpp`
- Integrate with `libafpclient` or equivalent

### Internationalization (i18n)

- i18n support wired via the KF6::I18n module and `KLocalizedString`.
    - Update or add translations by editing files under `po/` and run `xgettext`/`ki18n` tools to generate/refresh `.po`/`.pot` files.
    - The CMake install target will install translations present in `po/` when `make install` is run.
- Regenerate the POT file using the built-in CMake target:
    - `cmake --build build --target update-po` (or `make update-po`). This will regenerate the POT file and, if `msgmerge` is installed, automatically merge the updated POT into existing PO files under `po/` using gettext's fuzzy-matching rules.

## License

GNU General Public License v2

