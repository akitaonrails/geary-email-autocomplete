CC ?= cc
PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 sqlite3)
PKG_LIBS := $(shell pkg-config --libs gtk+-3.0 sqlite3)
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
LDFLAGS ?=

MODULE := libgeary-email-autocomplete.so
TEST := test_geary_email_autocomplete
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib/geary-email-autocomplete

.PHONY: all test check smoke run debug install clean

all: $(MODULE)

$(MODULE): geary-email-autocomplete.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< $(PKG_CFLAGS) $(PKG_LIBS) $(LDFLAGS)

$(TEST): test_geary_email_autocomplete.c geary-email-autocomplete.c
	$(CC) $(CFLAGS) -o $@ test_geary_email_autocomplete.c $(PKG_CFLAGS) $(PKG_LIBS) $(LDFLAGS)

test check: $(TEST) $(MODULE)
	xvfb-run -a ./$(TEST)

smoke: $(TEST) $(MODULE)
	nm -D $(MODULE) | grep ' T gtk_module_init'
	xvfb-run -a env GTK_MODULES=$(CURDIR)/$(MODULE) GEARY_EMAIL_AUTOCOMPLETE_DEBUG=1 ./$(TEST) --quiet

run: $(MODULE)
	GTK_MODULES=$(CURDIR)/$(MODULE) geary

debug: $(MODULE)
	GEARY_EMAIL_AUTOCOMPLETE_DEBUG=1 GTK_MODULES=$(CURDIR)/$(MODULE) geary

install: $(MODULE)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(MODULE) $(DESTDIR)$(LIBDIR)/$(MODULE)

clean:
	rm -f $(MODULE) $(TEST) *.o
