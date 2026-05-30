# xwpe TODO

## v1.6.2 (current -- release preparation)

### Debian packaging
- [ ] Phase 6: Update Debian package for v1.6.2 (gbp import, changelog, sbuild)
- [ ] Add texinfo to Build-Depends, install xwpe.info via dh_installinfo
- [ ] Update debian/control Suggests (gcc, g++, gfortran, fpc, default-jdk, gdb)
- [ ] Add Closes: for Debian bugs in debian/changelog

### Verification
- [ ] Verify amagnasco search/replace bugs (#73, #77, #83, #87)

### Outreach
- [ ] Email to Dennis Payne: improvements summary + call for testers

### Done in v1.6.2
- [x] Complete SCREENCELL migration for popup save/restore
- [x] Compiler integration: gcc, g++, gfortran, fpc, javac (F9 + Alt-T/V)
- [x] Debugger resurrection: gdb with pty-based output capture
- [x] jdb (Java Debugger) as 5th debugger backend
- [x] ncurses mouse support for terminal emulators
- [x] GPM mouse migrated from Suraci direct to ncurses-managed
- [x] Fix cursor-to-Messages after successful compile
- [x] Fix menu bar not redrawn after Ctrl-F9 Run
- [x] Fix File Manager Tab cycling and panel scroll
- [x] Fix System Info dialog overflow (e_pr_str_wrap, e_pr_text)
- [x] Integrated Texinfo manual into help system
- [x] 33 automated pyte tests
- [x] 12-chapter Texinfo manual
- [x] Font recommendations documented
- [x] README, CHANGELOG, AUTHORS updated
- [x] Console tips (setfont, GPM) documented

## v1.7 (planned)

### Code quality / refactoring
- [ ] Doxygen documentation: add `/** */` headers to all public functions
- [ ] Generate API docs with `doxygen` (add Doxyfile to build system)
- [ ] Move e_pr_text() to we_e_aus.c as public function; adopt codebase-wide
- [ ] Extract reusable dialog helpers (e_pr_str_wrap) into shared module
- [ ] Audit all `e_pr_str()` call sites for potential overflow
- [ ] Replace hardcoded dialog coordinates with layout calculations
- [ ] Replace `char tmp[80]` local buffers with safe sizing

### Features
- [ ] DAP (Debug Adapter Protocol) client for multi-language debugging
- [ ] Add LaTeX compiler support and test it
- [ ] Restore historically supported languages: Perl, Python, COBOL
- [ ] Fix UTF-8 display in X11 mode (xwpe/xwe)
- [ ] Fix window resize not scaling editor windows proportionally

## v2.0 (future)

- [ ] DAP server mode: expose xwpe's debugger via DAP
- [ ] Consider panel library migration for popup management
