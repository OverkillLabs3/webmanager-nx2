# Building

Built with devkitPro and the Switch 2 devkit (libounce and the kaiHEN plugin runtime), on Windows (the devkitPro MSYS2 shell) or on Linux.

## Requirements

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `switch-dev` group
- the [ounce-devkit](https://github.com/ouncebrew/ounce-devkit), installed into the same devkitPro (run its `install.sh`)
- `python3` and `make`
- `DEVKITPRO` set (the default is `/opt/devkitpro`)
- a host `gcc` for the tests

## Steps

```sh
make                       # generates source/ui_html.h from source/ui.html and builds webfm.nso
make test                  # host tests of the request parsing and path handling
python3 tools/package.py   # the SD-card zip in dist/
```

`make` links against `-lkaihen_plugin -lounce_core` and converts the result with `elf2nso`.

## Layout

| Path | What |
|---|---|
| `source/main.c` | the plugin: sockets, threads and the file operations |
| `source/webcore.c` | request parsing, path cleaning, escaping: no console calls, tested on a PC |
| `source/ui.html` | the page; `tools/gen_ui.py` turns it into `source/ui_html.h` |
| `tests/test_webcore.c` | host tests |
| `tests/device_test.py` | end-to-end tests against a console |
| `tests/mock_server.py` | a stand-in for the API on a PC folder, to try the page in a browser |
| `tools/package.py` | builds the release zip |

## Changing the page

Edit `source/ui.html` and run `make`. The page is one self-contained file: it is linked into the plugin, so the plugin needs no files from the SD card.
