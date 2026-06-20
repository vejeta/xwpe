class Xwpe < Formula
  desc "Borland-style IDE clone with LSP/DAP support (terminal UI)"
  homepage "https://codeberg.org/mendezr/xwpe"
  url "https://codeberg.org/mendezr/xwpe/archive/v1.6.5.tar.gz"
  sha256 "8026ad8605dab876b04da5ddd76a165c691f6de1a0995394cffa14dc69f1df50"
  license "GPL-2.0-or-later"
  head "https://codeberg.org/mendezr/xwpe.git", branch: "main"

  depends_on "autoconf"   => :build
  depends_on "automake"   => :build
  depends_on "pkgconf" => :build
  depends_on "texinfo"    => :build

  depends_on "json-c"
  depends_on "libvterm"
  depends_on "ncurses"
  depends_on "zlib"

  def install
    # ncurses is keg-only on macOS; teach pkg-config where ncursesw.pc lives.
    ENV.prepend_path "PKG_CONFIG_PATH", Formula["ncurses"].opt_lib/"pkgconfig"

    system "autoreconf", "-fi"
    system "./configure",
           "--prefix=#{prefix}",
           "--without-x",
           "--without-gpm"
    system "make"
    system "make", "install"
  end

  def caveats
    <<~EOS
      This formula installs only the terminal-mode editor (`wpe`, `we`).
      For the X11 GUI build (`xwpe`, `xwe`) -- which requires XQuartz on
      macOS -- use the upstream tap instead:

          brew tap mendezr/xwpe https://codeberg.org/mendezr/homebrew-xwpe
          brew install xwpe
    EOS
  end

  test do
    %w[we wpe].each do |name|
      assert_path_exists bin/name, "missing entry point: #{name}"
    end
    assert_match(/executable/, shell_output("file -b #{bin}/we"))
  end
end
