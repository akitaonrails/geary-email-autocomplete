#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

/* Geary private coupling: we only replace Geary's stock ContactEntryCompletion
 * (or, as a debug-logged fallback, composer/email entry widgets).  Everything
 * fails open so Geary keeps its built-in completion if this module cannot work.
 */

#define MODULE_DATA_KEY "geary-email-autocomplete-processed"
#define CONTEXT_DATA_KEY "geary-email-autocomplete-context"
#define SEEN_IMPORTANCE 30

typedef struct {
    char *email;
    char *name;
    char *formatted;
    int importance;
    int count;
} Contact;

typedef struct {
    GPtrArray *contacts; /* Contact* sorted by rank */
} Index;

typedef struct {
    GtkEntry *entry;
    GtkEntryCompletion *original;
    GtkEntryCompletion *completion;
    GtkListStore *store;
    gulong changed_id;
    gulong selected_id;
    gulong notify_completion_id;
    guint complete_idle_id;
} EntryContext;

enum { COL_TEXT, COL_EMAIL, N_COLS };

static gboolean opt_debug = FALSE;
static gboolean opt_include_noreply = FALSE;
static gboolean opt_allow_fallback = FALSE;
static int opt_max = 20;
static GMutex index_mutex;
static Index *global_index = NULL;
static gboolean index_ready = FALSE;
static gboolean index_started = FALSE;
static gulong map_hook_id = 0;

static gboolean scan_toplevels_idle(gpointer unused);
static gboolean complete_in_idle(gpointer user_data);

static void debug_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void debug_log(const char *fmt, ...) {
    if (!opt_debug) return;
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_printerr("[geary-email-autocomplete] %s\n", msg);
    g_log("geary-email-autocomplete", G_LOG_LEVEL_DEBUG, "%s", msg);
    g_free(msg);
}

static void debug_logv_legacy(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void G_GNUC_UNUSED debug_logv_legacy(const char *fmt, ...) {
    if (!opt_debug) return;
    va_list ap;
    va_start(ap, fmt);
    g_logv("geary-email-autocomplete", G_LOG_LEVEL_DEBUG, fmt, ap);
    va_end(ap);
}

static char *str_trim_dup(const char *s) {
    if (!s) return g_strdup("");
    char *copy = g_strdup(s);
    g_strstrip(copy);
    return copy;
}

static char *normalize_email(const char *email) {
    char *t = str_trim_dup(email);
    char *lt = strchr(t, '<');
    char *gt = strrchr(t, '>');
    if (lt && gt && gt > lt) {
        *gt = '\0';
        char *inside = str_trim_dup(lt + 1);
        g_free(t);
        t = inside;
    }
    char *lower = g_utf8_strdown(t, -1);
    g_free(t);
    return lower;
}

static gboolean is_valid_email(const char *email) {
    if (!email || !*email) return FALSE;
    if (strchr(email, ' ') || strchr(email, '\n') || strchr(email, '\r')) return FALSE;
    const char *at = strchr(email, '@');
    if (!at || at == email || !at[1] || strchr(at + 1, '@')) return FALSE;
    const char *dot = strrchr(at + 1, '.');
    return dot && dot > at + 1 && dot[1];
}

static gboolean is_noreply_address(const char *email) {
    if (!email) return FALSE;
    char *lower = g_utf8_strdown(email, -1);
    gboolean bad =
        g_str_has_prefix(lower, "no-reply@") || g_str_has_prefix(lower, "noreply@") ||
        g_str_has_prefix(lower, "donotreply@") || g_str_has_prefix(lower, "do-not-reply@") ||
        g_str_has_prefix(lower, "notification@") || g_str_has_prefix(lower, "notifications@") ||
        g_str_has_prefix(lower, "newsletter@") || g_str_has_prefix(lower, "news@") ||
        g_str_has_prefix(lower, "updates@") || g_str_has_prefix(lower, "marketing@") ||
        g_str_has_prefix(lower, "mailer-daemon@") || g_str_has_prefix(lower, "postmaster@") ||
        strstr(lower, "no-reply") || strstr(lower, "noreply") || strstr(lower, "do-not-reply") ||
        strstr(lower, "listserv") || strstr(lower, "mailman-bounces");
    g_free(lower);
    return bad;
}

static gboolean name_distinct_enough(const char *name, const char *email) {
    if (!name || !*name || !email) return FALSE;
    char *n = str_trim_dup(name);
    if (!*n) { g_free(n); return FALSE; }
    char *nl = g_utf8_strdown(n, -1);
    char *el = g_utf8_strdown(email, -1);
    char *at = strchr(el, '@');
    if (at) *at = '\0';
    gboolean ok = strcmp(nl, el) != 0 && !strchr(n, '@');
    g_free(n); g_free(nl); g_free(el);
    return ok;
}

static char *format_contact(const char *name, const char *email) {
    if (name_distinct_enough(name, email)) {
        char *n = str_trim_dup(name);
        gboolean needs_quotes = strpbrk(n, ",\"<>;:") != NULL;
        GString *display = g_string_new(NULL);
        if (needs_quotes) g_string_append_c(display, '"');
        for (char *p = n; *p; p++) {
            if (*p == '"' || *p == '\\') g_string_append_c(display, '\\');
            g_string_append_c(display, *p);
        }
        if (needs_quotes) g_string_append_c(display, '"');
        char *out = g_strdup_printf("%s <%s>", display->str, email);
        g_string_free(display, TRUE);
        g_free(n);
        return out;
    }
    return g_strdup(email ? email : "");
}

static void contact_free(gpointer p) {
    Contact *c = p;
    if (!c) return;
    g_free(c->email); g_free(c->name); g_free(c->formatted); g_free(c);
}

static void G_GNUC_UNUSED index_free(Index *idx) {
    if (!idx) return;
    g_ptr_array_free(idx->contacts, TRUE);
    g_free(idx);
}

static gint contact_rank_cmp(gconstpointer a, gconstpointer b) {
    const Contact *ca = *(Contact * const *)a;
    const Contact *cb = *(Contact * const *)b;
    if (ca->importance != cb->importance) return cb->importance - ca->importance;
    if (ca->count != cb->count) return cb->count - ca->count;
    return g_ascii_strcasecmp(ca->formatted, cb->formatted);
}

static Index *index_new(void) {
    Index *idx = g_new0(Index, 1);
    idx->contacts = g_ptr_array_new_with_free_func(contact_free);
    return idx;
}

static Contact *index_find(Index *idx, const char *email) {
    for (guint i = 0; idx && i < idx->contacts->len; i++) {
        Contact *c = g_ptr_array_index(idx->contacts, i);
        if (g_ascii_strcasecmp(c->email, email) == 0) return c;
    }
    return NULL;
}

static void index_add_contact(Index *idx, const char *email_raw, const char *name, int importance) {
    char *email = normalize_email(email_raw);
    if (!is_valid_email(email) || (!opt_include_noreply && is_noreply_address(email))) {
        g_free(email); return;
    }
    Contact *existing = index_find(idx, email);
    if (existing) {
        existing->count++;
        if (importance > existing->importance) existing->importance = importance;
        if ((!existing->name || !*existing->name) && name && *name) {
            g_free(existing->name); existing->name = str_trim_dup(name);
            g_free(existing->formatted); existing->formatted = format_contact(existing->name, existing->email);
        }
        g_free(email); return;
    }
    Contact *c = g_new0(Contact, 1);
    c->email = email;
    c->name = str_trim_dup(name);
    c->formatted = format_contact(c->name, c->email);
    c->importance = importance;
    c->count = 1;
    g_ptr_array_add(idx->contacts, c);
}

static gboolean contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return FALSE;
    char *h = g_utf8_strdown(haystack, -1);
    char *n = g_utf8_strdown(needle, -1);
    gboolean found = strstr(h, n) != NULL;
    g_free(h); g_free(n);
    return found;
}

static gboolean word_prefix_match(const char *text, const char *query) {
    if (!text || !query || !*query) return FALSE;
    char *tl = g_utf8_strdown(text, -1);
    char *ql = g_utf8_strdown(query, -1);
    gboolean match = g_str_has_prefix(tl, ql);
    for (char *p = tl; !match && *p; p++) {
        if (g_ascii_isspace(*p) || *p == '.' || *p == '-' || *p == '_' || *p == '<')
            match = g_str_has_prefix(p + 1, ql);
    }
    g_free(tl); g_free(ql);
    return match;
}

static gboolean contact_matches(Contact *c, const char *query) {
    if (!query || !*query) return FALSE;
    return word_prefix_match(c->email, query) || word_prefix_match(c->name, query) ||
           word_prefix_match(c->formatted, query) || contains_ci(c->email, query);
}

static GPtrArray *index_search(Index *idx, const char *query, int max_results) {
    GPtrArray *out = g_ptr_array_new();
    if (!idx) return out;
    char *q = str_trim_dup(query);
    if (!*q) { g_free(q); return out; }
    for (guint i = 0; i < idx->contacts->len && (int)out->len < max_results; i++) {
        Contact *c = g_ptr_array_index(idx->contacts, i);
        if (contact_matches(c, q)) g_ptr_array_add(out, c);
    }
    g_free(q);
    return out;
}

typedef struct { int start; int end; char *text; } Segment;

static gboolean is_separator_comma(const char *text, int byte_pos, gboolean *in_quote, gboolean *escaped, int *angle_depth) {
    char c = text[byte_pos];
    if (*escaped) { *escaped = FALSE; return FALSE; }
    if (c == '\\' && *in_quote) { *escaped = TRUE; return FALSE; }
    if (c == '"' && *angle_depth == 0) { *in_quote = !*in_quote; return FALSE; }
    if (!*in_quote) {
        if (c == '<') { (*angle_depth)++; return FALSE; }
        if (c == '>' && *angle_depth > 0) { (*angle_depth)--; return FALSE; }
        if (c == ',' && *angle_depth == 0) return TRUE;
    }
    return FALSE;
}

static void segment_bounds(const char *text, int byte_cursor, int *out_start, int *out_end) {
    int len = (int)strlen(text);
    int start = 0;
    gboolean in_quote = FALSE, escaped = FALSE;
    int angle_depth = 0;
    for (int i = 0; i < byte_cursor && i < len; i++) {
        if (is_separator_comma(text, i, &in_quote, &escaped, &angle_depth)) start = i + 1;
    }

    int end = len;
    in_quote = FALSE; escaped = FALSE; angle_depth = 0;
    for (int i = 0; i < len; i++) {
        if (is_separator_comma(text, i, &in_quote, &escaped, &angle_depth) && i >= byte_cursor) {
            end = i;
            break;
        }
    }

    while (start < end && g_ascii_isspace(text[start])) start++;
    while (end > start && g_ascii_isspace(text[end - 1])) end--;
    *out_start = start;
    *out_end = end;
}

static Segment current_segment(const char *text, int cursor_pos) {
    Segment s = {0, 0, NULL};
    text = text ? text : "";
    int chars = (int)g_utf8_strlen(text, -1);
    if (cursor_pos < 0 || cursor_pos > chars) cursor_pos = chars;
    int len = (int)strlen(text);
    int byte_cursor = (int)(g_utf8_offset_to_pointer(text, cursor_pos) - text);
    int start = 0, end = len;
    segment_bounds(text, byte_cursor, &start, &end);
    s.start = start; s.end = end; s.text = g_strndup(text + start, end - start);
    return s;
}

static char *replace_current_segment(const char *text, int cursor_pos, const char *replacement, int *new_cursor) {
    Segment s = current_segment(text ? text : "", cursor_pos);
    GString *g = g_string_new(NULL);
    g_string_append_len(g, text ? text : "", s.start);
    g_string_append(g, replacement ? replacement : "");
    int pos_bytes = (int)g->len;
    g_string_append(g, text ? text + s.end : "");
    if (new_cursor) *new_cursor = (int)g_utf8_pointer_to_offset(g->str, g->str + pos_bytes);
    g_free(s.text);
    return g_string_free(g, FALSE);
}

static int table_has_column(sqlite3 *db, const char *table, const char *column) {
    char *sql = g_strdup_printf("PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && g_ascii_strcasecmp((const char *)name, column) == 0) { found = 1; break; }
        }
    }
    sqlite3_finalize(st);
    g_free(sql);
    return found;
}

static void load_db(Index *idx, const char *path) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        debug_log("open failed for %s: %s", path, db ? sqlite3_errmsg(db) : "unknown");
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 1000);
    if (!table_has_column(db, "ContactTable", "highest_importance")) { sqlite3_close(db); return; }
    const char *email_col = table_has_column(db, "ContactTable", "email") ? "email" :
                            (table_has_column(db, "ContactTable", "normalized_email") ? "normalized_email" : NULL);
    const char *name_col = table_has_column(db, "ContactTable", "real_name") ? "real_name" :
                           (table_has_column(db, "ContactTable", "name") ? "name" : NULL);
    if (!email_col) { debug_log("ContactTable email column not found in %s", path); sqlite3_close(db); return; }
    char *sql = g_strdup_printf("SELECT %s, %s, highest_importance FROM ContactTable WHERE highest_importance >= %d",
                                email_col, name_col ? name_col : "''", SEEN_IMPORTANCE);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *email = (const char *)sqlite3_column_text(st, 0);
            const char *name = (const char *)sqlite3_column_text(st, 1);
            int importance = sqlite3_column_int(st, 2);
            index_add_contact(idx, email, name, importance);
        }
    } else {
        debug_log("query failed for %s: %s", path, sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    g_free(sql);
    sqlite3_close(db);
}

static void scan_root(Index *idx, const char *root) {
    DIR *dir = opendir(root);
    if (!dir) { debug_log("cannot open DB root %s: %s", root, g_strerror(errno)); return; }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char *path = g_build_filename(root, de->d_name, "geary.db", NULL);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) load_db(idx, path);
        g_free(path);
    }
    char *direct = g_build_filename(root, "geary.db", NULL);
    if (g_file_test(direct, G_FILE_TEST_IS_REGULAR)) load_db(idx, direct);
    g_free(direct);
    closedir(dir);
}

static Index *load_index_from_root(const char *root) {
    Index *idx = index_new();
    scan_root(idx, root);
    g_ptr_array_sort(idx->contacts, contact_rank_cmp);
    debug_log("loaded %u contacts from %s", idx->contacts->len, root);
    return idx;
}

static gpointer index_thread(gpointer unused) {
    (void)unused;
    const char *root_env = g_getenv("GEARY_EMAIL_AUTOCOMPLETE_DB_ROOT");
    char *root = root_env && *root_env ? g_strdup(root_env) : g_build_filename(g_get_home_dir(), ".local", "share", "geary", NULL);
    Index *idx = NULL;
    for (int attempt = 0; attempt < 3; attempt++) {
        idx = load_index_from_root(root);
        if (idx->contacts->len > 0 || attempt == 2) break;
        index_free(idx);
        idx = NULL;
        g_usleep(250000);
    }
    g_mutex_lock(&index_mutex);
    global_index = idx;
    index_ready = TRUE;
    g_mutex_unlock(&index_mutex);
    g_idle_add(scan_toplevels_idle, NULL);
    g_free(root);
    return NULL;
}

static void start_index_thread(void) {
    if (index_started) return;
    index_started = TRUE;
    GThread *thread = g_thread_new("geary-email-autocomplete-index", index_thread, NULL);
    g_thread_unref(thread);
}

static guint update_completion_model(EntryContext *ctx, gboolean show_popup) {
    gtk_list_store_clear(ctx->store);
    const char *text = gtk_entry_get_text(ctx->entry);
    int cursor = gtk_editable_get_position(GTK_EDITABLE(ctx->entry));
    Segment seg = current_segment(text, cursor);
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    g_mutex_lock(&index_mutex);
    GPtrArray *matches = index_search(global_index, seg.text, opt_max);
    for (guint i = 0; i < matches->len; i++) {
        Contact *c = g_ptr_array_index(matches, i);
        g_ptr_array_add(rows, g_strdup(c->formatted));
        g_ptr_array_add(rows, g_strdup(c->email));
    }
    g_ptr_array_free(matches, TRUE);
    g_mutex_unlock(&index_mutex);
    for (guint i = 0; i + 1 < rows->len; i += 2) {
        char *formatted = g_ptr_array_index(rows, i);
        char *email = g_ptr_array_index(rows, i + 1);
        GtkTreeIter it;
        gtk_list_store_append(ctx->store, &it);
        gtk_list_store_set(ctx->store, &it, COL_TEXT, formatted, COL_EMAIL, email, -1);
    }
    guint match_count = rows->len / 2;
    if (match_count > 0 && show_popup && ctx->complete_idle_id == 0) {
        ctx->complete_idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, complete_in_idle, ctx, NULL);
    }
    debug_log("completion update entry=%s query='%s' matches=%u",
              G_OBJECT_TYPE_NAME(ctx->entry), seg.text, match_count);
    g_ptr_array_free(rows, TRUE);
    g_free(seg.text);
    return match_count;
}

static void on_entry_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    update_completion_model((EntryContext *)user_data, TRUE);
}

static gboolean completion_match_all(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter, gpointer user_data) {
    (void)completion; (void)key; (void)iter; (void)user_data;
    return TRUE;
}

static gboolean complete_in_idle(gpointer user_data) {
    EntryContext *ctx = user_data;
    if (!ctx || !ctx->entry || g_object_get_data(G_OBJECT(ctx->entry), CONTEXT_DATA_KEY) != ctx) return G_SOURCE_REMOVE;
    ctx->complete_idle_id = 0;
    debug_log("showing completion popup for entry=%s", G_OBJECT_TYPE_NAME(ctx->entry));
    gtk_entry_completion_complete(ctx->completion);
    return G_SOURCE_REMOVE;
}

static gboolean on_match_selected(GtkEntryCompletion *completion, GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data) {
    (void)completion;
    EntryContext *ctx = user_data;
    char *value = NULL;
    gtk_tree_model_get(model, iter, COL_TEXT, &value, -1);
    if (!value) return TRUE;
    int cursor = gtk_editable_get_position(GTK_EDITABLE(ctx->entry));
    int new_cursor = 0;
    char *new_text = replace_current_segment(gtk_entry_get_text(ctx->entry), cursor, value, &new_cursor);
    g_signal_handler_block(ctx->entry, ctx->changed_id);
    gtk_entry_set_text(ctx->entry, new_text);
    gtk_editable_set_position(GTK_EDITABLE(ctx->entry), new_cursor);
    g_signal_handler_unblock(ctx->entry, ctx->changed_id);
    g_free(new_text); g_free(value);
    gtk_list_store_clear(ctx->store);
    return TRUE;
}

static void context_destroy(gpointer data) {
    EntryContext *ctx = data;
    if (!ctx) return;
    if (ctx->entry && ctx->changed_id && g_signal_handler_is_connected(ctx->entry, ctx->changed_id))
        g_signal_handler_disconnect(ctx->entry, ctx->changed_id);
    if (ctx->completion && ctx->selected_id && g_signal_handler_is_connected(ctx->completion, ctx->selected_id))
        g_signal_handler_disconnect(ctx->completion, ctx->selected_id);
    if (ctx->entry && ctx->notify_completion_id && g_signal_handler_is_connected(ctx->entry, ctx->notify_completion_id))
        g_signal_handler_disconnect(ctx->entry, ctx->notify_completion_id);
    if (ctx->complete_idle_id) g_source_remove(ctx->complete_idle_id);
    g_clear_object(&ctx->original);
    g_clear_object(&ctx->completion);
    g_clear_object(&ctx->store);
    g_free(ctx);
}

static void on_entry_completion_notify(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    EntryContext *ctx = user_data;
    if (!ctx || gtk_entry_get_completion(GTK_ENTRY(object)) == ctx->completion) return;
    debug_log("entry completion changed externally; disabling module for entry");
    g_object_set_data(G_OBJECT(object), MODULE_DATA_KEY, NULL);
    gpointer stolen = g_object_steal_data(G_OBJECT(object), CONTEXT_DATA_KEY);
    context_destroy(stolen);
}

static gboolean setup_entry(GtkEntry *entry, gboolean allow_fallback) {
    if (g_object_get_data(G_OBJECT(entry), MODULE_DATA_KEY)) return FALSE;

    g_mutex_lock(&index_mutex);
    gboolean ready = index_ready && global_index && global_index->contacts->len > 0;
    g_mutex_unlock(&index_mutex);
    if (!ready) { debug_log("index not ready; leaving stock completion"); return FALSE; }

    GtkEntryCompletion *orig = gtk_entry_get_completion(entry);
    const char *entry_type = G_OBJECT_TYPE_NAME(entry);
    const char *comp_type = orig ? G_OBJECT_TYPE_NAME(orig) : "(none)";
    gboolean preferred = orig && strstr(comp_type, "ContactEntryCompletion");
    gboolean fallback = opt_allow_fallback && allow_fallback && (strstr(entry_type, "Composer") || strstr(entry_type, "EmailEntry"));
    debug_log("mapped entry type=%s completion=%s", entry_type, comp_type);
    if (!preferred && !fallback) return FALSE;
    if (fallback && !preferred) debug_log("using fallback entry-type detection for %s", entry_type);

    g_object_set_data(G_OBJECT(entry), MODULE_DATA_KEY, GINT_TO_POINTER(1));

    EntryContext *ctx = g_new0(EntryContext, 1);
    ctx->entry = entry;
    ctx->original = orig ? g_object_ref(orig) : NULL;
    ctx->store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING);
    ctx->completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(ctx->completion, GTK_TREE_MODEL(ctx->store));
    gtk_entry_completion_set_text_column(ctx->completion, COL_TEXT);
    gtk_entry_completion_set_inline_completion(ctx->completion, FALSE);
    gtk_entry_completion_set_inline_selection(ctx->completion, TRUE);
    gtk_entry_completion_set_popup_completion(ctx->completion, TRUE);
    gtk_entry_completion_set_minimum_key_length(ctx->completion, 1);
    gtk_entry_completion_set_match_func(ctx->completion, completion_match_all, NULL, NULL);
    ctx->changed_id = g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), ctx);
    ctx->selected_id = g_signal_connect(ctx->completion, "match-selected", G_CALLBACK(on_match_selected), ctx);
    gtk_entry_set_completion(entry, ctx->completion);
    g_object_set_data_full(G_OBJECT(entry), CONTEXT_DATA_KEY, ctx, context_destroy);
    ctx->notify_completion_id = g_signal_connect(entry, "notify::completion", G_CALLBACK(on_entry_completion_notify), ctx);
    debug_log("attached module completion to entry type=%s original=%s", entry_type, comp_type);
    update_completion_model(ctx, FALSE);
    return TRUE;
}

static gboolean map_hook(GSignalInvocationHint *ihint, guint n_param_values, const GValue *param_values, gpointer data) {
    (void)ihint; (void)data;
    if (n_param_values < 1) return TRUE;
    gpointer obj = g_value_get_object(&param_values[0]);
    if (GTK_IS_ENTRY(obj)) setup_entry(GTK_ENTRY(obj), TRUE);
    return TRUE;
}

static void scan_widget_tree(GtkWidget *widget) {
    if (GTK_IS_ENTRY(widget)) setup_entry(GTK_ENTRY(widget), TRUE);
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *l = children; l; l = l->next) scan_widget_tree(GTK_WIDGET(l->data));
        g_list_free(children);
    }
}

static gboolean scan_toplevels_idle(gpointer unused) {
    (void)unused;
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *l = toplevels; l; l = l->next) scan_widget_tree(GTK_WIDGET(l->data));
    g_list_free(toplevels);
    return G_SOURCE_REMOVE;
}

G_MODULE_EXPORT void gtk_module_init(gint *argc, gchar ***argv) {
    (void)argc; (void)argv;
    opt_debug = g_strcmp0(g_getenv("GEARY_EMAIL_AUTOCOMPLETE_DEBUG"), "1") == 0;
    opt_include_noreply = g_strcmp0(g_getenv("GEARY_EMAIL_AUTOCOMPLETE_INCLUDE_NOREPLY"), "1") == 0;
    opt_allow_fallback = g_strcmp0(g_getenv("GEARY_EMAIL_AUTOCOMPLETE_ALLOW_FALLBACK"), "1") == 0;
    const char *max_env = g_getenv("GEARY_EMAIL_AUTOCOMPLETE_MAX");
    if (max_env && *max_env) opt_max = MAX(1, atoi(max_env));
    start_index_thread();
    gpointer widget_class = g_type_class_ref(GTK_TYPE_WIDGET);
    guint signal_id = g_signal_lookup("map", GTK_TYPE_WIDGET);
    if (signal_id) map_hook_id = g_signal_add_emission_hook(signal_id, 0, map_hook, NULL, NULL);
    if (widget_class) g_type_class_unref(widget_class);
    debug_log("module initialized hook=%lu max=%d include_noreply=%d allow_fallback=%d", map_hook_id, opt_max, opt_include_noreply, opt_allow_fallback);
}
