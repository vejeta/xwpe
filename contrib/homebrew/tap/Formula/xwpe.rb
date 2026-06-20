class Xwpe < Formula
  desc "Borland-style IDE clone with LSP/DAP support (X11 + terminal UI)"
  homepage "https://codeberg.org/mendezr/xwpe"
  url "https://codeberg.org/mendezr/xwpe/archive/v1.6.5.tar.gz"
  sha256 "8026ad8605dab876b04da5ddd76a165c691f6de1a0995394cffa14dc69f1df50"
  license "GPL-2.0-or-later"
  head "https://codeberg.org/mendezr/xwpe.git", branch: "main"

  depends_on "autoconf"   => :build
  depends_on "automake"   => :build
  depends_on "pkgconf" => :build
  depends_on "texinfo"    => :build

  depends_on "cairo"
  depends_on "freetype"
  depends_on "json-c"
  depends_on "libvterm"
  depends_on "libxft"
  depends_on :macos
  depends_on "ncurses"
  depends_on "pango"
  depends_on "zlib"

  # XQuartz cannot be expressed as a `depends_on` -- Homebrew formulas may not
  # depend on casks. We require it manually at install time and document the
  # need to install it (and to use `brew install --cask xquartz`) in `caveats`.

  def install
    odie <<~EOS unless File.directory?("/opt/X11/include/X11")
      xwpe requires XQuartz for its X11 GUI build, but /opt/X11 was not found.
      Install it first:

          brew install --cask xquartz

      and re-run `brew install xwpe`.
    EOS

    # XQuartz ships old fontconfig/cairo (2.14/1.17) that brewed pango rejects
    # (it wants >= 2.17 / 1.18), so brewed cairo/pango/fontconfig/freetype/
    # libxft pkg-config dirs must win the search ahead of XQuartz; XQuartz is
    # then appended to fill the X11 protocol gaps (x11.pc, xrender.pc, ...).
    # ncurses is keg-only and needed for ncursesw.
    %w[cairo pango fontconfig freetype libxft].each do |f|
      ENV.prepend_path "PKG_CONFIG_PATH", Formula[f].opt_lib/"pkgconfig"
    end
    ENV.prepend_path "PKG_CONFIG_PATH", Formula["ncurses"].opt_lib/"pkgconfig"
    ENV.append_path  "PKG_CONFIG_PATH", "/opt/X11/lib/pkgconfig"

    # libxft.pc declares freetype2 only as `Requires.private`, so superenv
    # drops its include path. Xft.h includes <ft2build.h> unconditionally, so
    # the freetype2 subdir must be on the compiler include path explicitly.
    ENV.append "CPPFLAGS", "-I#{Formula["freetype"].opt_include}/freetype2"

    # AC_PATH_XTRA puts -L/opt/X11/lib in X_LIBS so XQuartz also wins at link
    # time, loading its older libcairo/libfontconfig/libfreetype at runtime
    # against a pango built for the newer brewed ABIs -- which crashes
    # (SIGSEGV in cairo) on first launch. Prepend brewed lib dirs to LDFLAGS
    # so the linker (which honours -Wl,-search_paths_first) resolves the
    # brewed copies first; XQuartz still provides the pure-X11 libs (libSM,
    # ICE, Xext, Xrender) that have no brewed equivalent in the link.
    %w[libxft freetype fontconfig pango cairo].each do |f|
      ENV.prepend "LDFLAGS", "-L#{Formula[f].opt_lib}"
    end

    system "autoreconf", "-fi"
    system "./configure",
           "--prefix=#{prefix}",
           "--with-x",
           "--without-gpm",
           "--x-includes=/opt/X11/include",
           "--x-libraries=/opt/X11/lib"
    system "make"
    system "make", "install"
  end

  def caveats
    <<~EOS
      xwpe's X11 GUI (`xwpe`, `xwe`) needs XQuartz at runtime. If you have not
      installed it yet:

          brew install --cask xquartz

      Then log out and back in so DISPLAY is set, or start the server with
      `open -a XQuartz`. The terminal binaries (`wpe`, `we`) work without it.
    EOS
  end

  test do
    # All four entry points must exist and dispatch via argv[0].
    %w[we wpe xwpe xwe].each do |name|
      assert_path_exists bin/name, "missing entry point: #{name}"
    end
    # The binary refuses to run without a controlling terminal, so a no-op
    # invocation is enough to confirm linkage resolved (loader errors return
    # 127 / SIGTRAP; a non-zero exit from the editor itself is expected).
    assert_match(/Mach-O .* executable/, shell_output("file -b #{bin}/we"))
  end
end
