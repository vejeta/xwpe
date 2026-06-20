# Homebrew packaging assets

This directory holds packaging material for distributing xwpe via
[Homebrew](https://brew.sh). It is **not** consumed by the build; nothing in
this tree is installed by `make install`.

## Layout

```
contrib/homebrew/
├── tap/                       # Drop-in contents for the mendezr/homebrew-xwpe repo
│   ├── Formula/xwpe.rb        # X11 build (depends on cairo, pango, libxft, xquartz cask)
│   ├── LICENSE                # BSD-2-Clause (the formula, not xwpe itself)
│   └── README.md              # User-facing tap docs
└── core/                      # Material for the Homebrew/homebrew-core PR
    ├── xwpe.rb                # Terminal-only build (no cask/X11 deps)
    └── PR_BODY.md             # Draft body for the homebrew-core pull request
```

## Releasing a new version

1. Cut and push a `vX.Y.Z` git tag.
2. Fetch the archive and compute the SHA256:

   ```sh
   curl -sL "https://codeberg.org/mendezr/xwpe/archive/vX.Y.Z.tar.gz" \
     | shasum -a 256
   ```

3. Update `url` + `sha256` in **both** `tap/Formula/xwpe.rb` and
   `core/xwpe.rb`.
4. Push the change to the `mendezr/homebrew-xwpe` tap (X11 build) and open a
   version-bump PR against `Homebrew/homebrew-core` (terminal build).

## Local validation

```sh
# Style (works on standalone files):
brew style contrib/homebrew/tap/Formula/xwpe.rb
brew style contrib/homebrew/core/xwpe.rb

# Tap formula (requires XQuartz + the X11 stack to be installable):
brew install --build-from-source ./contrib/homebrew/tap/Formula/xwpe.rb
brew test xwpe

# Core formula (terminal-only; safe to test anywhere):
brew install --build-from-source ./contrib/homebrew/core/xwpe.rb
brew test xwpe
```

`brew style` of `core/xwpe.rb` will report `Sorbet/StrictSigil` and
`Style/Documentation` -- both cops are disabled for `Formula/*.rb` in the real
`homebrew-core` repo's `.rubocop.yml`, so they go silent once the file is
moved into `Formula/x/xwpe.rb` inside a forked `homebrew-core` checkout.
