# geary-email-autocomplete

GTK3 module that widens Geary recipient autocomplete by reading Geary's contact
databases read-only and suggesting contacts with `highest_importance >= 30`
(`SEEN`). Geary already has built-in composer autocomplete, but stock Geary only
shows contacts at a higher threshold, so many legitimate seen senders never
appear.

## Build and test

```sh
make
make test
```

Run manually:

```sh
GTK_MODULES=$PWD/libgeary-email-autocomplete.so geary
```

Debug run:

```sh
make debug
```

Install:

```sh
sudo make install
```

## Install on Arch (AUR)

Pick the package that matches your Geary package, matching the pattern used by
`geary-hide-sidebar`:

```sh
yay -S geary-email-autocomplete       # source build; depends on geary-git
yay -S geary-email-autocomplete-bin   # prebuilt x86_64 .so; depends on geary
```

The package installs to `/usr/lib/geary-email-autocomplete/` and patches both
Geary launch paths:

- `/usr/share/applications/org.gnome.Geary.desktop`
- `/usr/share/dbus-1/services/org.gnome.Geary.service`

The patch appends this module to `GTK_MODULES` and preserves existing entries,
so it can be installed alongside `geary-hide-sidebar`. A pacman hook re-applies
the patch after Geary or geary-hide-sidebar package changes. Removing the package
removes only this module from `GTK_MODULES`.

Restart Geary after install/upgrade/removal:

```sh
geary --quit
```

## Loading with other GTK modules

Use colon-separated `GTK_MODULES` entries. For example, alongside
`geary-hide-sidebar`:

```sh
GTK_MODULES=/usr/lib/geary-hide-sidebar/libgeary-hide-sidebar.so:/path/to/libgeary-email-autocomplete.so geary
```

Remember Geary may be D-Bus activated; launchers and the D-Bus service need the
same environment if you want this loaded outside a terminal. The AUR package's
injector handles both paths automatically.

## Environment variables

- `GEARY_EMAIL_AUTOCOMPLETE_DEBUG=1` enables debug logging.
- `GEARY_EMAIL_AUTOCOMPLETE_MAX=20` caps suggestions.
- `GEARY_EMAIL_AUTOCOMPLETE_DB_ROOT=/path` overrides `~/.local/share/geary`.
- `GEARY_EMAIL_AUTOCOMPLETE_INCLUDE_NOREPLY=1` includes no-reply/list-ish senders.
- `GEARY_EMAIL_AUTOCOMPLETE_ALLOW_FALLBACK=1` allows a weaker widget-type fallback if Geary's stock completion type cannot be detected. Leave this off unless debug logs show the normal detection path is missing your recipient fields.

## Behavior and limitations

- Uses C for v1 and exports `gtk_module_init` as a GTK3 module.
- Reads SQLite databases read-only with a short busy timeout.
- Uses `ContactTable`, not raw messages, and never mutates Geary databases.
- Spam/junk avoidance relies on Geary's own contact-harvesting policy plus this module's conservative no-reply/list-style filters. It does not inspect folder history itself.
- Hooks mapped `GtkEntry` widgets, preferring Geary's private
  `ContactEntryCompletion` type. It falls back to composer/email entry type names
  only when `GEARY_EMAIL_AUTOCOMPLETE_ALLOW_FALLBACK=1` is set, with debug logging.
- Does not replace Geary's stock completion until the background index is ready.
- Private Geary widget names can change; if detection fails, Geary remains usable
  with its built-in completion.
- `make smoke` checks that `gtk_module_init` is exported and that the module can be loaded through `GTK_MODULES` under Xvfb.
