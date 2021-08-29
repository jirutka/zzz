prefix        := /usr/local
mandir        := $(prefix)/share/man
sbindir       := $(prefix)/sbin
sysconfdir    := /etc

MAN_FILES     := $(basename $(wildcard *.[1-9].adoc))

ASCIIDOCTOR   := asciidoctor
INSTALL       := install
LN_S          := ln -s
GIT           := git
SED           := sed

MAKEFILE_PATH  = $(lastword $(MAKEFILE_LIST))


#: Print list of targets (the default target).
help:
	@printf '%s\n\n' 'List of targets:'
	@$(SED) -En '/^#:.*/{ N; s/^#: (.*)\n([A-Za-z0-9_-]+).*/\2 \1/p }' $(MAKEFILE_PATH) \
		| while read label desc; do printf '%-15s %s\n' "$$label" "$$desc"; done

#: Build sources.
build: man

#: Convert man pages.
man: $(MAN_FILES)

#: Remove generated files.
clean:
	rm -f ./*.[1-9]

#: Install into $DESTDIR.
install: install-other install-man

#: Install everything except the man pages into $DESTDIR.
install-other:
	$(INSTALL) -D -m755 zzz "$(DESTDIR)$(sbindir)/zzz"
	$(LN_S) zzz "$(DESTDIR)$(sbindir)/ZZZ"
	$(INSTALL) -d -m755 "$(DESTDIR)$(sysconfdir)/zzz.d"

#: Install man pages into $DESTDIR/$mandir/man*/.
install-man: man
	$(INSTALL) -D -m644 -t $(DESTDIR)$(mandir)/man8/ $(filter %.8,$(MAN_FILES))

#: Uninstall from $DESTDIR.
uninstall:
	rm -f "$(DESTDIR)$(sbindir)/zzz"
	rm -f "$(DESTDIR)$(sbindir)/ZZZ"
	for name in $(MAN_FILES); do \
		rm -f "$(DESTDIR)$(mandir)/man$${name##*.}/$$name"; \
	done
	rmdir "$(DESTDIR)$(sysconfdir)/zzz.d" || true

#: Update version in the script and README.adoc to $VERSION.
bump-version:
	test -n "$(VERSION)"  # $$VERSION
	$(SED) -E -i "s/^(:version:).*/\1 $(VERSION)/" README.adoc
	$(SED) -E -i "s/^(readonly VERSION)=.*/\1='$(VERSION)'/" zzz

#: Bump version to $VERSION, create release commit and tag.
release: .check-git-clean | bump-version
	test -n "$(VERSION)"  # $$VERSION
	$(GIT) add .
	$(GIT) commit -m "Release version $(VERSION)"
	$(GIT) tag -s v$(VERSION) -m v$(VERSION)


.check-git-clean:
	@test -z "$(shell $(GIT) status --porcelain)" \
		|| { echo 'You have uncommitted changes!' >&2; exit 1; }

.PHONY: help man clean install install-other install-man uninstall bump-version release .check-git-clean


%.8: %.8.adoc
	$(ASCIIDOCTOR) -b manpage -o $@ $<
