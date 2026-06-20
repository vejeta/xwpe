# homebrew-xwpe

[Homebrew](https://brew.sh) tap for [xwpe](https://codeberg.org/mendezr/xwpe),
a Borland-style IDE clone with LSP/DAP support that runs in both an X11 GUI
(`xwpe`) and a terminal (`wpe`).

This tap ships the **X11 build** -- `xwpe`, `xwe`, `wpe`, `we`. It needs
[XQuartz](https://www.xquartz.org); Homebrew formulas can't depend on casks,
so you install it once yourself before installing xwpe. If you only want the
terminal-mode editor, use the upstream
[`xwpe`](https://formulae.brew.sh/formula/xwpe) formula from `homebrew-core`
instead -- it's smaller and has no GUI dependencies.

## Install

This tap lives on Codeberg, so Homebrew needs the URL on first tap:

```sh
brew install --cask xquartz                                       # one-time
brew tap mendezr/xwpe https://codeberg.org/mendezr/homebrew-xwpe
brew install xwpe
```

To track upstream `main` instead of the tagged release:

```sh
brew install --HEAD xwpe
```

Open a fresh terminal afterwards so `wpe` / `xwpe` are on `PATH`, then:

```sh
xwpe path/to/file.c   # X11 GUI
wpe   path/to/file.c  # terminal UI
```

## Updating

```sh
brew update
brew upgrade xwpe
```

## License

The formula in this repository is released under the
[BSD 2-Clause License](LICENSE). xwpe itself is GPL-2.0-or-later; see its
[`COPYING`](https://codeberg.org/mendezr/xwpe/src/branch/main/COPYING).
