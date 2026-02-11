# kio-afp

Frontend and KIO Worker for the AFP (Apple Filing Protocol) for KDE 6+,
built using the afpfs-ng Stateless Client Library.

Inspired by *kioslave_afp* for KDE 3 by Alex deVries, but was written from scratch and shares no code with the original.

## Build Requirements
- Qt 6.5+ (Core, Widgets)
- KDE Frameworks 6.20+ (KIO, I18n)
- ECM (Extra CMake Modules)
- CMake 3.20+
- libafpclient and libafpsl (via pkg-config) - shared libraries and development headers required for direct AFP client integration

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

Once installed, Dolphin, Konqueror, and other KDE file managers will recognize `afp://` URLs.

```shell
afp_connect --server localhost --share afp1 --user myuser --pass mypass mount ~/mnt
```

## Test

Open Dolphin and try connecting to an AFP server:

```shell
dolphin afp://localhost
```

Check logs for debug output:

```shell
QT_LOGGING_RULES="kio.*=true" dolphin afp://localhost
```

### CLI Testing

1. List volumes and directory

kioclient ls afp://localhost/
kioclient ls afp://localhost/afp1/

2. Download a file (get)

kioclient copy afp://localhost/afp1/testfile.txt .
cat /tmp/afp_get_test.txt

3. Upload a new file (put - create)

echo "test content" > /tmp/afp_put_test.txt
kioclient copy /tmp/afp_put_test.txt afp://localhost/afp1/afp_put_test.txt

4. Upload overwrite (put - overwrite with smaller file)

echo "short" > /tmp/afp_small.txt
kioclient copy --overwrite /tmp/afp_small.txt afp://localhost/afp1/afp_put_test.txt

5. Verify overwrite correctness

kioclient copy afp://localhost/afp1/afp_put_test.txt /tmp/afp_verify.txt
cat /tmp/afp_verify.txt  # Should be "short" only

6. Test mkdir, rename, delete

kioclient mkdir afp://localhost/afp1/testdir
kioclient move afp://localhost/afp1/afp_put_test.txt afp://localhost/afp1/testdir/moved.txt
kioclient remove afp://localhost/afp1/testdir/moved.txt
kioclient remove afp://localhost/afp1/testdir

## Development Notes

- **Entry point**: `kdemain()` function in [src/kafp_worker.cpp](src/kafp_worker.cpp) initializes the worker and enters dispatch loop.
- **Worker class**: `AfpWorker` inherits from `KIO::WorkerBase` and implements core operations (`get()`, `stat()`, `listDir()`).
- **UI**: [src/afploginwidget.h/cpp](src/afploginwidget.h) provides login dialog widgets (currently minimal stubs).

### Internationalization (i18n)

- i18n support wired via the KF6::I18n module and `KLocalizedString`.
    - Update or add translations by editing files under `po/` and run `xgettext`/`ki18n` tools to generate/refresh `.po`/`.pot` files.
    - The CMake install target will install translations present in `po/` when `make install` is run.
- Regenerate the POT file using the built-in CMake target:
    - `cmake --build build --target update-po` (or `make update-po`). This will regenerate the POT file and, if `msgmerge` is installed, automatically merge the updated POT into existing PO files under `po/` using gettext's fuzzy-matching rules.

## Copyright and License

Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>

This software is distributed under the terms of the GNU General Public License v2
