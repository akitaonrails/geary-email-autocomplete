#!/bin/sh
#
# Patch Geary launchers so they load geary-email-autocomplete via GTK_MODULES.
# The patch is idempotent and coexists with other GTK modules by appending or
# removing only this module from a colon-separated GTK_MODULES value.

set -eu

MODULE="/usr/lib/geary-email-autocomplete/libgeary-email-autocomplete.so"

TARGETS="
/usr/share/applications/org.gnome.Geary.desktop
/usr/share/dbus-1/services/org.gnome.Geary.service
"

patch_file() {
    action="$1"
    file="$2"
    [ -f "$file" ] || return 0
    tmp="$file.gea.tmp"

    awk -v action="$action" -v mod="$MODULE" '
        function first_token(s) {
            sub(/^[ \t]+/, "", s)
            sub(/[ \t].*$/, "", s)
            return s
        }

        function strip_empty_env(line,    rest, tok) {
            if (line ~ /^Exec=env[ \t]+/) {
                rest = line
                sub(/^Exec=env[ \t]+/, "", rest)
                tok = first_token(rest)
                if (tok !~ /=/) sub(/^Exec=env[ \t]+/, "Exec=", line)
            }
            return line
        }

        function update_modules(value, add,    n, parts, i, out, seen) {
            n = split(value, parts, ":")
            out = ""
            seen = 0
            for (i = 1; i <= n; i++) {
                if (parts[i] == "" || parts[i] == mod) {
                    if (parts[i] == mod) seen = 1
                    continue
                }
                out = out (out == "" ? "" : ":") parts[i]
            }
            if (add && !seen) out = out (out == "" ? "" : ":") mod
            return out
        }

        function apply_line(line,    p, rest, val, end, newval) {
            if (line !~ /^Exec=/) return line
            if (index(line, mod) > 0) return line

            p = index(line, "GTK_MODULES=")
            if (p > 0) {
                rest = substr(line, p + length("GTK_MODULES="))
                end = index(rest, " ")
                if (end == 0) {
                    val = rest
                    newval = update_modules(val, 1)
                    return substr(line, 1, p + length("GTK_MODULES=") - 1) newval
                }
                val = substr(rest, 1, end - 1)
                newval = update_modules(val, 1)
                return substr(line, 1, p + length("GTK_MODULES=") - 1) newval substr(rest, end)
            }

            if (line ~ /^Exec=env[ \t]+/) {
                sub(/^Exec=env[ \t]+/, "Exec=env GTK_MODULES=" mod " ", line)
            } else {
                sub(/^Exec=/, "Exec=env GTK_MODULES=" mod " ", line)
            }
            return line
        }

        function remove_line(line,    p, rest, val, end, newval, before, after) {
            if (line !~ /^Exec=/) return line
            p = index(line, "GTK_MODULES=")
            if (p == 0) return line

            rest = substr(line, p + length("GTK_MODULES="))
            end = index(rest, " ")
            if (end == 0) {
                val = rest
                after = ""
            } else {
                val = substr(rest, 1, end - 1)
                after = substr(rest, end)
            }
            newval = update_modules(val, 0)
            before = substr(line, 1, p - 1)
            if (newval == "") {
                line = before after
                gsub(/[ \t]+/, " ", line)
                sub(/[ \t]+$/, "", line)
                return strip_empty_env(line)
            }
            return before "GTK_MODULES=" newval after
        }

        /^Exec=/ {
            if (action == "apply") $0 = apply_line($0)
            else $0 = remove_line($0)
        }
        { print }
    ' "$file" > "$tmp"

    cat "$tmp" > "$file"
    rm -f "$tmp"
}

action="${1:-apply}"
case "$action" in
    apply|remove) ;;
    *) echo "usage: $0 {apply|remove}" >&2; exit 2 ;;
esac

for f in $TARGETS; do
    patch_file "$action" "$f"
done

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
fi
