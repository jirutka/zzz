prefix        := $(or $(prefix),$(PREFIX),/usr/local)
mandir        := $(prefix)/share/man
sbindir       := $(prefix)/sbin
sysconfdir    := /etc

BUILD_DIR     := build
BIN_FILES     := zzz
MAN_FILES     := $(basename $(wildcard *.[1-9].adoc))

ASCIIDOCTOR   := asciidoctor
INSTALL       := install
LN_S          := ln -s
GIT           := git
SED           := sed

GIT_REV       := $(shell test -d .git && git describe --tags --match 'v*' 2>/dev/null)
ifneq ($(GIT_REV),)
  VERSION     := $(patsubst v%,%,$(GIT_REV))
endif

ifeq ($(DEBUG), 1)
  CFLAGS      := -g -DDEBUG
  CFLAGS      += -Wall -Wextra -pedantic
  ifeq ($(shell $(CC) --version | grep -q clang && echo clang), clang)
    CFLAGS    += -Weverything -Wno-vla
  endif
else
  CFLAGS      ?= -Os -DNDEBUG
endif

D              = $(BUILD_DIR)
MAKEFILE_PATH  = $(lastword $(MAKEFILE_LIST))


all: build

#: Print list of targets.
help:
	@printf '%s\n\n' 'List of targets:'
	@$(SED) -En '/^#:.*/{ N; s/^#: (.*)\n([A-Za-z0-9_-]+).*/\2 \1/p }' $(MAKEFILE_PATH) \
		| while read label desc; do printf '%-15s %s\n' "$$label" "$$desc"; done

.PHONY: help

#: Build sources (the default target).
build: build-exec build-man

#: Build executables.
build-exec: $(addprefix $(D)/,$(BIN_FILES))

#: Build man pages.
build-man: $(addprefix $(D)/,$(MAN_FILES))

#: Remove generated files.
clean:
	rm -rf "$(D)"

.PHONY: build build-exec build-man clean

#: Install into $DESTDIR.
install: install-conf install-exec install-man

#: Create directory for hooks in $DESTDIR/$sysconfdir.
install-conf:
	$(INSTALL) -d -m755 "$(DESTDIR)$(sysconfdir)/zzz.d"

#: Install executables into $DESTDIR/$sbindir/.
install-exec: build-exec
	$(INSTALL) -D -m755 $(D)/zzz "$(DESTDIR)$(sbindir)/zzz"
	$(LN_S) zzz "$(DESTDIR)$(sbindir)/ZZZ"

#: Install man pages into $DESTDIR/$mandir/man*/.
install-man: build-man
	$(INSTALL) -D -m644 -t $(DESTDIR)$(mandir)/man8/ $(addprefix $(D)/,$(filter %.8,$(MAN_FILES)))

#: Uninstall from $DESTDIR.
uninstall:
	rm -f "$(DESTDIR)$(sbindir)/zzz"
	rm -f "$(DESTDIR)$(sbindir)/ZZZ"
	for name in $(MAN_FILES); do \
		rm -f "$(DESTDIR)$(mandir)/man$${name##*.}/$$name"; \
	done
	rmdir "$(DESTDIR)$(sysconfdir)/zzz.d" || true

.PHONY: install install-conf install-exec install-man uninstall

#: Update version in zzz.c and README.adoc to $VERSION.
bump-version:
	test -n "$(VERSION)"  # $$VERSION
	$(SED) -E -i "s/^(:version:).*/\1 $(VERSION)/" README.adoc
	$(SED) -E -i "s/(#define\s+VERSION\s+).*/\1\"$(VERSION)\"/" zzz.c

#: Bump version to $VERSION, create release commit and tag.
release: .check-git-clean | bump-version
	test -n "$(VERSION)"  # $$VERSION
	$(GIT) add .
	$(GIT) commit -m "Release version $(VERSION)"
	$(GIT) tag -s v$(VERSION) -m v$(VERSION)

.PHONY: build-version release

$(D)/%.o: %.c | .builddir
	$(CC) $(CFLAGS) -std=c11 $(if $(VERSION),-DVERSION='"$(VERSION)"') -o $@ -c $<

$(D)/%: $(D)/%.o
	$(CC) $(LDFLAGS) -o $@ $<

$(D)/%.8: %.8.adoc | .builddir
	$(ASCIIDOCTOR) -b manpage -o $@ $<

.builddir:
	@mkdir -p "$(D)"

.check-git-clean:
	@test -z "$(shell $(GIT) status --porcelain)" \
		|| { echo 'You have uncommitted changes!' >&2; exit 1; }

.PHONY: .builddir .check-git-clean
