# xwpe TODO

## v1.6.2 (current)

- [ ] Phase 6: Update Debian package for v1.6.2
- [ ] Add texinfo to Debian Build-Depends, install xwpe.info
- [ ] Update debian/control Suggests (compilers, debuggers)
- [ ] Add Closes: for Debian bugs in debian/changelog
- [ ] Verify amagnasco search/replace bugs (#73, #77, #83, #87)
- [ ] Email to Dennis Payne: improvements summary + call for testers

## v1.7 (planned)

### Code quality / refactoring
- [ ] Doxygen documentation: add `/** */` headers to all public functions
- [ ] Generate API docs with `doxygen` (add Doxyfile to build system)
- [ ] Extract reusable dialog helpers (e_pr_str_wrap, e_pr_str_fit)
      into a shared module -- currently duplicated inline patterns
- [ ] Audit all `e_pr_str()` call sites for potential overflow
- [ ] Replace hardcoded dialog coordinates with layout calculations
- [ ] Replace `char tmp[80]` local buffers with safe sizing

### Features
- [ ] DAP (Debug Adapter Protocol) client for multi-language debugging
- [ ] Add LaTeX compiler support and test it
- [ ] Restore historically supported languages: Perl, Python, COBOL
- [ ] Fix UTF-8 display in X11 mode (xwpe/xwe)
- [ ] Fix window resize not scaling editor windows proportionally
- [ ] Investigate console/X11 font rendering quality

## v2.0 (future)

- [ ] DAP server mode: expose xwpe's debugger via DAP
- [ ] Consider panel library migration for popup management
