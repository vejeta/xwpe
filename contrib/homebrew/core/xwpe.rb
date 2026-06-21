class Xwpe < Formula
  desc "Borland-style IDE clone with LSP/DAP support (terminal UI)"
  homepage "https://codeberg.org/mendezr/xwpe"
  url "https://codeberg.org/mendezr/xwpe/archive/v1.6.5.tar.gz"
  sha256 "8026ad8605dab876b04da5ddd76a165c691f6de1a0995394cffa14dc69f1df50"
  license "GPL-2.0-or-later"
  head "https://codeberg.org/mendezr/xwpe.git", branch: "main"

  depends_on "autoconf" => :build
  depends_on "automake" => :build
  depends_on "pkgconf" => :build
  # Required at build time: the manual is written in Texinfo and `makeinfo`
  # generates the `xwpe.info` pages that the editor opens from its own
  # Help -> Info menu (wired in via -DXWPE_INFODIR). The release tarball ships
  # only the `.texi` source, so the pages must be built here.
  depends_on "texinfo" => :build

  depends_on "json-c"
  depends_on "libvterm"
  depends_on "ncurses"
  depends_on "zlib"

  def install
    # ncurses is keg-only on macOS; teach pkg-config where ncursesw.pc lives.
    ENV.prepend_path "PKG_CONFIG_PATH", Formula["ncurses"].opt_lib/"pkgconfig"

    system "autoreconf", "--force", "--install", "--verbose"
    system "./configure", *std_configure_args, "--without-x", "--without-gpm"
    system "make"
    system "make", "install"
  end

  test do
    require "pty"

    # xwpe has no batch mode, so -- like the project's own pyte regression
    # suite -- the only way to test real behaviour is to drive the terminal UI.
    # Open a file holding a unique marker under a pseudo-terminal and confirm
    # the editor initialises ncurses, reads the file and paints that marker on
    # screen, then quit on Alt-X. Asserting on the marker (not just startup)
    # proves it actually loaded and rendered the buffer.
    marker = "XWPE_BREW_TEST_MARKER"
    (testpath/"hello.c").write <<~C
      // #{marker}
      int main(void) { return 0; }
    C
    ENV["TERM"] = "xterm-256color"

    output = +""
    PTY.spawn(bin/"wpe", "hello.c") do |r, w, pid|
      r.winsize = [24, 80]
      begin
        Timeout.timeout(30) do
          loop do
            chunk = r.read_nonblock(4096, exception: false)
            if chunk.is_a?(String)
              output << chunk
              break if output.include?(marker)
            else
              sleep 0.1
            end
          end
        end
        w.write "\ex" # Alt-X -> quit
      rescue Timeout::Error, Errno::EIO
        nil # fall through to the assertions on whatever was captured
      ensure
        begin
          Process.kill("TERM", pid)
        rescue Errno::ESRCH
          nil
        end
      end
    end

    assert_match(/\e\[/, output) # ncurses drew the screen
    assert_match marker, output  # ... and rendered the file we opened
  end
end
