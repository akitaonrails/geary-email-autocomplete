#define GEARY_EMAIL_AUTOCOMPLETE_TEST 1
#include "geary-email-autocomplete.c"

typedef struct _FakeContactEntryCompletion {
    GtkEntryCompletion parent_instance;
} FakeContactEntryCompletion;

typedef struct _FakeContactEntryCompletionClass {
    GtkEntryCompletionClass parent_class;
} FakeContactEntryCompletionClass;

G_DEFINE_TYPE(FakeContactEntryCompletion, fake_contact_entry_completion, GTK_TYPE_ENTRY_COMPLETION)
static void fake_contact_entry_completion_class_init(FakeContactEntryCompletionClass *klass) { (void)klass; }
static void fake_contact_entry_completion_init(FakeContactEntryCompletion *self) { (void)self; }

static void test_segment_replace(void) {
    Segment s = current_segment("A <a@x.test>, bo", 17);
    g_assert_cmpstr(s.text, ==, "bo");
    g_assert_cmpint(s.start, ==, 14);
    g_free(s.text);
    int pos = 0;
    char *r = replace_current_segment("a@x.test, bo, c@x.test", 12, "Bob <bob@x.test>", &pos);
    g_assert_cmpstr(r, ==, "a@x.test, Bob <bob@x.test>, c@x.test");
    g_assert_cmpint(pos, ==, 26);
    g_free(r);

    s = current_segment("\"Doe, John\" <john@example.com>, ali", 33);
    g_assert_cmpstr(s.text, ==, "ali");
    g_free(s.text);

    r = replace_current_segment("\"Doe, John\" <john@example.com>, ali, z@example.com", 33,
                                "Alice Example <alice@example.com>", &pos);
    g_assert_cmpstr(r, ==, "\"Doe, John\" <john@example.com>, Alice Example <alice@example.com>, z@example.com");
    g_free(r);
}

static void test_noreply_filter(void) {
    g_assert_true(is_noreply_address("no-reply@example.com"));
    g_assert_true(is_noreply_address("notifications@example.com"));
    g_assert_true(is_noreply_address("newsletter@example.com"));
    g_assert_false(is_noreply_address("person@example.com"));
    g_assert_true(is_valid_email("person@example.com"));
    g_assert_false(is_valid_email("not-an-email"));
}

static void test_formatting(void) {
    char *f = format_contact("Alice Example", "alice@example.com");
    g_assert_cmpstr(f, ==, "Alice Example <alice@example.com>");
    g_free(f);
    f = format_contact("alice", "alice@example.com");
    g_assert_cmpstr(f, ==, "alice@example.com");
    g_free(f);
    f = format_contact("Example, Alice", "alice@example.com");
    g_assert_cmpstr(f, ==, "\"Example, Alice\" <alice@example.com>");
    g_free(f);
}

static void test_ranking_matching(void) {
    Index *idx = index_new();
    index_add_contact(idx, "bob@example.com", "Bob Low", 30);
    index_add_contact(idx, "alice@example.com", "Alice High", 100);
    index_add_contact(idx, "noreply@example.com", "No", 100);
    g_ptr_array_sort(idx->contacts, contact_rank_cmp);
    GPtrArray *m = index_search(idx, "ali", 20);
    g_assert_cmpuint(m->len, ==, 1);
    Contact *c = g_ptr_array_index(m, 0);
    g_assert_cmpstr(c->email, ==, "alice@example.com");
    g_ptr_array_free(m, TRUE);
    m = index_search(idx, "example", 20);
    g_assert_cmpuint(m->len, ==, 2);
    c = g_ptr_array_index(m, 0);
    g_assert_cmpstr(c->email, ==, "alice@example.com");
    g_ptr_array_free(m, TRUE);
    index_free(idx);
}

static void exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    g_assert_cmpint(rc, ==, SQLITE_OK);
    sqlite3_free(err);
}

static void test_sqlite_fixture(void) {
    char tmpl[] = "/tmp/geary-email-autocomplete-test-XXXXXX";
    char *root = g_mkdtemp(tmpl);
    g_assert_nonnull(root);
    char *acct = g_build_filename(root, "account", NULL);
    g_assert_cmpint(g_mkdir(acct, 0700), ==, 0);
    char *dbp = g_build_filename(acct, "geary.db", NULL);
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(dbp, &db), ==, SQLITE_OK);
    exec_sql(db, "CREATE TABLE ContactTable (email TEXT, real_name TEXT, highest_importance INTEGER)");
    exec_sql(db, "INSERT INTO ContactTable VALUES ('seen@example.com','Seen Sender',30)");
    exec_sql(db, "INSERT INTO ContactTable VALUES ('sent@example.com','Sent Person',100)");
    exec_sql(db, "INSERT INTO ContactTable VALUES ('hidden@example.com','Hidden',0)");
    exec_sql(db, "INSERT INTO ContactTable VALUES ('no-reply@example.com','Robot',100)");
    sqlite3_close(db);
    Index *idx = load_index_from_root(root);
    g_assert_cmpuint(idx->contacts->len, ==, 2);
    GPtrArray *m = index_search(idx, "seen", 20);
    g_assert_cmpuint(m->len, ==, 1);
    g_ptr_array_free(m, TRUE);
    index_free(idx);
    g_remove(dbp); g_rmdir(acct); g_rmdir(root);
    g_free(dbp); g_free(acct);
}

static void test_gtk_attach(void) {
    if (!gtk_init_check(NULL, NULL)) return;
    Index *idx = index_new();
    index_add_contact(idx, "alice@example.com", "Alice Example", 100);
    g_ptr_array_sort(idx->contacts, contact_rank_cmp);
    g_mutex_lock(&index_mutex);
    global_index = idx; index_ready = TRUE;
    g_mutex_unlock(&index_mutex);

    GtkWidget *entry = gtk_entry_new();
    g_assert_true(setup_entry(GTK_ENTRY(entry), TRUE) == FALSE); /* plain GtkEntry should not be fallback */
    gtk_widget_destroy(entry);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(win), entry);
    gtk_widget_show_all(win);
    GtkEntryCompletion *stock = g_object_new(fake_contact_entry_completion_get_type(), NULL);
    gtk_entry_set_completion(GTK_ENTRY(entry), stock);
    g_object_unref(stock);
    g_assert_true(setup_entry(GTK_ENTRY(entry), FALSE));
    EntryContext *ctx = g_object_get_data(G_OBJECT(entry), CONTEXT_DATA_KEY);
    g_assert_nonnull(ctx);
    gtk_entry_set_text(GTK_ENTRY(entry), "bob@example.com, ali, zed@example.com");
    gtk_editable_set_position(GTK_EDITABLE(entry), (int)g_utf8_strlen("bob@example.com, ali", -1));
    int cursor_before_popup = gtk_editable_get_position(GTK_EDITABLE(entry));
    g_assert_cmpuint(update_completion_model(ctx, TRUE), ==, 1);
    while (gtk_events_pending()) gtk_main_iteration();
    g_assert_cmpint(gtk_editable_get_position(GTK_EDITABLE(entry)), ==, cursor_before_popup);
    g_assert_nonnull(ctx->popover);
    g_assert_nonnull(ctx->listbox);
    g_assert_true(gtk_widget_get_visible(ctx->popover));
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(ctx->listbox, 0);
    g_assert_nonnull(row);
    on_suggestion_row_activated(ctx->listbox, row, ctx);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(entry)), ==,
                    "bob@example.com, Alice Example <alice@example.com>, zed@example.com");
    g_assert_false(gtk_widget_get_visible(ctx->popover));

    gtk_entry_set_text(GTK_ENTRY(entry), "bob@example.com, ali, zed@example.com");
    gtk_editable_set_position(GTK_EDITABLE(entry), (int)g_utf8_strlen("bob@example.com, ali", -1));
    g_assert_cmpuint(update_completion_model(ctx, TRUE), ==, 1);
    g_assert_true(gtk_widget_get_visible(ctx->popover));
    GdkEventKey escape = {0};
    escape.keyval = GDK_KEY_Escape;
    g_assert_true(on_entry_key_press(entry, &escape, ctx));
    g_assert_false(gtk_widget_get_visible(ctx->popover));

    gtk_entry_set_text(GTK_ENTRY(entry), "bob@example.com, ali, zed@example.com");
    gtk_editable_set_position(GTK_EDITABLE(entry), (int)g_utf8_strlen("bob@example.com, ali", -1));
    g_assert_cmpuint(update_completion_model(ctx, FALSE), ==, 1);
    GtkTreeIter iter;
    g_assert_true(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ctx->store), &iter));
    g_assert_true(on_match_selected(ctx->completion, GTK_TREE_MODEL(ctx->store), &iter, ctx));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(entry)), ==,
                    "bob@example.com, Alice Example <alice@example.com>, zed@example.com");

    GtkEntryCompletion *replacement = gtk_entry_completion_new();
    gtk_entry_set_completion(GTK_ENTRY(entry), replacement);
    g_object_unref(replacement);
    g_assert_null(g_object_get_data(G_OBJECT(entry), CONTEXT_DATA_KEY));
    gtk_widget_destroy(win);

    g_mutex_lock(&index_mutex);
    global_index = NULL; index_ready = FALSE;
    g_mutex_unlock(&index_mutex);
    index_free(idx);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    opt_include_noreply = FALSE;
    opt_max = 20;
    g_test_add_func("/segment/replace", test_segment_replace);
    g_test_add_func("/filter/noreply", test_noreply_filter);
    g_test_add_func("/format/contact", test_formatting);
    g_test_add_func("/index/ranking_matching", test_ranking_matching);
    g_test_add_func("/sqlite/fixture", test_sqlite_fixture);
    g_test_add_func("/gtk/attach", test_gtk_attach);
    return g_test_run();
}
