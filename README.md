# kio-afp

KIO Worker for AFP (Apple Filing Protocol) using KDE Frameworks 6 and Qt 6,
built on top of the [afpfs-ng](https://github.com/Netatalk/afpfs-ng) Stateless Client Library.

Inspired by [kioslave_afp](https://github.com/Netatalk/kioslave_afp) for KDE 3 by Alex deVries,
however this is a complete rewrite and the two share no code.

## Build Requirements

- Qt 6.5+ (Core, Widgets)
- KDE Frameworks 6.20+ (KIO, I18n)
- ECM (Extra CMake Modules)
- CMake 3.20+
- afpfs-ng 0.9.4 or later

## Build & Install

```shell
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build .
sudo cmake --install .
```

## Runtime

Once installed, Dolphin, Konqueror, and other KIO clients will recognize `afp://` URLs.

The *kio_afp* plugin will be automatically loaded when you try to access an AFP server,
and will in turn spawn an *afpsld* daemon (from afpfs-ng) to handle the actual AFP communication in a separate process.

### Credentials Management

AFP usernames and passwords can be managed via KDE Wallet, and the plugin will prompt for credentials when needed.
Use the KDE Wallet Manager to manage stored credentials for AFP servers,
which will be securely accessed by the plugin when connecting to those servers.

You can also specify credentials directly in the URL (e.g., `afp://user:pass@host/`),
but using KDE Wallet is recommended for security.

## Development Notes

### Code Style

clang-format v20 is used for code formatting, with the WebKit style as the base and some customizations.

- The project uses a `.clang-format` file at the root to define the formatting rules.
- Run clang-format on modified files before committing to ensure consistent code style.

### Internationalization (i18n)

- i18n support wired via the KF6::I18n module and `KLocalizedString`.
    - Update or add translations by editing files under `po/` and run `xgettext`/`ki18n` tools to generate/refresh `.po`/`.pot` files.
    - The CMake install target will install translations present in `po/` when `make install` is run.
- Regenerate the POT file using the built-in CMake target:
    - `cmake --build build --target update-po` (or `make update-po`). This will regenerate the POT file and, if `msgmerge` is installed, automatically merge the updated POT into existing PO files under `po/` using gettext's fuzzy-matching rules.

## Copyright and License

Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>

This software is distributed under the terms of the GNU General Public License v2
