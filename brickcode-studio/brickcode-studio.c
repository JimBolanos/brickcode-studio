/*
 * BrickCode Studio — one native GTK4 + C editor for two programming languages:
 *
 *   NQC  (RCX 1.0/2.0, Scout, Spybotics)  ->  runs `nqc`
 *   NXC  (NXT)                                          ->  runs `nbc`
 *
 * Works on Linux and macOS (Apple Silicon).
 *
 * Features:
 *   - Editor with syntax highlighting and a block-style snippet palette
 *   - Compile / Compile & Run / Compile & Download, with the first error
 *     line highlighted in the editor
 *   - RCX utilities (tower port detection, firmware, datalog, IR range...)
 *   - NXT: nbc only compiles; the IDE itself uploads and starts the program
 *     over USB with libusb - no vendor driver, NeXTTool or Python needed
 *   - NXT Explorer: lists files, battery and firmware over USB
 *   - Follows the active Omarchy theme on Linux (dark theme everywhere else)
 *
 * Build:
 *   cc $(pkg-config --cflags gtk4 libusb-1.0) -O2 -o brickcode-studio \
 *       brickcode-studio.c $(pkg-config --libs gtk4 libusb-1.0)
 *
 * BrickCode Studio is an independent project. It is not affiliated with, or endorsed
 * by, the LEGO Group. LEGO and MINDSTORMS are trademarks of the LEGO Group.
 *
 * Author: Jim Bolanos
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <libusb.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#define APP_NAME     "BrickCode Studio"
#define APP_VERSION  "2.1.1"

#pragma GCC diagnostic ignored "-Wformat-nonliteral"

/* ---------------------------------------------------------------------
 * Languages: English (keys in the code) and Spanish (table below).
 * Every visible string is looked up with T(); widgets created through the
 * wrappers keep their English key, so tr_apply() can re-translate them live.
 * --------------------------------------------------------------------- */
typedef enum { LANG_EN = 0, LANG_ES = 1 } Lang;
static Lang g_lang = LANG_EN;

static const char *ES[][2] = {
#include "i18n_es.inc"
    { NULL, NULL }
};

static const char *T(const char *en) {
    if (g_lang == LANG_EN || !en || !*en) return en;
    for (int i = 0; ES[i][0]; i++)
        if (strcmp(ES[i][0], en) == 0) return ES[i][1];
    return en;
}

static void tr_tag(gpointer obj, const char *name, const char *key) {
    g_object_set_data_full(G_OBJECT(obj), name, g_strdup(key), g_free);
}

static GtkWidget *tr_label_new(const char *en) {
    GtkWidget *w = (gtk_label_new)(T(en));
    tr_tag(w, "tr-en", en);
    return w;
}
static void tr_label_set(GtkLabel *l, const char *en) {
    (gtk_label_set_text)(l, T(en));
    tr_tag(l, "tr-en", en);
}
static GtkWidget *tr_button_new(const char *en) {
    GtkWidget *w = (gtk_button_new_with_label)(T(en));
    tr_tag(w, "tr-en", en);
    return w;
}
static void tr_tooltip(GtkWidget *w, const char *en) {
    (gtk_widget_set_tooltip_text)(w, T(en));
    tr_tag(w, "tr-tip", en);
}
static void tr_placeholder(GtkEntry *e, const char *en) {
    (gtk_entry_set_placeholder_text)(e, T(en));
    tr_tag(e, "tr-ph", en);
}

#define gtk_label_new(s)                   tr_label_new(s)
#define gtk_label_set_text(l, s)           tr_label_set(l, s)
#define gtk_button_new_with_label(s)       tr_button_new(s)
#define gtk_widget_set_tooltip_text(w, s)  tr_tooltip(w, s)
#define gtk_entry_set_placeholder_text(e, s) tr_placeholder(e, s)
#define gtk_window_set_title(w, s)         (gtk_window_set_title)(w, T(s))
#define gtk_file_dialog_set_title(d, s)    (gtk_file_dialog_set_title)(d, T(s))
#define gtk_file_filter_set_name(f, s)     (gtk_file_filter_set_name)(f, T(s))

/* Re-translate the whole widget tree after a language change. */
static void tr_apply(GtkWidget *w) {
    const char *k;
    if ((k = g_object_get_data(G_OBJECT(w), "tr-en"))) {
        if (GTK_IS_LABEL(w)) (gtk_label_set_text)(GTK_LABEL(w), T(k));
        else if (GTK_IS_BUTTON(w)) gtk_button_set_label(GTK_BUTTON(w), T(k));
    }
    if ((k = g_object_get_data(G_OBJECT(w), "tr-tip")))
        (gtk_widget_set_tooltip_text)(w, T(k));
    if ((k = g_object_get_data(G_OBJECT(w), "tr-ph")) && GTK_IS_ENTRY(w))
        (gtk_entry_set_placeholder_text)(GTK_ENTRY(w), T(k));
    if (GTK_IS_MENU_BUTTON(w)) {
        GtkPopover *p = gtk_menu_button_get_popover(GTK_MENU_BUTTON(w));
        if (p) tr_apply(GTK_WIDGET(p));
    }
    if (GTK_IS_POPOVER(w)) {
        GtkWidget *c = gtk_popover_get_child(GTK_POPOVER(w));
        if (c) tr_apply(c);
    }
    for (GtkWidget *c = gtk_widget_get_first_child(w); c;
         c = gtk_widget_get_next_sibling(c))
        tr_apply(c);
}

static Lang lang_from_env(void) {
    const char *l = g_getenv("LC_ALL");
    if (!l || !*l) l = g_getenv("LC_MESSAGES");
    if (!l || !*l) l = g_getenv("LANG");
    return (l && g_str_has_prefix(l, "es")) ? LANG_ES : LANG_EN;
}

#define NBC_CMD      "nbc"
#define NQC_CMD      "nqc"

#define TIMEOUT_DEFAULT  90
#define TIMEOUT_FIRMWARE 420

/* ---------------------------------------------------------------------
 * Languages / modes
 * --------------------------------------------------------------------- */

typedef enum { MODE_NQC = 0, MODE_NXC = 1 } Mode;

static const char *NXC_KEYWORDS[] = {
    "task", "sub", "void", "int", "long", "short", "float", "bool",
    "string", "byte", "unsigned", "signed", "const", "mutex", "struct",
    "typedef", "if", "else", "while", "for", "do", "switch", "case",
    "default", "break", "continue", "return", "true", "false", "start",
    "stop", "priority", "safecall", "inline", "asm", NULL
};

static const char *NXC_BUILTINS[] = {
    "OnFwd", "OnRev", "Off", "Coast", "Float", "Wait", "SetSensor",
    "SensorValue", "SensorUS", "TextOut", "NumOut", "ClearScreen",
    "PlayTone", "PlayToneEx", "PlaySound", "Acquire", "Release",
    "Random", "Sqrt", "abs", "sign", "SetOutput", "Sensor",
    "MotorRotationCount", "ResetRotationCount", NULL
};

#include "nqc_tables.inc"

/* Linux: the remote keeps the tower open (see the Remote section); close it before any nqc run. */
static void remote_tower_close(void);

static const char *NQC_TEMPLATE =
    "// A first program. Press F5, or the Compile & Run button.\n"
    "\n"
    "task main()\n"
    "{\n"
    "    SetPower(OUT_A + OUT_C, 7);\n"
    "\n"
    "    OnFwd(OUT_A + OUT_C);   // forward\n"
    "    Wait(200);              // 200 ticks = 2 seconds\n"
    "\n"
    "    OnRev(OUT_A + OUT_C);   // and back\n"
    "    Wait(200);\n"
    "\n"
    "    Off(OUT_A + OUT_C);\n"
    "    PlaySound(SOUND_UP);\n"
    "}\n";

static const char *NXC_TEMPLATE =
    "task main()\n"
    "{\n"
    "    TextOut(0, LCD_LINE1, \"Hello NXT\");\n"
    "    Wait(3000);\n"
    "}\n";

typedef struct {
    const char *name;        /* shown in the selector */
    const char *badge;       /* header icon text */
    const char *ext;         /* default file extension */
    const char *tool;        /* compiler command */
    const char *subtitle;    /* header subtitle */
    const char *template_;   /* starter program */
    const char **keywords;
    const char **builtins;
} ModeInfo;

static const ModeInfo MODES[] = {
    { "NQC  ·  RCX / Scout", "NQC", ".nqc", NQC_CMD,
      "NQC for the RCX and Scout  ·  compiled and sent by nqc",
      NULL, NQC_KEYWORDS, NQC_BUILTINS },
    { "NXC  ·  NXT", "NXC", ".nxc", NBC_CMD,
      "NXC for the NXT  ·  compiled by nbc, sent over USB",
      NULL, NXC_KEYWORDS, NXC_BUILTINS },
};

/* The -T targets, verbatim from nqc. RCX2 is nqc's own default. */
static const struct { const char *name; const char *label; const char *desc; } NQC_TARGETS[] = {
    { "RCX2",  "RCX2 (firm0328.lgo)", "RCX 2.0 — firmware 0328 or newer (nqc default)" },
    { "RCX",   "RCX (firm0309.lgo)",  "RCX 1.0 — older firmware" },
    { "Scout", "Scout",               "Scout" },
    { "Spy",   "Spy",                 "Spybotics" },
};

/* Where the RCX tower shows up on Linux. */
static const char *TOWER_DEVICES[] = {
    "/dev/usb/legousbtower0", "/dev/legousbtower0", "/dev/rcx", NULL
};
static const char *SERIAL_DEVICES[] = { "/dev/ttyUSB0", "/dev/ttyS0", NULL };
static const char *SHELL_RC_FILES[] = {
    ".config/fish/config.fish", ".config/fish/conf.d/rcx.fish", ".bashrc",
    ".bash_profile", ".zshrc", ".zshenv", ".profile", NULL
};

/* ---------------------------------------------------------------------
 * Application state
 * --------------------------------------------------------------------- */

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkTextView *editor;
    GtkTextBuffer *buffer;
    GtkTextView *output;
    GtkTextBuffer *output_buffer;
    char *current_file;
    Mode mode;

    GtkWidget *filename_label;
    GtkWidget *status_dot;
    GtkWidget *status_label;

    GtkTextView *lineno_view;
    GtkTextBuffer *lineno_buffer;

    /* Header: language selector */
    GtkWidget *mode_dropdown;
    GtkWidget *lang_dropdown;
    GtkWidget *badge_label;
    GtkWidget *subtitle_label;

    /* NXC settings */
    GtkWidget *nxc_settings;
    GtkWidget *port_dropdown;
    GtkWidget *bt_revealer;
    GtkWidget *bt_entry;

    /* NQC settings */
    GtkWidget *nqc_settings;
    GtkWidget *target_dropdown;
    GtkWidget *target_note;
    GtkWidget *slot_dropdown;

    /* Utilities (collapsible) */
    GtkWidget *utils_expander;
    GtkWidget *utils_nxc;
    GtkWidget *utils_nqc;
    GtkWidget *tower_entry;
    GtkWidget *fw_entry;
    GtkWidget *sleep_spin;

    GtkWidget *theme_dropdown;
    gboolean dark_theme;

    GtkWidget *cmd_entry;
    GtkWidget *cmd_prefix;
    GPtrArray *cmd_history;
    gint cmd_history_pos;

    GtkWidget *cancel_btn;
    GtkWidget *btn_run;
    GtkWidget *btn_upload;

    /* Running tool */
    GSubprocess *proc;
    gboolean running;
    guint run_id;
    gint64 deadline;
    int first_error;
    int last_seen_line;
    gboolean port_hint_shown;
    char *ok_msg;
    GtkWidget *dev_dot, *dev_label;   /* connected-brick indicator */
    GtkWidget *drawer;       /* right settings drawer (GtkRevealer) */
    GtkWidget *console_box;
    char *post_rxe;          /* NXT: file to upload after nbc succeeds */
    gboolean post_run;       /* ...and whether to start it afterwards */

    /* Omarchy theme */
    GHashTable *theme_colors;
    GtkCssProvider *css_provider;
    GtkCssProvider *theme_override_provider;
    GFileMonitor *theme_monitor;

    GtkTextTag *tag_keyword;
    GtkTextTag *tag_builtin;
    GtkTextTag *tag_preproc;
    GtkTextTag *tag_number;
    GtkTextTag *tag_string;
    GtkTextTag *tag_comment;
    GtkTextTag *tag_errline;

    /* Output taps: the firmware sheet reads nqc's output byte by byte */
    void (*on_chars)(const char *data, gsize n, gpointer user);
    void (*on_done)(int code, gpointer user);
    gpointer tap_data;

    /* Sidebar and header */
    GtkWidget *side_nqc_tools, *side_nxc_tools;
    GtkWidget *side_rcx_dot, *side_nxt_dot;
    GtkWidget *seg_btn[2];

    /* Unsaved changes */
    gboolean dirty;
    gboolean close_after_save;
} AppState;

static const char *mode_template(Mode m) {
    return m == MODE_NQC ? NQC_TEMPLATE : NXC_TEMPLATE;
}

/* ---------------------------------------------------------------------
 * Syntax highlighting
 * --------------------------------------------------------------------- */

static void apply_regex_tag(GtkTextBuffer *buffer, const char *text,
                             const char *pattern, GtkTextTag *tag,
                             GRegexCompileFlags flags) {
    GError *error = NULL;
    GRegex *regex = g_regex_new(pattern, flags, 0, &error);
    if (!regex) {
        g_printerr("Regex error: %s\n", error->message);
        g_error_free(error);
        return;
    }
    GMatchInfo *match_info;
    g_regex_match(regex, text, 0, &match_info);
    while (g_match_info_matches(match_info)) {
        gint start, end;
        g_match_info_fetch_pos(match_info, 0, &start, &end);
        /* GRegex gives byte offsets; GtkTextBuffer wants character offsets */
        glong cs = g_utf8_pointer_to_offset(text, text + start);
        glong ce = g_utf8_pointer_to_offset(text, text + end);
        GtkTextIter start_iter, end_iter;
        gtk_text_buffer_get_iter_at_offset(buffer, &start_iter, cs);
        gtk_text_buffer_get_iter_at_offset(buffer, &end_iter, ce);
        gtk_text_buffer_apply_tag(buffer, tag, &start_iter, &end_iter);
        g_match_info_next(match_info, NULL);
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);
}

static char *word_pattern(const char **words) {
    GString *s = g_string_new("\\b(");
    for (int i = 0; words[i]; i++) {
        if (i) g_string_append_c(s, '|');
        g_string_append(s, words[i]);
    }
    g_string_append(s, ")\\b");
    return g_string_free(s, FALSE);
}

static void highlight_buffer(AppState *st) {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(st->buffer, &start, &end);
    gtk_text_buffer_remove_tag(st->buffer, st->tag_keyword, &start, &end);
    gtk_text_buffer_remove_tag(st->buffer, st->tag_builtin, &start, &end);
    gtk_text_buffer_remove_tag(st->buffer, st->tag_preproc, &start, &end);
    gtk_text_buffer_remove_tag(st->buffer, st->tag_number, &start, &end);
    gtk_text_buffer_remove_tag(st->buffer, st->tag_string, &start, &end);
    gtk_text_buffer_remove_tag(st->buffer, st->tag_comment, &start, &end);
    gtk_text_buffer_remove_tag(st->buffer, st->tag_errline, &start, &end);

    char *text = gtk_text_buffer_get_text(st->buffer, &start, &end, FALSE);
    const ModeInfo *mi = &MODES[st->mode];

    char *kw = word_pattern(mi->keywords);
    apply_regex_tag(st->buffer, text, kw, st->tag_keyword, 0);
    g_free(kw);
    char *bi = word_pattern(mi->builtins);
    apply_regex_tag(st->buffer, text, bi, st->tag_builtin, 0);
    g_free(bi);

    apply_regex_tag(st->buffer, text, "^\\s*#\\w+", st->tag_preproc,
                     G_REGEX_MULTILINE);
    apply_regex_tag(st->buffer, text, "\\b\\d+\\.?\\d*\\b", st->tag_number, 0);
    apply_regex_tag(st->buffer, text, "\"([^\"\\\\]|\\\\.)*\"",
                     st->tag_string, 0);
    apply_regex_tag(st->buffer, text, "//[^\\n]*", st->tag_comment, 0);
    apply_regex_tag(st->buffer, text, "/\\*.*?\\*/", st->tag_comment,
                     G_REGEX_DOTALL);
    g_free(text);
}

static void update_line_numbers(AppState *st) {
    gint n = gtk_text_buffer_get_line_count(st->buffer);
    GString *s = g_string_new(NULL);
    for (gint i = 1; i <= n; i++) {
        if (i > 1) g_string_append_c(s, '\n');
        g_string_append_printf(s, "%d", i);
    }
    gtk_text_buffer_set_text(st->lineno_buffer, s->str, -1);
    g_string_free(s, TRUE);
}

static void update_title(AppState *st);

static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
    AppState *st = user_data;
    highlight_buffer(st);
    update_line_numbers(st);
    if (!st->dirty) {
        st->dirty = TRUE;
        update_title(st);
    }
}

/* ---------------------------------------------------------------------
 * Console / status / title
 * --------------------------------------------------------------------- */

static void log_output(AppState *st, const char *text, const char *tag_name) {
    text = T(text);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(st->output_buffer, &end);
    GtkTextTag *tag = tag_name ? gtk_text_tag_table_lookup(
        gtk_text_buffer_get_tag_table(st->output_buffer), tag_name) : NULL;
    if (tag)
        gtk_text_buffer_insert_with_tags(st->output_buffer, &end, text, -1, tag, NULL);
    else
        gtk_text_buffer_insert(st->output_buffer, &end, text, -1);
    gtk_text_buffer_get_end_iter(st->output_buffer, &end);
    gtk_text_buffer_insert(st->output_buffer, &end, "\n", -1);
    GtkTextMark *mark = gtk_text_buffer_get_insert(st->output_buffer);
    gtk_text_buffer_get_end_iter(st->output_buffer, &end);
    gtk_text_buffer_place_cursor(st->output_buffer, &end);
    gtk_text_view_scroll_mark_onscreen(st->output, mark);
}

static void set_status(AppState *st, const char *text, const char *dot_class) {
    gtk_label_set_text(GTK_LABEL(st->status_label), text);
    gtk_widget_remove_css_class(st->status_dot, "status-ok");
    gtk_widget_remove_css_class(st->status_dot, "status-busy");
    gtk_widget_remove_css_class(st->status_dot, "status-error");
    gtk_widget_add_css_class(st->status_dot, dot_class);
}

static void update_title(AppState *st) {
    char title[512];
    snprintf(title, sizeof(title), APP_NAME " — %s",
              st->current_file ? st->current_file : T("untitled"));
    gtk_window_set_title(GTK_WINDOW(st->window), title);
    char *base = st->current_file ? g_path_get_basename(st->current_file)
                                  : g_strdup("untitled");
    char *shown = g_strdup_printf("%s%s", base, st->dirty ? " •" : "");
    gtk_label_set_text(GTK_LABEL(st->filename_label), shown);
    g_free(shown);
    g_free(base);
}

/* ---------------------------------------------------------------------
 * Settings (remembered between runs)
 * --------------------------------------------------------------------- */

static char *settings_path(void) {
    return g_build_filename(g_get_user_config_dir(), "brickcode-studio",
                             "settings.ini", NULL);
}

static void settings_save(AppState *st) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "ide", "mode", st->mode);
    g_key_file_set_integer(kf, "ide", "lang", g_lang);
    g_key_file_set_integer(kf, "ide", "theme",
        gtk_drop_down_get_selected(GTK_DROP_DOWN(st->theme_dropdown)));
    g_key_file_set_integer(kf, "nqc", "target2",
        gtk_drop_down_get_selected(GTK_DROP_DOWN(st->target_dropdown)));
    g_key_file_set_integer(kf, "nqc", "slot",
        gtk_drop_down_get_selected(GTK_DROP_DOWN(st->slot_dropdown)));
    g_key_file_set_string(kf, "nqc", "tower_port",
        gtk_editable_get_text(GTK_EDITABLE(st->tower_entry)));
    g_key_file_set_string(kf, "nqc", "firmware",
        gtk_editable_get_text(GTK_EDITABLE(st->fw_entry)));
    g_key_file_set_integer(kf, "nxc", "port",
        gtk_drop_down_get_selected(GTK_DROP_DOWN(st->port_dropdown)));
    g_key_file_set_string(kf, "nxc", "bt_device",
        gtk_editable_get_text(GTK_EDITABLE(st->bt_entry)));

    char *path = settings_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
}

static int kf_int(GKeyFile *kf, const char *g, const char *k, int fallback) {
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, g, k, &e);
    if (e) { g_error_free(e); return fallback; }
    return v;
}

static char *kf_str(GKeyFile *kf, const char *g, const char *k) {
    char *v = g_key_file_get_string(kf, g, k, NULL);
    return v ? v : g_strdup("");
}

/* ---------------------------------------------------------------------
 * Mode switching
 * --------------------------------------------------------------------- */

static void apply_mode(AppState *st, Mode new_mode) {
    Mode old = st->mode;
    st->mode = new_mode;
    const ModeInfo *mi = &MODES[new_mode];

    gtk_label_set_text(GTK_LABEL(st->badge_label), mi->badge);
    gtk_label_set_text(GTK_LABEL(st->subtitle_label), mi->subtitle);
    gtk_label_set_text(GTK_LABEL(st->cmd_prefix), mi->tool);
    gtk_expander_set_expanded(GTK_EXPANDER(st->utils_expander), TRUE);
    gtk_label_set_text(GTK_LABEL(gtk_expander_get_label_widget(GTK_EXPANDER(st->utils_expander))),
        new_mode == MODE_NQC ? "Brick utilities (RCX / Scout)" : "NXT utilities");

    gtk_widget_set_visible(st->nqc_settings, new_mode == MODE_NQC);
    gtk_widget_set_visible(st->nxc_settings, new_mode == MODE_NXC);
    gtk_widget_set_visible(st->utils_nqc, new_mode == MODE_NQC);
    gtk_widget_set_visible(st->utils_nxc, new_mode == MODE_NXC);
    if (st->side_nqc_tools) gtk_widget_set_visible(st->side_nqc_tools, new_mode == MODE_NQC);
    if (st->side_nxc_tools) gtk_widget_set_visible(st->side_nxc_tools, new_mode == MODE_NXC);

    gtk_entry_set_placeholder_text(GTK_ENTRY(st->cmd_entry),
        new_mode == MODE_NQC
          ? "any nqc arguments, e.g. -firmware firm0328.lgo  ·  -sleep 5  ·  ↑↓ for history"
          : "any nbc arguments, e.g. -firmfast firm0332.lgo  ·  -pgm 3 -run  ·  ↑↓ for history");

    /* If the buffer still holds the other language's starter program,
     * swap in this language's starter. Never touch the user's own code. */
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(st->buffer, &s, &e);
    char *text = gtk_text_buffer_get_text(st->buffer, &s, &e, FALSE);
    if (old != new_mode && (g_strcmp0(text, mode_template(old)) == 0 || !*text)) {
        gtk_text_buffer_set_text(st->buffer, mode_template(new_mode), -1);
        st->dirty = FALSE;
    }
    g_free(text);

    highlight_buffer(st);
}

/* Change the selector, which triggers apply_mode through its signal. */
static void set_mode(AppState *st, Mode m) {
    if (st->mode == m) return;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->mode_dropdown), m);
}

/* Pick the language from a file name: .nqc/.nqh -> NQC, .nxc/.nxh -> NXC */
static void mode_from_path(AppState *st, const char *path) {
    if (g_str_has_suffix(path, ".nqc") || g_str_has_suffix(path, ".nqh"))
        set_mode(st, MODE_NQC);
    else if (g_str_has_suffix(path, ".nxc") || g_str_has_suffix(path, ".nxh"))
        set_mode(st, MODE_NXC);
}

static void mode_changed_cb(GObject *dropdown, GParamSpec *pspec,
                             gpointer user_data) {
    AppState *st = user_data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (sel < G_N_ELEMENTS(MODES)) apply_mode(st, (Mode)sel);
    if (sel < 2 && st->seg_btn[sel])
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->seg_btn[sel]), TRUE);
}

/* ---------------------------------------------------------------------
 * File handling
 * --------------------------------------------------------------------- */

static gboolean save_to_current_file(AppState *st) {
    if (!st->current_file) return FALSE;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(st->buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(st->buffer, &start, &end, FALSE);
    gboolean ok = g_file_set_contents(st->current_file, text, -1, NULL);
    g_free(text);
    if (ok) {
        st->dirty = FALSE;
        update_title(st);
    }
    return ok;
}

static GListModel *program_filters(void) {
    GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *f1 = gtk_file_filter_new();
    gtk_file_filter_set_name(f1, "NQC / NXC programs");
    gtk_file_filter_add_pattern(f1, "*.nqc");
    gtk_file_filter_add_pattern(f1, "*.nxc");
    gtk_file_filter_add_pattern(f1, "*.nqh");
    gtk_file_filter_add_pattern(f1, "*.nxh");
    g_list_store_append(store, f1);
    g_object_unref(f1);
    GtkFileFilter *f2 = gtk_file_filter_new();
    gtk_file_filter_set_name(f2, "All files");
    gtk_file_filter_add_pattern(f2, "*");
    g_list_store_append(store, f2);
    g_object_unref(f2);
    return G_LIST_MODEL(store);
}

static void new_file_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    gtk_text_buffer_set_text(st->buffer, mode_template(st->mode), -1);
    g_clear_pointer(&st->current_file, g_free);
    st->dirty = FALSE;
    update_title(st);
}

static void open_file_finish(GObject *source, GAsyncResult *res,
                              gpointer user_data) {
    AppState *st = user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &error);
    if (!file) {
        g_clear_error(&error);
        return;
    }
    char *path = g_file_get_path(file);
    char *contents = NULL;
    gsize len = 0;
    if (path && g_file_get_contents(path, &contents, &len, NULL)) {
        mode_from_path(st, path);          /* auto language from extension */
        gtk_text_buffer_set_text(st->buffer, contents, len);
        g_free(st->current_file);
        st->current_file = g_strdup(path);
        st->dirty = FALSE;
        update_title(st);
        g_free(contents);
    }
    g_free(path);
    g_object_unref(file);
}

static void open_file_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open NQC / NXC file");
    GListModel *filters = program_filters();
    gtk_file_dialog_set_filters(dialog, filters);
    g_object_unref(filters);
    gtk_file_dialog_open(dialog, GTK_WINDOW(st->window), NULL,
                          open_file_finish, st);
}

static void save_file_finish(GObject *source, GAsyncResult *res,
                              gpointer user_data) {
    AppState *st = user_data;
    GError *error = NULL;
    gboolean close_after = st->close_after_save;
    st->close_after_save = FALSE;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, &error);
    if (!file) {
        g_clear_error(&error);
        return;
    }
    g_free(st->current_file);
    st->current_file = g_file_get_path(file);
    if (st->current_file) mode_from_path(st, st->current_file);
    gboolean saved = save_to_current_file(st);
    update_title(st);
    g_object_unref(file);
    if (close_after && saved) gtk_window_close(GTK_WINDOW(st->window));
}

static void save_dialog(AppState *st, const char *title) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    char *name = st->current_file ? g_path_get_basename(st->current_file)
                                  : g_strdup_printf("program%s", MODES[st->mode].ext);
    gtk_file_dialog_set_initial_name(dialog, name);
    g_free(name);
    gtk_file_dialog_save(dialog, GTK_WINDOW(st->window), NULL,
                          save_file_finish, st);
}

static void save_file_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    if (st->current_file) {
        save_to_current_file(st);
        return;
    }
    save_dialog(st, "Save program");
}

static void save_as_cb(GtkButton *btn, gpointer user_data) {
    save_dialog(user_data, "Save as");
}

/* ---------------------------------------------------------------------
 * RCX tower port detection (NQC)
 * --------------------------------------------------------------------- */

static char *fish_unescape(const char *s) {
    /* fish stores universal variables with \xNN escapes (":" -> \x3a) */
    GString *out = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        unsigned int c;
        if (p[0] == '\\' && p[1] == 'x' && g_ascii_isxdigit(p[2]) &&
            g_ascii_isxdigit(p[3]) && sscanf(p + 2, "%2x", &c) == 1) {
            g_string_append_c(out, (char)c);
            p += 3;
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

/* Look for an exported RCX_PORT in the user's shell config files. A GUI
 * started from a launcher never reads these - which is exactly why nqc can
 * work in a terminal and fail here. */
static char *scan_shell_config_for_port(char **where) {
    GRegex *re = g_regex_new(
        "(?:^|\\s)(?:set\\s+(?:-[a-zA-Z]+\\s+)*RCX_PORT\\s+|export\\s+RCX_PORT\\s*=|RCX_PORT\\s*=)"
        "\\s*[\"']?([^\"'\\s#;]+)", G_REGEX_MULTILINE, 0, NULL);
    char *found = NULL;

    char *fv = g_build_filename(g_get_home_dir(), ".config/fish/fish_variables", NULL);
    char *contents = NULL;
    if (g_file_get_contents(fv, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i] && !found; i++) {
            if (!g_str_has_prefix(lines[i], "SETUVAR")) continue;
            const char *p = strstr(lines[i], "RCX_PORT:");
            if (p) {
                found = fish_unescape(p + strlen("RCX_PORT:"));
                g_strstrip(found);
                *where = g_strdup(fv);
            }
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(fv);

    for (int i = 0; !found && SHELL_RC_FILES[i]; i++) {
        char *full = g_build_filename(g_get_home_dir(), SHELL_RC_FILES[i], NULL);
        if (g_file_get_contents(full, &contents, NULL, NULL)) {
            gchar **lines = g_strsplit(contents, "\n", -1);
            for (int j = 0; lines[j] && !found; j++) {
                if (!strstr(lines[j], "RCX_PORT")) continue;
                const char *t = lines[j];
                while (*t == ' ' || *t == '\t') t++;
                if (*t == '#') continue;
                GMatchInfo *mi = NULL;
                if (g_regex_match(re, lines[j], 0, &mi)) {
                    found = g_match_info_fetch(mi, 1);
                    *where = g_strdup(full);
                }
                g_match_info_free(mi);
            }
            g_strfreev(lines);
            g_free(contents);
        }
        g_free(full);
    }
    if (re) g_regex_unref(re);
    return found;
}

/* Returns the port to use ("" = let nqc decide) and why. */
static char *detect_port(char **reason) {
    const char *env = g_getenv("RCX_PORT");
    if (env && *env) {
        *reason = g_strdup("from the RCX_PORT in this environment");
        return g_strdup(env);
    }
#ifdef __APPLE__
    /* macOS has no tower device node: nqc talks to the USB tower itself. */
    *reason = g_strdup("macOS: nqc opens the USB tower directly");
    return g_strdup("usb");
#endif
    char *where = NULL;
    char *found = scan_shell_config_for_port(&where);
    if (found) {
        *reason = g_strdup_printf("from RCX_PORT in %s", where);
        g_free(where);
        return found;
    }
    /* nqc only treats a port as USB when its name starts with "usb"; a plain
     * /dev/usb/legousbtower0 path is taken as a serial tower and fails. */
    for (int i = 0; TOWER_DEVICES[i]; i++) {
        if (g_file_test(TOWER_DEVICES[i], G_FILE_TEST_EXISTS)) {
            *reason = g_strdup("found the tower device node, nqc will use USB");
            return g_strdup("usb");
        }
    }
    *reason = g_strdup("USB tower (type a serial port by hand for a serial tower)");
    return g_strdup("usb");
}

static void port_diagnosis(AppState *st, const char *port) {
    char buf[600];
    if (port && *port && strcmp(port, "usb") != 0 && g_str_has_prefix(port, "/dev/")) {
        if (!g_file_test(port, G_FILE_TEST_EXISTS)) {
            snprintf(buf, sizeof(buf), T("   %s does not exist."), port);
            log_output(st, buf, "dim");
        } else if (access(port, R_OK | W_OK) != 0) {
            snprintf(buf, sizeof(buf),
                T("   %s exists but you do not have read/write access to it. "
                "That is a udev rule or a group you have not logged into yet."), port);
            log_output(st, buf, "dim");
        }
    }
    GString *tower = g_string_new(NULL), *all = g_string_new(NULL),
            *serial = g_string_new(NULL);
    for (int i = 0; TOWER_DEVICES[i]; i++) {
        if (all->len) g_string_append(all, ", ");
        g_string_append(all, TOWER_DEVICES[i]);
        if (g_file_test(TOWER_DEVICES[i], G_FILE_TEST_EXISTS)) {
            if (tower->len) g_string_append(tower, ", ");
            g_string_append(tower, TOWER_DEVICES[i]);
        }
    }
    for (int i = 0; SERIAL_DEVICES[i]; i++)
        if (g_file_test(SERIAL_DEVICES[i], G_FILE_TEST_EXISTS)) {
            if (serial->len) g_string_append(serial, ", ");
            g_string_append(serial, SERIAL_DEVICES[i]);
        }
    if (tower->len) {
        snprintf(buf, sizeof(buf), T("   Tower device nodes present: %s"), tower->str);
        log_output(st, buf, "dim");
    } else {
        snprintf(buf, sizeof(buf),
            T("   No tower device node found (%s). Is the tower plugged in, and is "
            "the legousbtower module loaded?"), all->str);
        log_output(st, buf, "dim");
        if (serial->len) {
            snprintf(buf, sizeof(buf),
                T("   Serial ports exist (%s) but those are only for a serial tower "
                "- set one by hand if that is what you have."), serial->str);
            log_output(st, buf, "dim");
        }
    }
    log_output(st, "   If nqc works in your terminal, 'echo $RCX_PORT' there tells "
                   "you the value to use.", "dim");
    g_string_free(tower, TRUE);
    g_string_free(all, TRUE);
    g_string_free(serial, TRUE);
}

static void detect_port_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    char *reason = NULL;
    char *found = detect_port(&reason);
    char buf[600];
    if (*found) {
        gtk_editable_set_text(GTK_EDITABLE(st->tower_entry), found);
        snprintf(buf, sizeof(buf), T("Tower port: %s (%s)"), found, T(reason));
        log_output(st, buf, "info");
    } else {
        log_output(st, "No tower port found automatically.", "err");
    }
    port_diagnosis(st, found);
    g_free(found);
    g_free(reason);
}

/* ---------------------------------------------------------------------
 * Running nqc / nbc (asynchronous, with Cancel and a watchdog timeout)
 * --------------------------------------------------------------------- */

static void set_busy(AppState *st, gboolean busy) {
    gtk_widget_set_sensitive(st->cancel_btn, busy);
    gtk_widget_set_sensitive(st->btn_run, !busy);
    gtk_widget_set_sensitive(st->btn_upload, !busy);
}

static void goto_line(AppState *st, int lineno) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(st->buffer, &start, MAX(0, lineno - 1));
    end = start;
    if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
    GtkTextIter b, e;
    gtk_text_buffer_get_bounds(st->buffer, &b, &e);
    gtk_text_buffer_remove_tag(st->buffer, st->tag_errline, &b, &e);
    gtk_text_buffer_apply_tag(st->buffer, st->tag_errline, &start, &end);
    gtk_text_buffer_place_cursor(st->buffer, &start);
    gtk_text_view_scroll_to_iter(st->editor, &start, 0.2, FALSE, 0, 0);
}

static gboolean goto_line_idle(gpointer data) {
    AppState *st = g_object_get_data(G_OBJECT(data), "st");
    goto_line(st, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(data), "line")));
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}

static void queue_goto_line(AppState *st, int lineno) {
    GObject *carrier = g_object_new(G_TYPE_OBJECT, NULL);
    g_object_set_data(carrier, "st", st);
    g_object_set_data(carrier, "line", GINT_TO_POINTER(lineno));
    g_timeout_add(80, goto_line_idle, carrier);
}

static void scan_nqc_line(AppState *st, const char *line) {
    /* nqc reports problems as  file.nqc:12: Error: ... */
    GRegex *re = g_regex_new("^[^:]+:(\\d+):\\s*(Error|Warning)",
                              G_REGEX_CASELESS, 0, NULL);
    GMatchInfo *mi = NULL;
    if (re && g_regex_match(re, line, 0, &mi) && st->first_error == 0) {
        char *sev = g_match_info_fetch(mi, 2);
        if (g_ascii_strcasecmp(sev, "error") == 0) {
            char *num = g_match_info_fetch(mi, 1);
            st->first_error = atoi(num);
            g_free(num);
            queue_goto_line(st, st->first_error);
        }
        g_free(sev);
    }
    g_match_info_free(mi);
    if (re) g_regex_unref(re);

    /* nqc could not talk to the tower: explain, once. */
    if (!st->port_hint_shown &&
        (strcasestr(line, "could not open serial port") ||
         strcasestr(line, "could not open usb"))) {
        st->port_hint_shown = TRUE;
        const char *port = gtk_editable_get_text(GTK_EDITABLE(st->tower_entry));
        char buf[600];
        log_output(st, "", "dim");
        log_output(st, "nqc reached the brick step but could not open the tower.", "err");
        if (!port || !*port)
            log_output(st, "No port is set here, so nqc used its built-in default "
                           "(usually /dev/rcx).", "err");
        else {
            snprintf(buf, sizeof(buf), T("Tried port: %s"), port);
            log_output(st, buf, "err");
        }
        port_diagnosis(st, port);
        char *reason = NULL;
        char *found = detect_port(&reason);
        if (*found && g_strcmp0(found, port) != 0) {
            snprintf(buf, sizeof(buf),
                T("Suggestion: set the Tower port to %s (%s) under Brick utilities."),
                found, T(reason));
            log_output(st, buf, "info");
        }
        g_free(found);
        g_free(reason);
    }
}

static void scan_nbc_line(AppState *st, const char *line) {
    /* nbc prints  File "x.nxc" ; line 12  and then  # Error: ... */
    const char *p = strcasestr(line, "line ");
    if (p && strchr(line, ';')) {
        int n = atoi(p + 5);
        if (n > 0) st->last_seen_line = n;
    }
    if (st->first_error == 0 && st->last_seen_line > 0 &&
        strstr(line, "# Error")) {
        st->first_error = st->last_seen_line;
        queue_goto_line(st, st->first_error);
    }
}

typedef struct {
    AppState *st;
    GInputStream *in;
    GString *buf;
    char chunk[512];
} Pump;

static void pump_line(Pump *p, const char *line) {
    char *clean = g_utf8_make_valid(line, -1);
    g_strchomp(clean);
    log_output(p->st, clean, NULL);
    if (p->st->mode == MODE_NQC) scan_nqc_line(p->st, clean);
    else scan_nbc_line(p->st, clean);
    g_free(clean);
}

static void pump_next(GObject *src, GAsyncResult *res, gpointer user_data);

static void pump_read(Pump *p) {
    g_input_stream_read_async(p->in, p->chunk, sizeof(p->chunk),
                              G_PRIORITY_DEFAULT, NULL, pump_next, p);
}

static void pump_next(GObject *src, GAsyncResult *res, gpointer user_data) {
    Pump *p = user_data;
    GError *error = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &error);
    g_clear_error(&error);
    if (n <= 0) {
        if (p->buf->len) pump_line(p, p->buf->str);
        g_object_unref(p->in);
        g_string_free(p->buf, TRUE);
        g_free(p);
        return;
    }
    if (p->st->on_chars) p->st->on_chars(p->chunk, (gsize)n, p->st->tap_data);
    g_string_append_len(p->buf, p->chunk, n);
    char *nl;
    while ((nl = memchr(p->buf->str, '\n', p->buf->len)) != NULL) {
        gsize used = (gsize)(nl - p->buf->str) + 1;
        *nl = 0;
        pump_line(p, p->buf->str);
        g_string_erase(p->buf, 0, used);
    }
    pump_read(p);
}

static void start_pump(AppState *st, GInputStream *in) {
    if (!in) return;
    Pump *p = g_new0(Pump, 1);
    p->st = st;
    p->in = g_object_ref(in);
    p->buf = g_string_new(NULL);
    pump_read(p);
}

typedef struct {
    AppState *st;
    guint run_id;
    int code;
} ExitCtx;

static gboolean nxt_upload(const char *path, gboolean run, char **err);

static gboolean finish_run_idle(gpointer data) {
    ExitCtx *x = data;
    AppState *st = x->st;
    if (st->running && st->run_id == x->run_id) {
        st->running = FALSE;
        /* NXT: nbc only compiled. Now send the .rxe ourselves over USB. */
        if (x->code == 0 && st->post_rxe) {
            char *err = NULL;
            log_output(st, "Uploading to the NXT over USB…", "info");
            if (nxt_upload(st->post_rxe, st->post_run, &err)) {
                log_output(st, st->post_run ? "Program started on the NXT."
                                            : "Program stored on the NXT.", "info");
            } else {
                log_output(st, err ? err : "Upload failed.", "err");
                x->code = 1;
            }
            g_free(err);
        }
        g_clear_pointer(&st->post_rxe, g_free);
        if (x->code == 0) {
            log_output(st, "\n✔ Finished.", "ok");
            set_status(st, st->ok_msg ? st->ok_msg : "Ready.", "status-ok");
        } else {
            log_output(st, "\n✘ Finished with errors.", "err");
            set_status(st, "Finished with errors.", "status-error");
        }
        set_busy(st, FALSE);
        g_clear_object(&st->proc);
        if (st->on_done) {
            void (*done)(int, gpointer) = st->on_done;
            gpointer ud = st->tap_data;
            st->on_chars = NULL;
            st->on_done = NULL;
            st->tap_data = NULL;
            done(x->code, ud);
        }
    }
    g_free(x);
    return G_SOURCE_REMOVE;
}

static void on_proc_exit(GObject *src, GAsyncResult *res, gpointer user_data) {
    ExitCtx *x = user_data;
    GSubprocess *p = G_SUBPROCESS(src);
    g_subprocess_wait_finish(p, res, NULL);
    x->code = g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
    /* give the output pumps a moment to drain before reporting */
    g_timeout_add(80, finish_run_idle, x);
}

/* The one place that starts nqc or nbc. `argv` is NULL-terminated and is
 * copied; `rcx_port` (may be NULL/"") is exported as RCX_PORT for nqc. */
static gboolean run_tool(AppState *st, const char *const *argv,
                          const char *rcx_port, int timeout, const char *ok_msg,
                          const char *busy_msg) {
    if (st->running) {
        log_output(st, "A command is already running. Wait for it or press Cancel.", "err");
        return FALSE;
    }
    const char *tool = argv[0];
    char *found = g_find_program_in_path(tool);
    if (!found) {
        char msg[300];
        snprintf(msg, sizeof(msg),
                 T("'%s' was not found. Install it and make sure it is in your PATH "
                 "- every action in this window runs it."), tool);
        log_output(st, msg, "err");
        set_status(st, "Tool not found.", "status-error");
        return FALSE;
    }
    g_free(found);

    GString *pretty = g_string_new(NULL);
    if (rcx_port && *rcx_port) g_string_append_printf(pretty, "RCX_PORT=%s ", rcx_port);
    for (int i = 0; argv[i]; i++) {
        char *q = g_shell_quote(argv[i]);
        g_string_append_printf(pretty, "%s%s", i ? " " : "", q);
        g_free(q);
    }
    char *line = g_strdup_printf("$ %s", pretty->str);
    log_output(st, line, "cmd");
    g_free(line);
    g_string_free(pretty, TRUE);

    remote_tower_close();
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    if (rcx_port && *rcx_port)
        g_subprocess_launcher_setenv(launcher, "RCX_PORT", rcx_port, TRUE);
    if (st->current_file) {
        char *dir = g_path_get_dirname(st->current_file);
        g_subprocess_launcher_set_cwd(launcher, dir);
        g_free(dir);
    }

    GError *error = NULL;
    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_object_unref(launcher);
    if (!proc) {
        char msg[400];
        snprintf(msg, sizeof(msg), T("Could not start %s: %s"), tool, error->message);
        log_output(st, msg, "err");
        g_error_free(error);
        set_status(st, "Failed to start.", "status-error");
        return FALSE;
    }

    st->proc = proc;
    st->running = TRUE;
    st->run_id++;
    st->first_error = 0;
    st->last_seen_line = 0;
    st->port_hint_shown = FALSE;
    st->deadline = g_get_monotonic_time() + (gint64)timeout * G_USEC_PER_SEC;
    g_free(st->ok_msg);
    st->ok_msg = g_strdup(ok_msg);
    set_busy(st, TRUE);
    set_status(st, busy_msg, "status-busy");

    start_pump(st, g_subprocess_get_stdout_pipe(proc));
    start_pump(st, g_subprocess_get_stderr_pipe(proc));

    ExitCtx *x = g_new0(ExitCtx, 1);
    x->st = st;
    x->run_id = st->run_id;
    g_subprocess_wait_async(proc, NULL, on_proc_exit, x);
    return TRUE;
}

/* Watchdog: IR is slow, so a stuck command must not freeze the panel. */
static gboolean watchdog_tick(gpointer data) {
    AppState *st = data;
    if (st->running && st->proc && g_get_monotonic_time() >= st->deadline) {
        log_output(st, "No answer in time. Stopping the command.", "err");
        g_subprocess_force_exit(st->proc);
    }
    return G_SOURCE_CONTINUE;
}

static void cancel_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    if (st->running && st->proc) {
        g_subprocess_force_exit(st->proc);
        log_output(st, "Stopped.", "err");
    }
}

/* ---------------------------------------------------------------------
 * Building the command lines
 * --------------------------------------------------------------------- */

/* nqc: the -T target belongs on every invocation (it decides both the
 * generated code and how nqc talks to the brick). */
static gboolean run_nqc(AppState *st, GPtrArray *args, int timeout,
                         const char *ok_msg, const char *busy_msg) {
    guint t = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->target_dropdown));
    char *target = g_strdup_printf("-T%s", NQC_TARGETS[t].name);

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gpointer)NQC_CMD);
    g_ptr_array_add(argv, target);
    for (guint i = 0; i < args->len; i++) g_ptr_array_add(argv, args->pdata[i]);
    g_ptr_array_add(argv, NULL);

    const char *port = gtk_editable_get_text(GTK_EDITABLE(st->tower_entry));
    char *port_copy = g_strstrip(g_strdup(port ? port : ""));
    gboolean ok = run_tool(st, (const char *const *)argv->pdata, port_copy,
                            timeout, ok_msg, busy_msg);
    g_free(port_copy);
    g_free(target);
    g_ptr_array_free(argv, TRUE);
    return ok;
}

static char *nxc_port_arg(AppState *st) {
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->port_dropdown));
    if (sel == 0) return g_strdup("usb");
    const char *dev = gtk_editable_get_text(GTK_EDITABLE(st->bt_entry));
    return g_strdup(dev && *dev ? dev : "/dev/rfcomm0");
}

/* nbc finds NXCDefs.h through a compiled-in folder that only exists on some
 * systems, so look for the headers ourselves and pass -I= to nbc. */
static char *nbc_include_dir(void) {
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    char *nbc = g_find_program_in_path(NBC_CMD);
    if (nbc) {
        char *real = realpath(nbc, NULL);
        char *bin = g_path_get_dirname(real ? real : nbc);
        g_ptr_array_add(dirs, g_build_filename(bin, "..", "include", "nbc", NULL));
        g_ptr_array_add(dirs, g_build_filename(bin, "..", "share", "brickcode", "nbc", NULL));
        g_free(bin);
        free(real);
        g_free(nbc);
    }
    g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".local", "share",
                                           "brickcode", "nbc", NULL));
    g_ptr_array_add(dirs, g_strdup("/usr/local/include/nbc"));
    g_ptr_array_add(dirs, g_strdup("/usr/include/nbc"));
    g_ptr_array_add(dirs, g_strdup("/opt/homebrew/include/nbc"));

    char *found = NULL;
    for (guint i = 0; i < dirs->len && !found; i++) {
        char *hdr = g_build_filename(dirs->pdata[i], "NXCDefs.h", NULL);
        if (g_file_test(hdr, G_FILE_TEST_EXISTS)) found = g_strdup(dirs->pdata[i]);
        g_free(hdr);
    }
    g_ptr_array_free(dirs, TRUE);
    return found;
}

static gboolean ensure_saved(AppState *st) {
    if (!st->current_file) {
        log_output(st, "Save the program first - the compiler works on a file, "
                       "not on the editor buffer.", "err");
        save_dialog(st, "Save program");
        return FALSE;
    }
    return save_to_current_file(st);
}

static void compile_and(AppState *st, gboolean run) {
    if (st->running) return;
    if (!ensure_saved(st)) return;

    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(st->output_buffer, &s, &e);
    gtk_text_buffer_delete(st->output_buffer, &s, &e);

    if (st->mode == MODE_NQC) {
        GPtrArray *args = g_ptr_array_new();
        g_ptr_array_add(args, "-d");
        g_ptr_array_add(args, st->current_file);
        guint slot = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->slot_dropdown)) + 1;
        char *slot_s = g_strdup_printf("%u", slot);
        if (slot != 1) {
            g_ptr_array_add(args, "-pgm");
            g_ptr_array_add(args, slot_s);
        }
        if (run) g_ptr_array_add(args, "-run");
        run_nqc(st, args, TIMEOUT_DEFAULT,
                run ? "Running on the brick." : "Downloaded to the brick.",
                run ? "Compiling and running…" : "Compiling and downloading…");
        g_free(slot_s);
        g_ptr_array_free(args, TRUE);
    } else {
        if (gtk_drop_down_get_selected(GTK_DROP_DOWN(st->port_dropdown)) == 1) {
            /* Bluetooth (Linux): let nbc download, as before. */
            char *port = nxc_port_arg(st);
            char *port_flag = g_strdup_printf("-S=%s", port);
            const char *bt[8];
            int k = 0;
            bt[k++] = NBC_CMD;
            bt[k++] = st->current_file;
            bt[k++] = "-d";
            if (run) bt[k++] = "-r";
            bt[k++] = port_flag;
            bt[k] = NULL;
            run_tool(st, bt, NULL, TIMEOUT_DEFAULT,
                     run ? "Running on the NXT." : "Downloaded to the NXT.",
                     run ? "Compiling and running…" : "Compiling and downloading…");
            g_free(port_flag);
            g_free(port);
            return;
        }
        char *base = g_strndup(st->current_file,
            strlen(st->current_file) - strlen(strrchr(st->current_file, '.') ?
                                              strrchr(st->current_file, '.') : ""));
        char *rxe = g_strconcat(base, ".rxe", NULL);
        char *out_flag = g_strdup_printf("-O=%s", rxe);
        char *inc = nbc_include_dir();
        char *inc_flag = inc ? g_strdup_printf("-I=%s", inc) : NULL;
        const char *argv[8];
        int n = 0;
        argv[n++] = NBC_CMD;
        argv[n++] = st->current_file;
        argv[n++] = out_flag;
        if (inc_flag) argv[n++] = inc_flag;
        argv[n] = NULL;
        g_free(st->post_rxe);
        st->post_rxe = g_strdup(rxe);
        st->post_run = run;
        if (!run_tool(st, argv, NULL, TIMEOUT_DEFAULT,
                      run ? "Running on the NXT." : "Downloaded to the NXT.",
                      run ? "Compiling and running…" : "Compiling and downloading…"))
            g_clear_pointer(&st->post_rxe, g_free);
        g_free(inc_flag); g_free(inc); g_free(out_flag); g_free(rxe); g_free(base);
    }
}

static void compile_run_cb(GtkButton *btn, gpointer user_data) {
    compile_and(user_data, TRUE);
}

static void compile_upload_cb(GtkButton *btn, gpointer user_data) {
    compile_and(user_data, FALSE);
}

/* RCX utility buttons: each is one nqc invocation, e.g. "-clear". */
static void nqc_util_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    const char *flag = g_object_get_data(G_OBJECT(btn), "nqc-flag");
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, (gpointer)flag);
    run_nqc(st, args, TIMEOUT_DEFAULT, "Command finished.", "Running command…");
    g_ptr_array_free(args, TRUE);
}

static void nqc_sleep_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    char *minutes = g_strdup_printf("%d",
        (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(st->sleep_spin)));
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, "-sleep");
    g_ptr_array_add(args, minutes);
    run_nqc(st, args, TIMEOUT_DEFAULT, "Auto-shutoff set.", "Running command…");
    g_ptr_array_free(args, TRUE);
    g_free(minutes);
}

/* Test tools: is each compiler in the PATH, and does it start? */
static void report_tool(AppState *st, const char *tool, const char *flag) {
    char msg[500];
    char *path = g_find_program_in_path(tool);
    if (!path) {
        snprintf(msg, sizeof(msg), T("%s: NOT found in PATH."), tool);
        log_output(st, msg, "err");
        return;
    }
    char *argv[] = { path, (char *)flag, NULL };
    char *out = NULL, *errs = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                               &out, &errs, NULL, NULL);
    const char *text = (out && *out) ? out : errs;
    char *first = g_strdup(text ? text : "");
    char *nl = strchr(first, '\n');
    if (nl) *nl = 0;
    snprintf(msg, sizeof(msg), ok ? T("%s: found at %s") : T("%s: found at %s but did not start"),
             tool, path);
    log_output(st, msg, ok ? "info" : "err");
    if (*first) log_output(st, first, "dim");
    g_free(first); g_free(out); g_free(errs); g_free(path);
}

static void test_tools_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    report_tool(st, NQC_CMD, "-help");
    report_tool(st, NBC_CMD, "-help");
}

static void firmware_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    const char *path = gtk_editable_get_text(GTK_EDITABLE(st->fw_entry));
    if (!path || !*path || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        log_output(st, "Select a firmware file (.lgo) first.", "err");
        return;
    }
    log_output(st, "Flashing firmware takes a while. Keep the RCX pointed at the "
                   "tower and do not move it.", "info");
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, "-firmware");
    g_ptr_array_add(args, (gpointer)path);
    run_nqc(st, args, TIMEOUT_FIRMWARE, "Firmware sent.", "Sending firmware…");
    g_ptr_array_free(args, TRUE);
}

static void firmware_picked(GObject *source, GAsyncResult *res, gpointer user_data) {
    AppState *st = user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &error);
    if (!file) {
        g_clear_error(&error);
        return;
    }
    char *path = g_file_get_path(file);
    if (path) gtk_editable_set_text(GTK_EDITABLE(st->fw_entry), path);
    g_free(path);
    g_object_unref(file);
}

static void pick_firmware_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select firmware file");
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "RCX firmware (*.lgo, *.srec)");
    gtk_file_filter_add_pattern(f, "*.lgo");
    gtk_file_filter_add_pattern(f, "*.srec");
    GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(store, f);
    g_object_unref(f);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(store));
    g_object_unref(store);
    gtk_file_dialog_open(dialog, GTK_WINDOW(st->window), NULL, firmware_picked, st);
}

/* Raw command row: arguments exactly as typed, for whichever tool is active */
static void run_raw(AppState *st, const char *args_line) {
    if (!args_line || !*args_line) return;
    gint argc = 0;
    gchar **user_args = NULL;
    GError *error = NULL;
    if (!g_shell_parse_argv(args_line, &argc, &user_args, &error)) {
        char msg[300];
        snprintf(msg, sizeof(msg), T("Could not parse the command: %s"), error->message);
        log_output(st, msg, "err");
        g_error_free(error);
        return;
    }
    gint skip = (argc > 0 && strcmp(user_args[0], MODES[st->mode].tool) == 0) ? 1 : 0;
    if (argc - skip <= 0) {
        g_strfreev(user_args);
        return;
    }
    GPtrArray *args = g_ptr_array_new();
    for (gint i = skip; i < argc; i++) g_ptr_array_add(args, user_args[i]);

    if (st->mode == MODE_NQC) {
        run_nqc(st, args, TIMEOUT_DEFAULT, "Command finished.", "Running command…");
    } else {
        GPtrArray *argv = g_ptr_array_new();
        g_ptr_array_add(argv, (gpointer)NBC_CMD);
        for (guint i = 0; i < args->len; i++) g_ptr_array_add(argv, args->pdata[i]);
        g_ptr_array_add(argv, NULL);
        run_tool(st, (const char *const *)argv->pdata, NULL, TIMEOUT_DEFAULT,
                 "Command finished.", "Running command…");
        g_ptr_array_free(argv, TRUE);
    }
    g_ptr_array_free(args, TRUE);
    g_strfreev(user_args);
}


/* =======================================================================
 * Explorador NXT — comunicación USB directa (protocolo LCP de LEGO)
 * ======================================================================= */

#define NXT_VENDOR_ID  0x0694
#define NXT_PRODUCT_ID 0x0002
#define NXT_EP_OUT     0x01
#define NXT_EP_IN      0x82
#define NXT_TIMEOUT_MS 2000
#define NXT_NAME_LEN   20

/* Opcodes del protocolo LCP (LEGO Communication Protocol) */
#define CMD_DIRECT_REPLY   0x00
#define CMD_SYSTEM_REPLY   0x01
#define CMD_REPLY          0x02

#define SYS_OPEN_READ           0x80
#define SYS_CLOSE               0x84
#define SYS_DELETE               0x85
#define SYS_FIND_FIRST           0x86
#define SYS_FIND_NEXT            0x87
#define SYS_GET_FIRMWARE_VERSION 0x88

#define DIRECT_GET_BATTERY_LEVEL 0x0B

typedef struct {
    GtkWidget *window;
    GtkWidget *info_label;
    GtkStringList *file_model;
    GtkWidget *list_view;
    libusb_context *ctx;
    libusb_device_handle *handle;
} ExplorerState;

static libusb_device_handle *nxt_usb_open(libusb_context *ctx) {
    libusb_device_handle *h = libusb_open_device_with_vid_pid(
        ctx, NXT_VENDOR_ID, NXT_PRODUCT_ID);
    if (!h) return NULL;
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0) != 0) {
        libusb_close(h);
        return NULL;
    }
    return h;
}

static gboolean nxt_transact(libusb_device_handle *h, const guint8 *req,
                              int req_len, guint8 *reply, int reply_len) {
    int transferred = 0;
    int r = libusb_bulk_transfer(h, NXT_EP_OUT, (unsigned char *)req,
                                  req_len, &transferred, NXT_TIMEOUT_MS);
    if (r != 0) return FALSE;
    r = libusb_bulk_transfer(h, NXT_EP_IN, reply, reply_len, &transferred,
                              NXT_TIMEOUT_MS);
    return r == 0 && transferred >= 3;
}

static void pack_name(guint8 *dest, const char *name) {
    memset(dest, 0, NXT_NAME_LEN);
    g_strlcpy((char *)dest, name, NXT_NAME_LEN);
}

/* Send a compiled .rxe to the NXT and (optionally) start it.
 * LCP over USB: delete old copy, OPEN_WRITE, WRITE chunks, CLOSE, START. */
#define SYS_OPEN_WRITE 0x81
#define SYS_WRITE      0x83
#define DIRECT_START_PROGRAM 0x00
#define DIRECT_STOP_PROGRAM  0x01
#define DIRECT_SET_OUTPUT_STATE 0x04

static gboolean nxt_upload(const char *path, gboolean run, char **err) {
    char *data = NULL;
    gsize size = 0;
    GError *ge = NULL;
    if (!g_file_get_contents(path, &data, &size, &ge)) {
        *err = g_strdup_printf(T("Cannot read %s: %s"), path, ge->message);
        g_error_free(ge);
        return FALSE;
    }
    /* The NXT allows at most 15 characters plus the extension in a file name.
     * Cut the base name, never the ".rxe" at the end. */
    char *base = g_path_get_basename(path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    if (strlen(base) > 15) base[15] = '\0';
    char *name = g_strconcat(base, ".rxe", NULL);
    g_free(base);

    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *h = nxt_usb_open(ctx);
    if (!h) {
        *err = g_strdup(T("NXT not found over USB. Is it on and connected?"));
        goto fail;
    }

    guint8 req[64], rep[64];

    /* A program from a previous run may still be going (for example a
     * while(true) loop) and may still hold the motors. Stop it and turn
     * all motors off, so the new program starts from a clean state. */
    if (run) {
        memset(req, 0, sizeof(req));
        req[0] = CMD_DIRECT_REPLY; req[1] = DIRECT_STOP_PROGRAM;
        nxt_transact(h, req, 2, rep, sizeof(rep));       /* may say "no program" */
        memset(req, 0, sizeof(req));
        req[0] = CMD_DIRECT_REPLY; req[1] = DIRECT_SET_OUTPUT_STATE;
        req[2] = 0xFF;                                   /* all ports, mode 0 = off */
        nxt_transact(h, req, 12, rep, sizeof(rep));
        g_usleep(300000);
    }

    /* Delete the old copy and open the new file. If the NXT says the file
     * still exists or is busy, stop whatever is running and try again. */
    gboolean opened = FALSE;
    for (int attempt = 0; attempt < 4 && !opened; attempt++) {
        if (attempt > 0) {
            memset(req, 0, sizeof(req));
            req[0] = CMD_DIRECT_REPLY; req[1] = DIRECT_STOP_PROGRAM;
            nxt_transact(h, req, 2, rep, sizeof(rep));
            g_usleep(500000);
        }
        memset(req, 0, sizeof(req));
        req[0] = CMD_SYSTEM_REPLY; req[1] = SYS_DELETE;
        pack_name(req + 2, name);
        nxt_transact(h, req, 22, rep, sizeof(rep));      /* failure is fine */

        memset(req, 0, sizeof(req));
        req[0] = CMD_SYSTEM_REPLY; req[1] = SYS_OPEN_WRITE;
        pack_name(req + 2, name);
        req[22] = size; req[23] = size >> 8; req[24] = size >> 16; req[25] = size >> 24;
        if (nxt_transact(h, req, 26, rep, sizeof(rep)) && rep[2] == 0)
            opened = TRUE;
    }
    if (!opened) {
        *err = g_strdup_printf(T("NXT refused the file (status 0x%02X)."), rep[2]);
        goto fail;
    }
    guint8 handle = rep[3];

    for (gsize i = 0; i < size; i += 60) {
        gsize chunk = MIN((gsize)60, size - i);
        req[0] = CMD_SYSTEM_REPLY; req[1] = SYS_WRITE; req[2] = handle;
        memcpy(req + 3, data + i, chunk);
        if (!nxt_transact(h, req, 3 + chunk, rep, sizeof(rep)) || rep[2] != 0) {
            *err = g_strdup_printf(T("Write failed at byte %lu (status 0x%02X)."),
                                   (unsigned long)i, rep[2]);
            goto fail;
        }
    }

    req[0] = CMD_SYSTEM_REPLY; req[1] = SYS_CLOSE; req[2] = handle;
    if (!nxt_transact(h, req, 3, rep, sizeof(rep)) || rep[2] != 0) {
        *err = g_strdup(T("Could not close the file on the NXT."));
        goto fail;
    }

    if (run) {
        memset(req, 0, sizeof(req));
        req[0] = CMD_DIRECT_REPLY; req[1] = DIRECT_START_PROGRAM;
        pack_name(req + 2, name);
        if (!nxt_transact(h, req, 22, rep, sizeof(rep)) || rep[2] != 0) {
            *err = g_strdup_printf(T("Uploaded, but could not start it (status 0x%02X)."), rep[2]);
            goto fail;
        }
    }
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    g_free(name); g_free(data);
    return TRUE;

fail:
    if (h) { libusb_release_interface(h, 0); libusb_close(h); }
    libusb_exit(ctx);
    g_free(name); g_free(data);
    return FALSE;
}

static void explorer_refresh(ExplorerState *ex) {
    gtk_string_list_splice(ex->file_model, 0,
                            g_list_model_get_n_items(G_LIST_MODEL(ex->file_model)),
                            NULL);
    if (!ex->handle) return;

    guint8 req[2 + NXT_NAME_LEN];
    guint8 reply[64];

    req[0] = CMD_SYSTEM_REPLY;
    req[1] = SYS_FIND_FIRST;
    pack_name(req + 2, "*.*");

    if (!nxt_transact(ex->handle, req, sizeof(req), reply, sizeof(reply)))
        return;

    while (reply[2] == 0x00) { /* status == success */
        guint8 file_handle = reply[3];
        char name[NXT_NAME_LEN + 1] = {0};
        memcpy(name, reply + 4, NXT_NAME_LEN);
        guint32 size = reply[24] | (reply[25] << 8) |
                       (reply[26] << 16) | (reply[27] << 24);

        char entry[64];
        snprintf(entry, sizeof(entry), T("%s   (%u bytes)"), name, size);
        gtk_string_list_append(ex->file_model, entry);

        guint8 close_req[3] = {CMD_SYSTEM_REPLY, SYS_CLOSE, file_handle};
        guint8 close_reply[4];
        nxt_transact(ex->handle, close_req, sizeof(close_req),
                     close_reply, sizeof(close_reply));

        guint8 next_req[3] = {CMD_SYSTEM_REPLY, SYS_FIND_NEXT, file_handle};
        if (!nxt_transact(ex->handle, next_req, sizeof(next_req), reply,
                           sizeof(reply)))
            break;
    }
}

static void explorer_refresh_cb(GtkButton *btn, gpointer user_data) {
    explorer_refresh((ExplorerState *)user_data);
}

static void explorer_connect_cb(GtkButton *btn, gpointer user_data) {
    ExplorerState *ex = user_data;
    if (!ex->ctx) libusb_init(&ex->ctx);
    if (ex->handle) {
        libusb_release_interface(ex->handle, 0);
        libusb_close(ex->handle);
        ex->handle = NULL;
    }
    ex->handle = nxt_usb_open(ex->ctx);
    if (!ex->handle) {
        gtk_label_set_text(GTK_LABEL(ex->info_label),
            "NXT not found. Check the USB cable, make sure it's "
            "powered on, and that the udev rule (70-nxt.rules) is installed.");
        return;
    }

    guint8 batt_req[2] = {CMD_DIRECT_REPLY, DIRECT_GET_BATTERY_LEVEL};
    guint8 batt_reply[6];
    guint8 fw_req[2] = {CMD_SYSTEM_REPLY, SYS_GET_FIRMWARE_VERSION};
    guint8 fw_reply[8];

    char info[128];
    snprintf(info, sizeof(info), "%s", T("Connected"));
    if (nxt_transact(ex->handle, batt_req, sizeof(batt_req), batt_reply,
                      sizeof(batt_reply)) && batt_reply[2] == 0) {
        int mv = batt_reply[3] | (batt_reply[4] << 8);
        char batt_str[32];
        snprintf(batt_str, sizeof(batt_str), T(" — battery %d mV"), mv);
        strncat(info, batt_str, sizeof(info) - strlen(info) - 1);
    }
    if (nxt_transact(ex->handle, fw_req, sizeof(fw_req), fw_reply,
                      sizeof(fw_reply)) && fw_reply[2] == 0) {
        char fw_str[32];
        snprintf(fw_str, sizeof(fw_str), T(" — firmware %d.%d"),
                 fw_reply[6], fw_reply[5]);
        strncat(info, fw_str, sizeof(info) - strlen(info) - 1);
    }
    gtk_label_set_text(GTK_LABEL(ex->info_label), info);
    explorer_refresh(ex);
}

/* Test brick: ask the brick something and show its answer. */
static void test_brick_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    if (st->mode == MODE_NQC) {
        GPtrArray *args = g_ptr_array_new();
        g_ptr_array_add(args, "-getversion");
        run_nqc(st, args, TIMEOUT_DEFAULT, "The brick answered.", "Asking the brick…");
        g_ptr_array_free(args, TRUE);
        return;
    }
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { log_output(st, "libusb could not start.", "err"); return; }
    libusb_device_handle *h = nxt_usb_open(ctx);
    if (!h) {
        log_output(st, "NXT not found. Check the USB cable, that it is powered on, "
                       "and the USB permissions (udev rule 70-nxt.rules).", "err");
        libusb_exit(ctx);
        return;
    }
    guint8 breq[2] = { CMD_DIRECT_REPLY, DIRECT_GET_BATTERY_LEVEL }, brep[6];
    guint8 freq[2] = { CMD_SYSTEM_REPLY, SYS_GET_FIRMWARE_VERSION }, frep[8];
    char msg[200];
    if (nxt_transact(h, breq, 2, brep, sizeof(brep)) && brep[2] == 0) {
        snprintf(msg, sizeof(msg), T("The NXT answered: battery %d mV"), brep[3] | (brep[4] << 8));
        log_output(st, msg, "info");
    } else {
        log_output(st, "The NXT is there but did not answer.", "err");
    }
    if (nxt_transact(h, freq, 2, frep, sizeof(frep)) && frep[2] == 0) {
        snprintf(msg, sizeof(msg), T("NXT firmware %d.%d"), frep[6], frep[5]);
        log_output(st, msg, "info");
    }
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
}

static void explorer_delete_cb(GtkButton *btn, gpointer user_data) {
    ExplorerState *ex = user_data;
    guint selected = gtk_single_selection_get_selected(
        GTK_SINGLE_SELECTION(gtk_list_view_get_model(GTK_LIST_VIEW(ex->list_view))));
    if (selected == GTK_INVALID_LIST_POSITION || !ex->handle) return;

    const char *entry = gtk_string_list_get_string(ex->file_model, selected);
    char name[NXT_NAME_LEN + 1] = {0};
    sscanf(entry, "%20s", name);

    guint8 req[2 + NXT_NAME_LEN];
    req[0] = CMD_SYSTEM_REPLY;
    req[1] = SYS_DELETE;
    pack_name(req + 2, name);
    guint8 reply[2 + NXT_NAME_LEN + 1];
    nxt_transact(ex->handle, req, sizeof(req), reply, sizeof(reply));
    explorer_refresh(ex);
}

static void explorer_item_setup(GtkListItemFactory *factory,
                                 GtkListItem *item, gpointer user_data) {
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_list_item_set_child(item, label);
}

static void explorer_item_bind(GtkListItemFactory *factory,
                                GtkListItem *item, gpointer user_data) {
    GtkWidget *label = gtk_list_item_get_child(item);
    GtkStringObject *obj = gtk_list_item_get_item(item);
    gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(obj));
}

static void explorer_cleanup(GtkWidget *window, gpointer user_data) {
    ExplorerState *ex = user_data;
    if (ex->handle) {
        libusb_release_interface(ex->handle, 0);
        libusb_close(ex->handle);
    }
    if (ex->ctx) libusb_exit(ex->ctx);
    g_free(ex);
}

static void explorer_close_cb(GtkButton *btn, gpointer user_data) {
    ExplorerState *ex = user_data;
    gtk_window_destroy(GTK_WINDOW(ex->window));
}

static void open_explorer_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    ExplorerState *ex = g_new0(ExplorerState, 1);

    ex->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ex->window), "NXT Explorer");
    gtk_window_set_default_size(GTK_WINDOW(ex->window), 420, 380);
    gtk_window_set_transient_for(GTK_WINDOW(ex->window), GTK_WINDOW(st->window));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_window_set_child(GTK_WINDOW(ex->window), vbox);

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ex_title = gtk_label_new("NXT Explorer");
    gtk_widget_add_css_class(ex_title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(ex_title), 0.0);
    gtk_widget_set_hexpand(ex_title, TRUE);
    GtkWidget *close_btn = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(close_btn, "close-btn");
    gtk_box_append(GTK_BOX(top_row), ex_title);
    gtk_box_append(GTK_BOX(top_row), close_btn);
    gtk_box_append(GTK_BOX(vbox), top_row);

    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *connect_btn = gtk_button_new_with_label("Connect");
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    GtkWidget *delete_btn = gtk_button_new_with_label("Delete selected");
    gtk_widget_add_css_class(connect_btn, "qa-btn");
    gtk_widget_add_css_class(connect_btn, "qa-green");
    gtk_widget_add_css_class(refresh_btn, "qa-btn");
    gtk_widget_add_css_class(refresh_btn, "qa-blue");
    gtk_widget_add_css_class(delete_btn, "qa-btn");
    gtk_widget_add_css_class(delete_btn, "qa-pink");
    gtk_box_append(GTK_BOX(btnbox), connect_btn);
    gtk_box_append(GTK_BOX(btnbox), refresh_btn);
    gtk_box_append(GTK_BOX(btnbox), delete_btn);
    gtk_box_append(GTK_BOX(vbox), btnbox);

    ex->info_label = gtk_label_new("Not connected");
    gtk_label_set_xalign(GTK_LABEL(ex->info_label), 0.0);
    gtk_widget_add_css_class(ex->info_label, "status-text");
    gtk_box_append(GTK_BOX(vbox), ex->info_label);

    ex->file_model = gtk_string_list_new(NULL);
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(ex->file_model));
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(explorer_item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(explorer_item_bind), NULL);
    ex->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ex->list_view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    g_signal_connect(connect_btn, "clicked", G_CALLBACK(explorer_connect_cb), ex);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(explorer_refresh_cb), ex);
    g_signal_connect(delete_btn, "clicked", G_CALLBACK(explorer_delete_cb), ex);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(explorer_close_cb), ex);
    g_signal_connect(ex->window, "destroy", G_CALLBACK(explorer_cleanup), ex);

    gtk_window_present(GTK_WINDOW(ex->window));
}

/* ---------------------------------------------------------------------
 * Estilo visual — lee el tema activo de Omarchy (colors.toml) y genera
 * el CSS dinamicamente, con respaldo a nuestra paleta original.
 * --------------------------------------------------------------------- */

static void apply_theme(AppState *st, gboolean dark); /* forward declaration */

static char *darken_hex(const char *hex, double factor) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return g_strdup(hex ? hex : "#000000");
    unsigned int r, g, b;
    sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b);
    r = (unsigned int)CLAMP((int)(r * factor), 0, 255);
    g = (unsigned int)CLAMP((int)(g * factor), 0, 255);
    b = (unsigned int)CLAMP((int)(b * factor), 0, 255);
    return g_strdup_printf("#%02x%02x%02x", r, g, b);
}

static char *omarchy_theme_path(void) {
    return g_build_filename(g_get_home_dir(),
        ".local", "state", "omarchy", "current", "theme", "colors.toml", NULL);
}

/* Very small parser for Omarchy's flat colors.toml (key = "value" lines) —
 * no need for a full TOML library since the file has no nested tables. */
static GHashTable *load_omarchy_colors(void) {
    char *path = omarchy_theme_path();
    char *contents = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        g_clear_error(&error);
        g_free(path);
        return NULL;
    }
    g_free(path);

    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line || line[0] == '#' || line[0] == '[') continue;
        gchar *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        gchar *key = g_strstrip(line);
        gchar *val = g_strstrip(eq + 1);
        gsize len = strlen(val);
        if (len >= 2 && val[0] == '"' && val[len - 1] == '"') {
            val[len - 1] = 0;
            val++;
        }
        if (*key) g_hash_table_insert(h, g_strdup(key), g_strdup(val));
    }
    g_strfreev(lines);
    g_free(contents);
    return h;
}

static const char *theme_get(AppState *st, const char *key, const char *fallback) {
    if (!st->theme_colors) return fallback;
    const char *v = g_hash_table_lookup(st->theme_colors, key);
    return (v && *v) ? v : fallback;
}

static void on_theme_file_changed(GFileMonitor *monitor, GFile *file,
                                   GFile *other_file, GFileMonitorEvent event,
                                   gpointer user_data);

static void apply_omarchy_style(AppState *st) {
    if (st->theme_colors) g_hash_table_unref(st->theme_colors);
    st->theme_colors = load_omarchy_colors();

    /* Tokens del tema de Omarchy, con nuestra paleta original como respaldo
     * para cuando no se encuentra colors.toml (p. ej. fuera de Omarchy). */
    const char *accent    = theme_get(st, "accent", "#4C8DFF");
    const char *bg        = theme_get(st, "background", "#0E1014");
    const char *dark_bg   = theme_get(st, "dark_background", "#13161C");
    const char *darker_bg = theme_get(st, "darker_background", "#0B0D11");
    const char *fg        = theme_get(st, "dark_foreground", "#E7EAF0");
    const char *muted_fg  = theme_get(st, "muted", "#8B93A3");
    const char *editor_fg = theme_get(st, "foreground", "#D8DEE9");
    const char *border_c  = theme_get(st, "selection", "#2B313D");
    const char *field_bg  = theme_get(st, "lighter_background", "#1A1E26");
    const char *green     = theme_get(st, "green", "#22B573");
    const char *yellow    = theme_get(st, "yellow", "#F5A623");
    const char *magenta   = theme_get(st, "magenta", "#8C5CF0");
    const char *red       = theme_get(st, "red", "#F0609C");
    const char *blue      = theme_get(st, "blue", "#3B82F6");

    /* Outside Omarchy the Dark / Light selector restyles the whole window,
     * macOS style. (With an Omarchy theme the system colors win.) */
    gboolean mac_light = !st->theme_colors && !st->dark_theme;
    if (mac_light) {
        accent = "#0A73F0";  bg = "#F5F5F7";  dark_bg = "#FFFFFF";  darker_bg = "#F6F6F7";
        fg = "#1D1D1F";  muted_fg = "#6E6E73";  editor_fg = "#1D1D1F";  border_c = "#D9D9DE";
        field_bg = "#ECECF0";  green = "#28B24B";  yellow = "#F5A623";
        magenta = "#8B5CF6";  red = "#F25C9B";  blue = "#3B82F6";
    }
    if (!st->theme_colors) {
        GtkSettings *gs = gtk_settings_get_default();
        if (gs) g_object_set(gs, "gtk-application-prefer-dark-theme", !mac_light, NULL);
    }

    char *accent_d  = darken_hex(accent, 0.75);
    char *green_d   = darken_hex(green, 0.8);
    char *yellow_d  = darken_hex(yellow, 0.8);
    char *magenta_d = darken_hex(magenta, 0.8);
    char *red_d     = darken_hex(red, 0.8);
    char *blue_d    = darken_hex(blue, 0.8);
    char *white_btn = darken_hex(field_bg, 0.94);

    GString *c = g_string_new(NULL);

    g_string_append_printf(c, "window { background-color: %s; }", bg);

    /* Header */
    g_string_append_printf(c,
        ".header-bar { background: linear-gradient(135deg, %s, %s); padding: 16px 20px; }"
        ".header-icon { background-color: rgba(255,255,255,0.20); border-radius: 12px; padding: 10px 14px; }"
        ".header-icon label { color: white; font-weight: bold; font-size: 18px; }"
        ".header-title { color: white; font-size: 18px; font-weight: bold; }"
        ".header-subtitle { color: rgba(255,255,255,0.85); font-size: 11px; }"
        "button.about-btn { background-color: rgba(255,255,255,0.20); background-image: none; "
        "  color: white; border-radius: 8px; padding: 6px 14px; border: none; font-weight: bold; }"
        "button.about-btn:hover { background-color: rgba(255,255,255,0.32); }"
        "dropdown.mode-dd button { background-color: rgba(255,255,255,0.20); "
        "  background-image: none; color: white; border-radius: 8px; border: none; "
        "  font-weight: bold; padding: 4px 10px; }"
        "dropdown.mode-dd button:hover { background-color: rgba(255,255,255,0.32); }"
        "dropdown.mode-dd button label, dropdown.mode-dd button arrow { color: white; }",
        accent, accent_d);

    /* Tarjetas */
    g_string_append_printf(c,
        ".card { background-color: %s; border-radius: 14px; padding: 14px; margin: 10px 14px 0 14px; }"
        ".card-title { font-weight: bold; color: %s; font-size: 11px; }",
        field_bg, fg);

    /* Campos + botones tipo Browse */
    g_string_append_printf(c,
        "entry.path-field, entry.command-entry { background-color: %s; border: 1px solid %s; "
        "  border-radius: 8px; padding: 8px; color: %s; }"
        "button.browse-btn { background-color: %s; background-image: none; color: %s; "
        "  border-radius: 8px; padding: 8px 16px; border: none; font-weight: bold; }"
        "button.browse-btn:hover { background-color: %s; }",
        field_bg, border_c, fg, white_btn, fg, border_c);

    /* Botones de accion (colores del tema, con respaldo fijo) */
    g_string_append_printf(c,
        "button.qa-btn, button.big-action { border-radius: 10px; padding: 10px 8px; "
        "  font-weight: bold; color: white; border: none; background-image: none; "
        "  box-shadow: 0 2px 0 rgba(0,0,0,0.15); }"
        "button.big-action { padding: 14px; font-size: 13px; }"
        "button.qa-blue, button.big-blue     { background-color: %s; }"
        "button.qa-blue:hover, button.big-blue:hover     { background-color: %s; }"
        "button.qa-green, button.big-green   { background-color: %s; }"
        "button.qa-green:hover, button.big-green:hover   { background-color: %s; }"
        "button.qa-orange  { background-color: %s; }"
        "button.qa-orange:hover  { background-color: %s; }"
        "button.qa-purple  { background-color: %s; }"
        "button.qa-purple:hover  { background-color: %s; }"
        "button.qa-pink    { background-color: %s; }"
        "button.qa-pink:hover    { background-color: %s; }",
        blue, blue_d, green, green_d, yellow, yellow_d,
        magenta, magenta_d, red, red_d);

    /* Boton de cerrar */
    g_string_append_printf(c,
        "button.close-btn { background-color: %s; background-image: none; color: %s; "
        "  border-radius: 8px; padding: 4px 12px; border: none; font-weight: bold; min-width: 0; }"
        "button.close-btn:hover { background-color: %s; color: white; }",
        white_btn, fg, red);

    /* Ventana Acerca de */
    g_string_append_printf(c,
        ".about-title { font-weight: bold; font-size: 17px; color: %s; }"
        ".about-version { font-size: 10px; color: %s; }"
        ".about-subtitle { font-weight: bold; font-size: 11.5px; color: %s; }"
        ".about-body { color: %s; font-size: 10.5px; }"
        ".about-icon-lg { background-color: %s; min-width: 64px; min-height: 64px; "
        "  border-radius: 18px; }"
        ".about-icon-lg label { color: white; font-weight: bold; font-size: 20px; }"
        ".about-card { background-color: %s; border-radius: 10px; padding: 10px 14px; }"
        ".about-row-label { color: %s; font-size: 10px; }"
        ".about-row-value { color: %s; font-size: 10px; font-weight: bold; }"
        ".about-license, .about-copyright { color: %s; font-size: 9.5px; }",
        fg, muted_fg, accent, muted_fg, accent, field_bg, muted_fg, fg, muted_fg);

    /* Barra de herramientas */
    g_string_append_printf(c,
        ".toolbar-row { background-color: %s; padding: 10px 14px; border-bottom: 1px solid %s; }"
        ".filename-label, .console-label { font-weight: bold; color: %s; font-size: 12px; }",
        field_bg, border_c, fg);

    /* Paneles oscuros: editor / numeros de linea / consola */
    g_string_append_printf(c,
        ".panel { background-color: %s; border-radius: 10px; margin: 10px 14px; }"
        "textview.code-editor, textview.code-editor text "
        "{ background-color: %s; color: %s; }"
        "textview.console-editor, textview.console-editor text "
        "{ background-color: %s; color: %s; }"
        "textview.line-numbers, textview.line-numbers text "
        "{ background-color: %s; color: %s; }",
        dark_bg, dark_bg, editor_fg, dark_bg, editor_fg, darker_bg, muted_fg);

    /* Fila de ajustes */
    g_string_append_printf(c,
        ".settings-row { background-color: %s; padding: 10px 14px; border-top: 1px solid %s; }"
        ".settings-label { font-size: 10px; color: %s; font-weight: bold; }"
        "dropdown.settings-dd { border-radius: 8px; background-color: %s; border: 1px solid %s; }",
        field_bg, border_c, muted_fg, field_bg, border_c);

    /* Utilidades del NXT */
    g_string_append_printf(c,
        ".utils-row { background-color: %s; padding: 4px 14px 10px 14px; }"
        "expander.utils-expander title { font-weight: bold; color: %s; font-size: 11px; }",
        field_bg, fg);

    /* Renglon de comando */
    g_string_append_printf(c,
        ".command-row { background-color: %s; padding: 10px 14px; border-top: 1px solid %s; "
        "  border-bottom-left-radius: 14px; border-bottom-right-radius: 14px; }",
        field_bg, border_c);

    /* Barra de estado — los colores de estado quedan fijos a proposito       */
    /* (verde/naranja/rojo semanticos, no deben cambiar con el tema visual). */
    g_string_append(c,
        ".statusbar { padding: 8px 18px; }"
        ".status-dot { border-radius: 999px; min-width: 10px; min-height: 10px; }"
        ".status-ok { background-color: #22C55E; }"
        ".status-busy { background-color: #F5A623; }"
        ".status-error { background-color: #EF4444; }");
    g_string_append_printf(c, ".status-text { color: %s; font-size: 11px; }", muted_fg);


    /* ---- BrickCode Studio layout: top bar, drawer ---- */
    g_string_append_printf(c,
        "window { background-color: %s; }"
        ".topbar { background-color: %s; padding: 10px 14px; border-bottom: 1px solid %s; }"
        "button.home-btn { background: none; border: none; box-shadow: none; padding: 0; min-width: 0; }"
        "button.home-btn:hover { opacity: 0.85; }"
        ".tab-chip { background-color: %s; border-radius: 12px; padding: 4px 6px 4px 10px; }"
        ".lang-pill { background-color: %s; color: white; font-weight: bold; font-size: 10px; "
        "  border-radius: 6px; padding: 2px 7px; }"
        "button.kebab-btn, button.kebab-btn:hover { background: none; color: %s; border: none; font-weight: bold; "
        "  box-shadow: none; min-width: 0; padding: 0 6px; }"
        "button.plus-btn { background-color: %s; background-image: none; color: %s; "
        "  border: none; border-radius: 999px; min-width: 30px; min-height: 30px; padding: 0; "
        "  font-weight: bold; font-size: 16px; }"
        "button.plus-btn:hover { background-color: %s; }"
        "dropdown.mode-dd button { background-color: %s; background-image: none; color: %s; "
        "  border-radius: 10px; border: none; font-weight: bold; padding: 4px 10px; }"
        "dropdown.mode-dd button label, dropdown.mode-dd button arrow { color: %s; }"
        ".dev-chip { background-color: %s; border-radius: 999px; padding: 6px 14px; }"
        ".dev-label { color: %s; font-size: 11px; font-weight: bold; }"
        "button.pill-btn { border-radius: 999px; padding: 8px 18px; font-weight: bold; "
        "  color: white; border: none; background-image: none; }"
        "button.pill-green { background-color: %s; } button.pill-green:hover { background-color: %s; }"
        "button.pill-blue { background-color: %s; } button.pill-blue:hover { background-color: %s; }",
        bg, darker_bg, border_c, field_bg, accent, fg,
        field_bg, fg, border_c,
        field_bg, fg, fg,
        field_bg, fg,
        green, green_d, blue, blue_d);

    g_string_append_printf(c,
        "popover.snippet-pop contents { background-color: %s; border: 1px solid %s; border-radius: 12px; padding: 6px; }"
        "button.snippet-btn { background: none; border: none; box-shadow: none; color: %s; "
        "  border-radius: 8px; padding: 6px 12px 6px 16px; }"
        "button.snippet-btn:hover { background-color: %s; }"
        ".edge { background-color: %s; padding: 12px 8px; }"
        "button.edge-btn { background-color: %s; background-image: none; color: %s; border: none; "
        "  border-radius: 12px; min-width: 40px; min-height: 40px; font-weight: bold; }"
        "button.edge-btn:hover { background-color: %s; }"
        ".drawer { background-color: %s; padding: 14px; border-left: 1px solid %s; }"
        ".console-head { padding: 6px 14px 0 14px; }"
        ".status-idle { background-color: %s; }"
        "dropdown.settings-dd button, dropdown.settings-dd button label, dropdown.settings-dd arrow "
        "  { background-color: %s; background-image: none; color: %s; box-shadow: none; }"
        "dropdown popover contents, dropdown popover listview, dropdown popover listview row "
        "  { background-color: %s; color: %s; }"
        "expander.utils-expander title label { color: %s; }"
        "spinbutton, spinbutton text, spinbutton button { background-color: %s; color: %s; "
        "  background-image: none; border-color: %s; box-shadow: none; }"
        "paned separator { background-color: %s; min-height: 1px; min-width: 1px; }"
        "menubutton.kebab-btn button, menubutton.kebab-btn button label, menubutton.kebab-btn arrow "
        "  { background: none; border: none; box-shadow: none; color: white; min-width: 0; padding: 0 4px; }"
        "button.browse-btn { padding: 4px 12px; }",
        field_bg, border_c, fg, border_c,
        bg, field_bg, fg, border_c,
        bg, border_c, muted_fg,
        field_bg, fg, field_bg, fg, fg, field_bg, fg, border_c, border_c);

    /* workspace panels: dark canvas with soft corners */
    g_string_append_printf(c,
        ".panel { margin: 8px 6px 8px 10px; border-radius: 14px; }"
        "button.browse-btn { background-color: %s; color: %s; }"
        "button.browse-btn:hover { background-color: %s; }"
        ".statusbar { background-color: %s; border-top: 1px solid %s; }"
        ".card, .about-card { background-color: %s; }",
        border_c, fg, muted_fg, darker_bg, border_c, field_bg);

    /* ---- macOS-style layout: sidebar, segmented control, sheets ---- */
    g_string_append_printf(c,
        ".sidebar { background-color: %s; padding: 14px 10px; border-right: 1px solid %s; }"
        ".side-sec { font-size: 11px; font-weight: bold; color: %s; margin: 14px 8px 4px 8px; }"
        ".side-brick { padding: 4px 8px; font-size: 13px; color: %s; }"
        "button.side-btn { background: none; background-image: none; border: none; box-shadow: none; "
        "  border-radius: 7px; padding: 5px 8px; color: %s; min-height: 0; }"
        "button.side-btn:hover { background-color: %s; }"
        ".side-sq { min-width: 16px; min-height: 16px; border-radius: 4px; }"
        ".sq-blue { background-color: %s; } .sq-green { background-color: %s; }"
        ".sq-orange { background-color: %s; } .sq-pink { background-color: %s; }"
        ".sq-gray { background-color: %s; }"
        ".seg { background-color: %s; border-radius: 9px; padding: 2px; }"
        ".seg button { background: none; background-image: none; border: none; box-shadow: none; "
        "  border-radius: 7px; padding: 4px 14px; min-height: 0; color: %s; font-size: 12px; }"
        ".seg button:checked { background-color: %s; font-weight: bold; "
        "  box-shadow: 0 1px 2px rgba(0,0,0,0.25); }",
        field_bg, border_c, muted_fg, fg, fg, border_c,
        blue, green, yellow, red, muted_fg,
        border_c, fg, dark_bg);

    g_string_append_printf(c,
        "window.fw-win { background: none; }"
        ".fw-sheet { background-color: %s; border-radius: 14px; padding: 26px 30px 22px 30px; "
        "  border: 1px solid %s; }"
        ".fw-title { font-size: 17px; font-weight: bold; color: %s; }"
        ".fw-sub { font-size: 12.5px; color: %s; }"
        "checkbutton.fw-opt { background-color: %s; border: 1px solid %s; border-radius: 10px; "
        "  padding: 10px 12px; }"
        "checkbutton.fw-opt:checked { border: 2px solid %s; padding: 9px 11px; }"
        ".fw-opt-name { font-weight: bold; font-size: 13.5px; color: %s; }"
        ".fw-opt-desc { font-size: 12px; color: %s; }"
        ".fw-tag { background-color: rgba(40,178,75,0.15); color: %s; font-size: 10.5px; "
        "  font-weight: bold; border-radius: 999px; padding: 3px 9px; }"
        ".fw-check { background-color: %s; border-radius: 10px; padding: 10px 14px; color: %s; "
        "  font-size: 12.5px; }"
        ".fw-err { color: %s; font-size: 12px; }"
        ".fw-warn { background-color: rgba(245,166,35,0.15); border: 1px solid rgba(245,166,35,0.6); "
        "  border-radius: 10px; padding: 10px 14px; color: %s; font-size: 12.5px; }"
        "progressbar.fw-bar trough { min-height: 10px; border-radius: 999px; background-color: %s; }"
        "progressbar.fw-bar progress { min-height: 10px; border-radius: 999px; background-color: %s; }"
        "menubutton.kebab-btn button, menubutton.kebab-btn button label, menubutton.kebab-btn arrow "
        "  { color: %s; }",
        bg, border_c, fg, muted_fg,
        field_bg, border_c, accent, fg, muted_fg, green,
        field_bg, fg, red, fg, border_c, accent, fg);

    if (st->css_provider) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(st->css_provider));
        g_object_unref(st->css_provider);
    }
    st->css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(st->css_provider, c->str);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(st->css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);

    g_string_free(c, TRUE);
    g_free(accent_d); g_free(green_d); g_free(yellow_d);
    g_free(magenta_d); g_free(red_d); g_free(blue_d); g_free(white_btn);

    /* Reaplicar el tema del editor (oscuro/claro) por encima, respetando
     * la seleccion actual del usuario en el desplegable TEMA. */
    if (st->theme_dropdown) apply_theme(st, st->dark_theme);
}

/* Se llama cuando el usuario cambia de tema con omarchy-theme-menu mientras
 * el IDE esta abierto: vuelve a leer colors.toml y repinta todo al vuelo. */
static void on_theme_file_changed(GFileMonitor *monitor, GFile *file,
                                   GFile *other_file, GFileMonitorEvent event,
                                   gpointer user_data) {
    if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        event != G_FILE_MONITOR_EVENT_CREATED &&
        event != G_FILE_MONITOR_EVENT_RENAMED)
        return;
    apply_omarchy_style((AppState *)user_data);
}

static void watch_omarchy_theme(AppState *st) {
    char *path = omarchy_theme_path();
    GFile *file = g_file_new_for_path(path);
    GError *error = NULL;
    st->theme_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
    if (st->theme_monitor) {
        g_signal_connect(st->theme_monitor, "changed",
                          G_CALLBACK(on_theme_file_changed), st);
    } else {
        g_clear_error(&error);
    }
    g_object_unref(file);
    g_free(path);
}

static void apply_theme(AppState *st, gboolean dark) {
    st->dark_theme = dark;
    const char *editor_bg = dark ? "#13161C" : "#FFFFFF";
    const char *editor_fg = dark ? "#D8DEE9" : "#1A1D2B";
    const char *gutter_bg = dark ? "#0F1217" : "#EEF1F7";
    const char *gutter_fg = dark ? "#5A6280" : "#9AA1B5";

    char css[1024];
    snprintf(css, sizeof(css),
        "textview.code-editor, textview.code-editor text, "
        "textview.console-editor, textview.console-editor text "
        "{ background-color: %s; color: %s; }"
        "textview.line-numbers, textview.line-numbers text "
        "{ background-color: %s; color: %s; }",
        editor_bg, editor_fg, gutter_bg, gutter_fg);

    if (st->theme_override_provider) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(st->theme_override_provider));
        g_object_unref(st->theme_override_provider);
    }
    st->theme_override_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(st->theme_override_provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(st->theme_override_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

    g_object_set(st->tag_number, "foreground", dark ? "#b5cea8" : "#2E7D32", NULL);
    g_object_set(st->tag_string, "foreground", dark ? "#ce9178" : "#B25000", NULL);
}


/* App icon, drawn with cairo (no image files needed): a dark rounded square,
 * a four-color ring and a play triangle. Coordinates are on a 128x128 grid. */
static void icon_rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void icon_fill(cairo_t *cr, double x, double y, double w, double h, double r,
                      int R, int G, int B) {
    icon_rrect(cr, x, y, w, h, r);
    cairo_set_source_rgb(cr, R / 255.0, G / 255.0, B / 255.0);
    cairo_fill(cr);
}

/* App icon: a small programmable brick (screen, orange button, ports)
 * on a dark rounded square. Drawn on a 512 grid. */
static void draw_app_icon(cairo_t *cr, double size) {
    cairo_save(cr);
    cairo_scale(cr, size / 512.0, size / 512.0);

    icon_rrect(cr, 0, 0, 512, 512, 112.64);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, 512);
    cairo_pattern_add_color_stop_rgb(g, 0, 28 / 255.0, 42 / 255.0, 74 / 255.0);
    cairo_pattern_add_color_stop_rgb(g, 1, 18 / 255.0, 26 / 255.0, 48 / 255.0);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    icon_fill(cr, 66, 92, 380, 340, 44, 120, 126, 136);       /* lower edge */
    icon_fill(cr, 66, 84, 380, 336, 44, 222, 226, 232);       /* body */
    icon_fill(cr, 80, 92, 352, 20, 10, 244, 246, 250);        /* top shine */
    icon_fill(cr, 112, 128, 288, 138, 16, 60, 64, 72);        /* screen frame */
    icon_fill(cr, 122, 138, 268, 118, 10, 150, 176, 140);     /* screen */
    icon_fill(cr, 196, 170, 22, 22, 3, 34, 52, 34);           /* eyes */
    icon_fill(cr, 296, 170, 22, 22, 3, 34, 52, 34);
    icon_fill(cr, 196, 214, 108, 12, 5, 34, 52, 34);          /* smile */
    icon_fill(cr, 184, 200, 16, 14, 5, 34, 52, 34);
    icon_fill(cr, 300, 200, 16, 14, 5, 34, 52, 34);
    icon_fill(cr, 112, 292, 64, 48, 12, 70, 76, 88);          /* arrow buttons */
    icon_fill(cr, 336, 292, 64, 48, 12, 70, 76, 88);
    icon_fill(cr, 180, 362, 152, 30, 12, 70, 76, 88);         /* back button */

    cairo_set_source_rgb(cr, 250 / 255.0, 140 / 255.0, 20 / 255.0);   /* orange button */
    cairo_new_path(cr);
    cairo_save(cr); cairo_translate(cr, 256, 316); cairo_scale(cr, 46, 34);
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI); cairo_restore(cr);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 176 / 255.0, 70 / 255.0);
    cairo_new_path(cr);
    cairo_save(cr); cairo_translate(cr, 256, 310); cairo_scale(cr, 32, 18);
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI); cairo_restore(cr);
    cairo_fill(cr);

    for (int k = 0; k < 4; k++)                               /* ports */
        icon_fill(cr, 112 + 64 * k, 404, 52, 20, 6, 40, 44, 52);
    cairo_restore(cr);
}

static void icon_draw_cb(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    draw_app_icon(cr, MIN(w, h));
}

static GtkWidget *make_icon_widget(int size) {
    GtkWidget *a = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(a), size);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(a), size);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a), icon_draw_cb, NULL, NULL);
    return a;
}

/* ---------------------------------------------------------------------
 * About window
 * --------------------------------------------------------------------- */

static void about_close_cb(GtkButton *btn, gpointer user_data) {
    gtk_window_destroy(GTK_WINDOW(user_data));
}

static void about_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "About");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(st->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_margin_start(card, 14);
    gtk_widget_set_margin_end(card, 14);
    gtk_widget_set_margin_top(card, 16);
    gtk_widget_set_margin_bottom(card, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), card);

    GtkWidget *icon_box = make_icon_widget(88);
    gtk_widget_set_halign(icon_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), icon_box);

    GtkWidget *title = gtk_label_new(APP_NAME);
    gtk_widget_add_css_class(title, "about-title");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(title, 10);
    gtk_box_append(GTK_BOX(card), title);

    GtkWidget *version = gtk_label_new("Version " APP_VERSION);
    gtk_widget_add_css_class(version, "about-version");
    gtk_widget_set_halign(version, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), version);

    GtkWidget *subtitle = gtk_label_new("NQC + NXC editor  ·  Linux and macOS");
    gtk_widget_add_css_class(subtitle, "about-subtitle");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(subtitle, 6);
    gtk_box_append(GTK_BOX(card), subtitle);

    GtkWidget *body = gtk_label_new(
        "Write NQC for the RCX and Scout, or NXC for the NXT, press F5,\n"
        "and the program compiles, downloads and runs on the brick.");
    gtk_widget_add_css_class(body, "about-body");
    gtk_label_set_justify(GTK_LABEL(body), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(body, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(body, 8);
    gtk_box_append(GTK_BOX(card), body);

    GtkWidget *info_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(info_card, "about-card");
    gtk_widget_set_margin_top(info_card, 12);

    const char *rows[][2] = {
        {"Compilers",    "nqc (RCX / Scout)  ·  nbc (NXT)"},
        {"NQC targets",  "RCX2 · RCX · Scout · Spy"},
        {"NQC link",     "IR tower (USB / serial)"},
        {"NXC link",     "USB (libusb) · Bluetooth on Linux"},
        {"Highlighting", "Custom (GRegex)"},
        {"Theme",        "Light · Dark · follows Omarchy on Linux"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(rows[i][0]);
        gtk_widget_add_css_class(label, "about-row-label");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        GtkWidget *value = gtk_label_new(rows[i][1]);
        gtk_widget_add_css_class(value, "about-row-value");
        gtk_label_set_xalign(GTK_LABEL(value), 1.0);
        gtk_widget_set_hexpand(value, TRUE);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), value);
        gtk_box_append(GTK_BOX(info_card), row);
    }
    gtk_box_append(GTK_BOX(card), info_card);

    GtkWidget *license = gtk_label_new("MIT License");
    gtk_widget_add_css_class(license, "about-license");
    gtk_widget_set_halign(license, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(license, 12);
    gtk_box_append(GTK_BOX(card), license);

    GtkWidget *github = gtk_link_button_new_with_label(
        "https://github.com/JimBolanos", "github.com/JimBolanos");
    gtk_widget_add_css_class(github, "about-link");
    gtk_widget_set_halign(github, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), github);

    GtkWidget *rebrickable = gtk_link_button_new_with_label(
        "https://rebrickable.com/users/jimmy.bolanos/", "rebrickable.com/users/jimmy.bolanos");
    gtk_widget_add_css_class(rebrickable, "about-link");
    gtk_widget_set_halign(rebrickable, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), rebrickable);

    GtkWidget *copyright_label = gtk_label_new("© 2026 Jim Bolaños");
    gtk_widget_add_css_class(copyright_label, "about-copyright");
    gtk_widget_set_halign(copyright_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), copyright_label);

    GtkWidget *legal = gtk_label_new(
        "Independent project - not affiliated with or endorsed\n"
        "by the LEGO Group. LEGO and MINDSTORMS are trademarks\n"
        "of the LEGO Group.");
    gtk_widget_add_css_class(legal, "about-copyright");
    gtk_label_set_justify(GTK_LABEL(legal), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(legal, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(legal, 8);
    gtk_box_append(GTK_BOX(card), legal);

    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(close_btn, "browse-btn");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(close_btn, 12);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(about_close_cb), dialog);
    gtk_box_append(GTK_BOX(card), close_btn);

    gtk_window_present(GTK_WINDOW(dialog));
}

/* ---------------------------------------------------------------------
 * Raw command row (with history)
 * --------------------------------------------------------------------- */

static void cmd_run_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(st->cmd_entry));
    if (!text || !*text) return;
    g_ptr_array_add(st->cmd_history, g_strdup(text));
    st->cmd_history_pos = (gint)st->cmd_history->len;
    run_raw(st, text);
}

static gboolean cmd_entry_key_cb(GtkEventControllerKey *ctrl, guint keyval,
                                  guint keycode, GdkModifierType state,
                                  gpointer user_data) {
    AppState *st = user_data;
    if (st->cmd_history->len == 0) return FALSE;
    if (keyval == GDK_KEY_Up) {
        if (st->cmd_history_pos > 0) st->cmd_history_pos--;
        gtk_editable_set_text(GTK_EDITABLE(st->cmd_entry),
            g_ptr_array_index(st->cmd_history, st->cmd_history_pos));
        return TRUE;
    }
    if (keyval == GDK_KEY_Down) {
        if (st->cmd_history_pos < (gint)st->cmd_history->len - 1) {
            st->cmd_history_pos++;
            gtk_editable_set_text(GTK_EDITABLE(st->cmd_entry),
                g_ptr_array_index(st->cmd_history, st->cmd_history_pos));
        } else {
            st->cmd_history_pos = (gint)st->cmd_history->len;
            gtk_editable_set_text(GTK_EDITABLE(st->cmd_entry), "");
        }
        return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------------
 * Small callbacks
 * --------------------------------------------------------------------- */

static void port_changed_cb(GObject *dropdown, GParamSpec *pspec,
                             gpointer user_data) {
    AppState *st = user_data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    gtk_revealer_set_reveal_child(GTK_REVEALER(st->bt_revealer), sel == 1);
}

static void target_changed_cb(GObject *dropdown, GParamSpec *pspec,
                               gpointer user_data) {
    AppState *st = user_data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (sel < G_N_ELEMENTS(NQC_TARGETS))
        gtk_label_set_text(GTK_LABEL(st->target_note), NQC_TARGETS[sel].desc);
}

static void theme_changed_cb(GObject *dropdown, GParamSpec *pspec,
                              gpointer user_data) {
    AppState *st = user_data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    st->dark_theme = (sel == 0);
    if (!st->theme_colors) apply_omarchy_style(st);
    else apply_theme(st, sel == 0);
}

static void clear_console_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(st->output_buffer, &s, &e);
    gtk_text_buffer_delete(st->output_buffer, &s, &e);
}

static gboolean window_key_cb(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data) {
    if (keyval == GDK_KEY_F5) {
        compile_run_cb(NULL, user_data);
        return TRUE;
    }
    return FALSE;
}

/* Unsaved changes: ask before closing (Save / Don't save / Cancel). */
static void close_answer(GObject *src, GAsyncResult *res, gpointer user_data) {
    AppState *st = user_data;
    GError *error = NULL;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &error);
    g_clear_error(&error);
    if (choice == 0) {                        /* Save */
        if (st->current_file) {
            if (save_to_current_file(st)) gtk_window_close(GTK_WINDOW(st->window));
        } else {
            st->close_after_save = TRUE;
            save_dialog(st, "Save program");
        }
    } else if (choice == 1) {                 /* Don't save */
        st->dirty = FALSE;
        gtk_window_close(GTK_WINDOW(st->window));
    }                                         /* Cancel: stay */
}

static gboolean window_close_cb(GtkWindow *w, gpointer user_data) {
    AppState *st = user_data;
    if (st->dirty) {
        char *base = st->current_file ? g_path_get_basename(st->current_file)
                                      : g_strdup(T("untitled"));
        char *msg = g_strdup_printf(T("Save changes to %s?"), base);
        GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
        gtk_alert_dialog_set_detail(dlg, T("If you don't save, your changes are lost."));
        const char *buttons[] = { T("Save"), T("Don't save"), T("Cancel"), NULL };
        gtk_alert_dialog_set_buttons(dlg, buttons);
        gtk_alert_dialog_set_default_button(dlg, 0);
        gtk_alert_dialog_set_cancel_button(dlg, 2);
        gtk_alert_dialog_choose(dlg, w, NULL, close_answer, st);
        g_object_unref(dlg);
        g_free(msg);
        g_free(base);
        return TRUE;                          /* keep the window for now */
    }
    settings_save(st);
    if (st->running && st->proc) g_subprocess_force_exit(st->proc);
    return FALSE;
}

/* ---------------------------------------------------------------------
 * Small widget helpers for the settings / utilities rows
 * --------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * Connected-brick indicator (top right): looks at the USB bus every 2 s
 * --------------------------------------------------------------------- */

static libusb_context *dev_ctx;

static gboolean device_poll(gpointer data) {
    AppState *st = data;
    if (!dev_ctx && libusb_init(&dev_ctx) != 0) return G_SOURCE_CONTINUE;
    gboolean nxt = FALSE, tower = FALSE, samba = FALSE;
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(dev_ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (d.idVendor == NXT_VENDOR_ID && d.idProduct == NXT_PRODUCT_ID) nxt = TRUE;
        if (d.idVendor == NXT_VENDOR_ID && d.idProduct == 0x0001) tower = TRUE;
        if (d.idVendor == 0x03eb && d.idProduct == 0x6124) samba = TRUE;
    }
    if (list) libusb_free_device_list(list, 1);

    const char *text = "No brick connected";
    if (nxt && tower) text = "NXT + RCX tower";
    else if (nxt) text = "NXT connected";
    else if (tower) text = "RCX tower connected";
    else if (samba) text = "NXT in firmware mode";
    gtk_label_set_text(GTK_LABEL(st->dev_label), text);
    gtk_widget_remove_css_class(st->dev_dot, "status-ok");
    gtk_widget_remove_css_class(st->dev_dot, "status-idle");
    gtk_widget_add_css_class(st->dev_dot,
        (nxt || tower) ? "status-ok" : "status-idle");
    if (st->side_rcx_dot && st->side_nxt_dot) {
        GtkWidget *dots[2] = { st->side_rcx_dot, st->side_nxt_dot };
        gboolean on[2] = { tower, nxt };
        for (int i = 0; i < 2; i++) {
            gtk_widget_remove_css_class(dots[i], "status-ok");
            gtk_widget_remove_css_class(dots[i], "status-idle");
            gtk_widget_add_css_class(dots[i], on[i] ? "status-ok" : "status-idle");
        }
    }
    return G_SOURCE_CONTINUE;
}

/* Small helpers for the new top bar */
static void popdown_cb(GtkButton *btn, gpointer pop) {
    gtk_popover_popdown(GTK_POPOVER(pop));
}

static GtkWidget *menu_item(const char *label, GCallback cb, gpointer data,
                             GtkWidget *popover) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "snippet-btn");
    gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(b))), 0.0);
    g_signal_connect(b, "clicked", cb, data);
    g_signal_connect(b, "clicked", G_CALLBACK(popdown_cb), popover);
    return b;
}

static void toggle_drawer_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    gtk_widget_set_visible(st->drawer, !gtk_widget_get_visible(st->drawer));
}

static void toggle_console_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    gtk_widget_set_visible(st->console_box,
                           !gtk_widget_get_visible(st->console_box));
}

static GtkWidget *settings_column(const char *label_text, GtkWidget *child) {
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_add_css_class(lbl, "settings-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(col), lbl);
    gtk_box_append(GTK_BOX(col), child);
    return col;
}

static GtkWidget *chip_button(const char *label, const char *css_class,
                               GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "qa-btn");
    gtk_widget_add_css_class(b, css_class);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static GtkWidget *nqc_flag_button(AppState *st, const char *label,
                                   const char *css_class, const char *flag) {
    GtkWidget *b = chip_button(label, css_class, G_CALLBACK(nqc_util_cb), st);
    g_object_set_data(G_OBJECT(b), "nqc-flag", (gpointer)flag);
    return b;
}

/* ---------------------------------------------------------------------
 * Remote: the buttons of the LEGO IR remote, sent with  nqc -remote
 * Each button is one 16-bit mask (the same values BricxCC uses).
 * --------------------------------------------------------------------- */

/* The RCX ignores a message whose opcode equals the previous one (it takes it
 * for a retransmission). "nqc -remote HEX N" always sends 0xD2, so only the first
 * message counts and the others time out. We send one message per nqc run with
 * "-raw1" and alternate the opcode between 0xD2 and 0xDA (the RCX toggle bit). */
static gboolean remote_toggle = FALSE;
static gboolean remote_active = FALSE;   /* a button is being held */
static gboolean remote_busy = FALSE;     /* an nqc run is in flight */
static char remote_hex[8] = "0000";
static AppState *remote_st = NULL;

static void remote_next(void);

#ifdef __linux__
/* On Linux the kernel driver waits 0.5 s every time the tower is closed, and nqc closes it
 * after each message. So the Remote opens /dev/usb/legousbtower0 once and writes the IR
 * frames itself (about 50 ms each). If anything goes wrong it falls back to nqc. */
static int tower_fd = -1;
static gboolean tower_failed = FALSE;

static void remote_tower_close(void) {
    if (tower_fd >= 0) { close(tower_fd); tower_fd = -1; }
}

static gboolean tower_open_dev(AppState *st) {
    if (tower_fd >= 0) return TRUE;
    if (tower_failed) return FALSE;
    const char *port = gtk_editable_get_text(GTK_EDITABLE(st->tower_entry));
    const char *dev = "/dev/usb/legousbtower0";
    if (port && g_str_has_prefix(port, "usb:") && port[4]) dev = port + 4;
    else if (port && *port && strcmp(port, "usb") != 0) return FALSE;   /* serial tower: use nqc */
    tower_fd = open(dev, O_RDWR | O_CLOEXEC);
    return tower_fd >= 0;
}

static void tower_write_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    gsize n;
    const guchar *p = g_bytes_get_data((GBytes *)data, &n);
    int fd = tower_fd;
    g_task_return_boolean(task, fd >= 0 && write(fd, p, n) == (ssize_t)n);
}

static void remote_done_tower(GObject *src, GAsyncResult *res, gpointer data) {
    if (!g_task_propagate_boolean(G_TASK(res), NULL)) {
        remote_tower_close();
        tower_failed = TRUE;          /* use nqc from now on */
    }
    remote_busy = FALSE;
    remote_next();
}

/* IR frame: 55 ff 00, then each byte followed by its complement, then checksum. */
static gboolean remote_send_tower(AppState *st, guchar op, guchar b1, guchar b2) {
    if (!tower_open_dev(st)) return FALSE;
    guchar sum = (guchar)(op + b1 + b2);
    guchar f[11] = { 0x55, 0xff, 0x00, op, (guchar)~op, b1, (guchar)~b1,
                     b2, (guchar)~b2, sum, (guchar)~sum };
    GTask *t = g_task_new(NULL, NULL, remote_done_tower, NULL);
    g_task_set_task_data(t, g_bytes_new(f, sizeof(f)), (GDestroyNotify)g_bytes_unref);
    remote_busy = TRUE;
    g_task_run_in_thread(t, tower_write_thread);
    g_object_unref(t);
    return TRUE;
}
#else
static void remote_tower_close(void) {}
#endif

static void remote_done(GObject *src, GAsyncResult *res, gpointer data) {
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    remote_busy = FALSE;
    remote_next();
}

/* Send one message (opcode toggles every time). */
static void remote_send(const char *hex) {
    AppState *st = remote_st;
    guint t = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->target_dropdown));
    char *target = g_strdup_printf("-T%s", NQC_TARGETS[t].name);
    char msg[16];
    /* bytes: opcode, low byte, high byte of the 16-bit mask */
    snprintf(msg, sizeof(msg), "%s%.2s%.2s", remote_toggle ? "DA" : "D2", hex + 2, hex);
    remote_toggle = !remote_toggle;
#ifdef __linux__
    {
        unsigned v = (unsigned)strtoul(hex, NULL, 16);       /* 16-bit key mask */
        if (remote_send_tower(st, msg[1] == 'A' ? 0xDA : 0xD2, v & 0xFF, (v >> 8) & 0xFF)) {
            g_free(target);
            return;
        }
    }
#endif
    const char *argv[] = { NQC_CMD, target, "-raw1", msg, NULL };
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    const char *port = gtk_editable_get_text(GTK_EDITABLE(st->tower_entry));
    if (port && *port) g_subprocess_launcher_setenv(l, "RCX_PORT", port, TRUE);
    GSubprocess *p = g_subprocess_launcher_spawnv(l, argv, NULL);
    g_object_unref(l);
    g_free(target);
    if (!p) return;
    remote_busy = TRUE;
    g_subprocess_wait_async(p, NULL, remote_done, NULL);
    g_object_unref(p);
}

/* Called when a run ends: keep sending while the button is held, then send 0000. */
static gboolean remote_stop_pending = FALSE;
static void remote_next(void) {
    if (remote_active) { remote_send(remote_hex); return; }
    if (remote_stop_pending) { remote_stop_pending = FALSE; remote_send("0000"); }
}

static void remote_btn_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    char hex[8];
    remote_st = st;
    if (remote_busy) return;
    snprintf(hex, sizeof(hex), "%04X",
             GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "mask")));
    remote_send(hex);
    char *line = g_strdup_printf("$ nqc -raw1 (remote %s)", hex);
    log_output(st, line, "cmd");
    g_free(line);
}

static GtkWidget *remote_button(AppState *st, const char *label, const char *css,
                                 guint mask) {
    GtkWidget *b = chip_button(label, css, G_CALLBACK(remote_btn_cb), st);
    g_object_set_data(G_OBJECT(b), "mask", GUINT_TO_POINTER(mask));
    return b;
}

/* Motor buttons work like the real remote: the motor runs while the button is
 * held (messages keep being sent); releasing sends "keys released" (0000). */
static void remote_press_cb(GtkGestureClick *g, gint n, gdouble x, gdouble y, gpointer data) {
    AppState *st = data;
    char hex[8];
    /* The button has its own click gesture that would cancel ours: claim the press. */
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    snprintf(hex, sizeof(hex), "%04X", GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "mask")));
    remote_st = st;
    g_strlcpy(remote_hex, hex, sizeof(remote_hex));
    remote_active = TRUE;
    remote_stop_pending = FALSE;
    if (!remote_busy) remote_send(remote_hex);
    char *line = g_strdup_printf("$ nqc -raw1 (remote %s, hold)", hex);
    log_output(st, line, "cmd");
    g_free(line);
}

static void remote_release_cb(GtkGestureClick *g, gint n, gdouble x, gdouble y, gpointer data) {
    if (!remote_active) return;
    remote_active = FALSE;
    remote_stop_pending = TRUE;
    if (!remote_busy) remote_next();
}

static void noop_cb(GtkButton *btn, gpointer data) { }

static GtkWidget *remote_hold_button(AppState *st, const char *label, const char *css,
                                      guint mask) {
    GtkWidget *b = chip_button(label, css, G_CALLBACK(noop_cb), st);
    g_object_set_data(G_OBJECT(b), "mask", GUINT_TO_POINTER(mask));
    GtkGesture *g = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g), GTK_PHASE_CAPTURE);
    g_signal_connect(g, "pressed", G_CALLBACK(remote_press_cb), st);
    g_signal_connect(g, "released", G_CALLBACK(remote_release_cb), st);
    g_signal_connect(g, "cancel", G_CALLBACK(remote_release_cb), st);
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    return b;
}

static void remote_win_destroy_cb(GtkWidget *w, gpointer data) { remote_tower_close(); }

static void open_remote_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), T("Remote"));
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(st->window));
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "drawer");
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkWidget *logo = make_icon_widget(64);
    gtk_widget_set_halign(logo, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), logo);

    /* Messages 1-3 */
    GtkWidget *l1 = gtk_label_new("Message");
    gtk_widget_add_css_class(l1, "settings-label");
    gtk_box_append(GTK_BOX(box), l1);
    GtkWidget *msgs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(msgs, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(msgs), remote_button(st, "1", "qa-blue", 0x0100));
    gtk_box_append(GTK_BOX(msgs), remote_button(st, "2", "qa-blue", 0x0200));
    gtk_box_append(GTK_BOX(msgs), remote_button(st, "3", "qa-blue", 0x0400));
    gtk_box_append(GTK_BOX(box), msgs);

    /* Motors A, B, C: forward on top, backward below */
    const char *names[3] = { "A", "B", "C" };
    const guint fwd[3] = { 0x0800, 0x1000, 0x2000 };
    const guint rev[3] = { 0x4000, 0x8000, 0x0001 };
    GtkWidget *motors = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_halign(motors, GTK_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_append(GTK_BOX(col), remote_hold_button(st, "▲", "qa-green", fwd[i]));
        GtkWidget *n = gtk_label_new(names[i]);
        gtk_widget_add_css_class(n, "console-label");
        gtk_box_append(GTK_BOX(col), n);
        gtk_box_append(GTK_BOX(col), remote_hold_button(st, "▼", "qa-green", rev[i]));
        gtk_box_append(GTK_BOX(motors), col);
    }
    gtk_box_append(GTK_BOX(box), motors);

    /* Programs 1-5 */
    GtkWidget *l2 = gtk_label_new("Program");
    gtk_widget_add_css_class(l2, "settings-label");
    gtk_box_append(GTK_BOX(box), l2);
    GtkWidget *progs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(progs, GTK_ALIGN_CENTER);
    const char *pn[5] = { "P1", "P2", "P3", "P4", "P5" };
    for (int i = 0; i < 5; i++)
        gtk_box_append(GTK_BOX(progs), remote_button(st, pn[i], "qa-purple", 0x0002u << i));
    gtk_box_append(GTK_BOX(box), progs);

    /* Stop outputs and sound */
    GtkWidget *last = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(last, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(last), remote_button(st, "Stop", "qa-pink", 0x0040));
    gtk_box_append(GTK_BOX(last), remote_button(st, "Sound", "qa-orange", 0x0080));
    gtk_box_append(GTK_BOX(box), last);

    g_signal_connect(win, "destroy", G_CALLBACK(remote_win_destroy_cb), NULL);
    gtk_window_present(GTK_WINDOW(win));
}

/* ---------------------------------------------------------------------
 * VLL: the Scout sends a VLL code to a Micro Scout.
 * Message = opcode 0xE2 (0xEA on every second send, the RCX toggle bit),
 * source 2 (constant), then the code. Codes are BricxCC's MSVLL_* values.
 * --------------------------------------------------------------------- */
static gboolean vll_toggle = FALSE;
static gboolean vll_busy = FALSE;

static void vll_done(GObject *src, GAsyncResult *res, gpointer data) {
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    vll_busy = FALSE;
}

static void vll_btn_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    if (vll_busy) return;
    guint code = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "code"));
    char msg[16];
    snprintf(msg, sizeof(msg), "%s02%02X", vll_toggle ? "EA" : "E2", code);
    vll_toggle = !vll_toggle;

    remote_tower_close();
    const char *argv[] = { NQC_CMD, "-TScout", "-raw1", msg, NULL };
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    const char *port = gtk_editable_get_text(GTK_EDITABLE(st->tower_entry));
    if (port && *port) g_subprocess_launcher_setenv(l, "RCX_PORT", port, TRUE);
    GSubprocess *p = g_subprocess_launcher_spawnv(l, argv, NULL);
    g_object_unref(l);
    if (!p) return;
    vll_busy = TRUE;
    g_subprocess_wait_async(p, NULL, vll_done, NULL);
    g_object_unref(p);

    char *line = g_strdup_printf("$ nqc -TScout -raw1 %s (VLL %u)", msg, code);
    log_output(st, line, "cmd");
    g_free(line);
}

static GtkWidget *vll_button(AppState *st, const char *label, const char *css, guint code) {
    GtkWidget *b = chip_button(label, css, G_CALLBACK(vll_btn_cb), st);
    g_object_set_data(G_OBJECT(b), "code", GUINT_TO_POINTER(code));
    return b;
}

static void open_vll_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), T("VLL (Micro Scout)"));
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(st->window));
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "drawer");
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkWidget *hint = gtk_label_new("Point the Scout's light at the Micro Scout.");
    gtk_widget_add_css_class(hint, "console-label");
    gtk_box_append(GTK_BOX(box), hint);

    /* Direct commands: motor forward / backward */
    GtkWidget *move = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(move, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(move), vll_button(st, "▲", "qa-green", 0));
    gtk_box_append(GTK_BOX(move), vll_button(st, "▼", "qa-green", 1));
    gtk_box_append(GTK_BOX(box), move);

    /* Direct commands: beeps 1-5 are codes 4-8 */
    GtkWidget *l1 = gtk_label_new("Beep");
    gtk_widget_add_css_class(l1, "settings-label");
    gtk_box_append(GTK_BOX(box), l1);
    GtkWidget *beeps = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(beeps, GTK_ALIGN_CENTER);
    const char *bn[5] = { "1", "2", "3", "4", "5" };
    for (int i = 0; i < 5; i++)
        gtk_box_append(GTK_BOX(beeps), vll_button(st, bn[i], "qa-orange", 4 + i));
    gtk_box_append(GTK_BOX(box), beeps);

    gtk_window_present(GTK_WINDOW(win));
}

/* ---------------------------------------------------------------------
 * Main window
 * --------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * Firmware sheet: choose the file, then watch the progress.
 * nqc prints one '*' per 200-byte block, so counting them gives a real bar.
 * --------------------------------------------------------------------- */

typedef struct {
    AppState *st;
    GtkWidget *win, *stack;
    GtkWidget *title, *subtitle;
    GtkWidget *opt[3];
    GtkWidget *other_desc;
    char *other_path;
    GtkWidget *err_label;
    GtkWidget *bar, *pct_label, *eta_label, *warn, *warn_text;
    GtkWidget *cancel_btn, *close_btn, *again_btn;
    GString *seen;
    int stars, total;
    gint64 t0;
    guint tick;
    gboolean sending, cancelled;
    char *file;
} FwSheet;

static char *find_firmware(const char *name) {
    char *dirs[] = {
        g_build_filename(g_get_home_dir(), ".local", "brickcode", "share", "firmware", NULL),
        g_strdup("/usr/local/share/brickcode-studio/firmware"),
        g_strdup("/usr/share/brickcode-studio/firmware"),
        NULL };
    char *found = NULL;
    for (int i = 0; dirs[i]; i++) {
        if (!found) {
            char *p = g_build_filename(dirs[i], name, NULL);
            if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) found = p;
            else g_free(p);
        }
        g_free(dirs[i]);
    }
    return found;
}

static void fw_set_head(FwSheet *f, const char *title, const char *sub) {
    gtk_label_set_text(GTK_LABEL(f->title), title);
    gtk_label_set_text(GTK_LABEL(f->subtitle), sub);
}

static void fw_update(FwSheet *f) {
    if (!f->sending) return;
    double frac = f->total > 0 ? MIN(1.0, (double)f->stars / f->total) : 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(f->bar), frac);
    char buf[160];
    if (f->total > 0)
        snprintf(buf, sizeof(buf), T("%d%%  ·  block %d of %d"),
                 (int)(frac * 100), MIN(f->stars, f->total), f->total);
    else
        snprintf(buf, sizeof(buf), "%s", T("Starting…"));
    gtk_label_set_text(GTK_LABEL(f->pct_label), buf);
    buf[0] = 0;
    if (f->total > 0 && f->stars >= 3) {
        double elapsed = (g_get_monotonic_time() - f->t0) / 1e6;
        int left = (int)(elapsed * (f->total - f->stars) / f->stars);
        if (left < 60) snprintf(buf, sizeof(buf), "%s", T("less than a minute left"));
        else snprintf(buf, sizeof(buf), T("about %d min left"), (left + 30) / 60);
    }
    gtk_label_set_text(GTK_LABEL(f->eta_label), buf);
}

static gboolean fw_tick(gpointer data) {
    FwSheet *f = data;
    fw_update(f);
    if (!f->sending) { f->tick = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void fw_chars(const char *data, gsize n, gpointer user) {
    FwSheet *f = user;
    for (gsize i = 0; i < n; i++) if (data[i] == '*') f->stars++;
    if (f->total == 0 && f->seen->len < 2048) {
        g_string_append_len(f->seen, data, n);
        const char *b = strchr(f->seen->str, '[');
        int bytes = 0;
        if (b && sscanf(b, "[%d bytes]", &bytes) == 1 && bytes > 0)
            f->total = (bytes + 199) / 200;
    }
    fw_update(f);
}

static void fw_done(int code, gpointer user) {
    FwSheet *f = user;
    f->sending = FALSE;
    if (f->tick) { g_source_remove(f->tick); f->tick = 0; }
    gtk_widget_set_visible(f->cancel_btn, FALSE);
    gtk_widget_set_visible(f->close_btn, TRUE);
    if (code == 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(f->bar), 1.0);
        gtk_label_set_text(GTK_LABEL(f->pct_label), "100%");
        gtk_label_set_text(GTK_LABEL(f->eta_label), "");
        gtk_widget_set_visible(f->warn, FALSE);
        fw_set_head(f, T("Firmware installed"), f->file ? f->file : "");
    } else {
        gtk_label_set_text(GTK_LABEL(f->warn_text), f->cancelled
            ? T("Stopped. The RCX may have no firmware now; install it again before using the brick.")
            : T("The RCX did not answer. Keep it close to the tower and still, check the batteries, "
                "unplug and plug the tower again, and try again."));
        gtk_widget_set_visible(f->warn, TRUE);
        gtk_widget_set_visible(f->again_btn, TRUE);
        fw_set_head(f, f->cancelled ? T("Firmware stopped") : T("Firmware failed"),
                    T("See the console for the details."));
    }
}

static void fw_close_cb(GtkButton *b, gpointer user) {
    FwSheet *f = user;
    gtk_window_close(GTK_WINDOW(f->win));
}

static void fw_cancel_cb(GtkButton *b, gpointer user) {
    FwSheet *f = user;
    if (f->sending) {
        f->cancelled = TRUE;
        cancel_cb(NULL, f->st);
    } else {
        gtk_window_close(GTK_WINDOW(f->win));
    }
}

static void fw_again_cb(GtkButton *b, gpointer user) {
    FwSheet *f = user;
    gtk_stack_set_visible_child_name(GTK_STACK(f->stack), "choose");
    gtk_widget_set_visible(f->again_btn, FALSE);
    gtk_widget_set_visible(f->close_btn, FALSE);
    gtk_widget_set_visible(f->cancel_btn, TRUE);
    fw_set_head(f, T("Install RCX firmware"),
        T("The RCX needs its firmware once, and loses it when the batteries are removed. "
          "Choose the file for your brick."));
}

static void fw_install_cb(GtkButton *b, gpointer user) {
    FwSheet *f = user;
    AppState *st = f->st;
    int sel = 0;
    for (int i = 0; i < 3; i++)
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(f->opt[i]))) sel = i;
    const char *target = sel == 0 ? "RCX2" : sel == 1 ? "RCX" : NULL;
    char *path = sel == 0 ? find_firmware("firm0328.lgo")
               : sel == 1 ? find_firmware("firm0309.lgo")
               : g_strdup(f->other_path);
    if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        gtk_label_set_text(GTK_LABEL(f->err_label), T("That firmware file was not found."));
        g_free(path);
        return;
    }
    if (target)
        for (guint i = 0; i < G_N_ELEMENTS(NQC_TARGETS); i++)
            if (strcmp(NQC_TARGETS[i].name, target) == 0)
                gtk_drop_down_set_selected(GTK_DROP_DOWN(st->target_dropdown), i);
    gtk_editable_set_text(GTK_EDITABLE(st->fw_entry), path);

    g_string_truncate(f->seen, 0);
    f->stars = f->total = 0;
    f->cancelled = FALSE;
    f->t0 = g_get_monotonic_time();
    st->on_chars = fw_chars;
    st->on_done = fw_done;
    st->tap_data = f;
    log_output(st, "Flashing firmware takes a while. Keep the RCX pointed at the "
                   "tower and do not move it.", "info");
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, "-firmware");
    g_ptr_array_add(args, path);
    gboolean ok = run_nqc(st, args, TIMEOUT_FIRMWARE, "Firmware sent.", "Sending firmware…");
    g_ptr_array_free(args, TRUE);
    if (!ok) {
        st->on_chars = NULL; st->on_done = NULL; st->tap_data = NULL;
        gtk_label_set_text(GTK_LABEL(f->err_label),
            T("Could not start nqc. See the console."));
        g_free(path);
        return;
    }
    g_free(f->file);
    char *base = g_path_get_basename(path);
    f->file = g_strdup_printf("%s → %s", base,
        NQC_TARGETS[gtk_drop_down_get_selected(GTK_DROP_DOWN(st->target_dropdown))].name);
    g_free(base);
    g_free(path);
    f->sending = TRUE;
    gtk_label_set_text(GTK_LABEL(f->err_label), "");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(f->bar), 0.0);
    gtk_label_set_text(GTK_LABEL(f->warn_text),
        T("Do not move the RCX. Keep it pointed at the tower until this window says it is done. "
          "This takes about 3 to 5 minutes."));
    gtk_widget_set_visible(f->warn, TRUE);
    gtk_widget_set_visible(f->cancel_btn, TRUE);
    gtk_widget_set_visible(f->close_btn, FALSE);
    gtk_widget_set_visible(f->again_btn, FALSE);
    fw_set_head(f, T("Installing firmware…"), f->file);
    gtk_stack_set_visible_child_name(GTK_STACK(f->stack), "send");
    fw_update(f);
    f->tick = g_timeout_add_seconds(1, fw_tick, f);
}

static void fw_other_picked(GObject *src, GAsyncResult *res, gpointer user) {
    FwSheet *f = user;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &error);
    g_clear_error(&error);
    if (!file) {
        if (!f->other_path) gtk_check_button_set_active(GTK_CHECK_BUTTON(f->opt[0]), TRUE);
        return;
    }
    g_free(f->other_path);
    f->other_path = g_file_get_path(file);
    g_object_unref(file);
    char *base = g_path_get_basename(f->other_path);
    gtk_label_set_text(GTK_LABEL(f->other_desc), base);
    g_free(base);
}

static void fw_other_toggled(GtkCheckButton *cb, gpointer user) {
    FwSheet *f = user;
    if (!gtk_check_button_get_active(cb) || f->other_path) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select firmware file");
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "RCX firmware (*.lgo, *.srec)");
    gtk_file_filter_add_pattern(flt, "*.lgo");
    gtk_file_filter_add_pattern(flt, "*.srec");
    GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(store, flt);
    g_object_unref(flt);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(store));
    g_object_unref(store);
    gtk_file_dialog_open(dialog, GTK_WINDOW(f->win), NULL, fw_other_picked, f);
}

static gboolean fw_close_request(GtkWindow *w, gpointer user) {
    FwSheet *f = user;
    if (f->sending) {            /* never leave nqc running behind a closed sheet */
        f->cancelled = TRUE;
        cancel_cb(NULL, f->st);
        return TRUE;
    }
    return FALSE;
}

static void fw_destroy(GtkWidget *w, gpointer user) {
    FwSheet *f = user;
    if (f->st->tap_data == f) {
        f->st->on_chars = NULL; f->st->on_done = NULL; f->st->tap_data = NULL;
    }
    if (f->tick) g_source_remove(f->tick);
    g_string_free(f->seen, TRUE);
    g_free(f->other_path);
    g_free(f->file);
    g_free(f);
}

static gboolean fw_key_cb(GtkEventControllerKey *c, guint keyval, guint code,
                           GdkModifierType state, gpointer user) {
    FwSheet *f = user;
    if (keyval == GDK_KEY_Escape && !f->sending) {
        gtk_window_close(GTK_WINDOW(f->win));
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *fw_option(const char *name, const char *desc, const char *tag,
                             GtkWidget **desc_out, GtkWidget *group) {
    GtkWidget *cb = gtk_check_button_new();
    gtk_widget_add_css_class(cb, "fw-opt");
    if (group) gtk_check_button_set_group(GTK_CHECK_BUTTON(cb), GTK_CHECK_BUTTON(group));
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(col, TRUE);
    GtkWidget *n = gtk_label_new(name);
    gtk_widget_add_css_class(n, "fw-opt-name");
    gtk_label_set_xalign(GTK_LABEL(n), 0.0);
    GtkWidget *d = gtk_label_new(desc);
    gtk_widget_add_css_class(d, "fw-opt-desc");
    gtk_label_set_xalign(GTK_LABEL(d), 0.0);
    gtk_box_append(GTK_BOX(col), n);
    gtk_box_append(GTK_BOX(col), d);
    gtk_box_append(GTK_BOX(row), col);
    if (tag) {
        GtkWidget *t = gtk_label_new(tag);
        gtk_widget_add_css_class(t, "fw-tag");
        gtk_widget_set_valign(t, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), t);
    }
    gtk_check_button_set_child(GTK_CHECK_BUTTON(cb), row);
    if (desc_out) *desc_out = d;
    return cb;
}

static void open_firmware_sheet_cb(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;
    if (st->running) {
        log_output(st, "A command is already running. Wait for it or press Cancel.", "err");
        return;
    }
    FwSheet *f = g_new0(FwSheet, 1);
    f->st = st;
    f->seen = g_string_new(NULL);

    f->win = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(f->win), GTK_WINDOW(st->window));
    gtk_window_set_modal(GTK_WINDOW(f->win), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(f->win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(f->win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(f->win), 580, -1);
    gtk_widget_add_css_class(f->win, "fw-win");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(box, "fw-sheet");
    gtk_window_set_child(GTK_WINDOW(f->win), box);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_append(GTK_BOX(head), make_icon_widget(56));
    GtkWidget *hcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(hcol, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(hcol, TRUE);
    f->title = gtk_label_new("Install RCX firmware");
    gtk_widget_add_css_class(f->title, "fw-title");
    gtk_label_set_xalign(GTK_LABEL(f->title), 0.0);
    f->subtitle = gtk_label_new(
        "The RCX needs its firmware once, and loses it when the batteries are removed. "
        "Choose the file for your brick.");
    gtk_widget_add_css_class(f->subtitle, "fw-sub");
    gtk_label_set_xalign(GTK_LABEL(f->subtitle), 0.0);
    gtk_label_set_wrap(GTK_LABEL(f->subtitle), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(f->subtitle), 56);
    gtk_box_append(GTK_BOX(hcol), f->title);
    gtk_box_append(GTK_BOX(hcol), f->subtitle);
    gtk_box_append(GTK_BOX(head), hcol);
    gtk_box_append(GTK_BOX(box), head);

    f->stack = gtk_stack_new();
    gtk_box_append(GTK_BOX(box), f->stack);

    /* ---- page 1: choose ---- */
    GtkWidget *p1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    char *fw1 = find_firmware("firm0328.lgo");
    char *fw2 = find_firmware("firm0309.lgo");
    f->opt[0] = fw_option("firm0328.lgo", fw1 ? "RCX 2.0 · target RCX2" : "not found in the install folder",
                          "Recommended", NULL, NULL);
    f->opt[1] = fw_option("firm0309.lgo", fw2 ? "RCX 1.0 · target RCX" : "not found in the install folder",
                          NULL, NULL, f->opt[0]);
    f->opt[2] = fw_option("Choose another .lgo file…", "For example a fast firmware or a newer one",
                          NULL, &f->other_desc, f->opt[0]);
    gtk_widget_set_sensitive(f->opt[0], fw1 != NULL);
    gtk_widget_set_sensitive(f->opt[1], fw2 != NULL);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(f->opt[fw1 ? 0 : fw2 ? 1 : 2]), TRUE);
    g_signal_connect(f->opt[2], "toggled", G_CALLBACK(fw_other_toggled), f);
    for (int i = 0; i < 3; i++) gtk_box_append(GTK_BOX(p1), f->opt[i]);
    g_free(fw1);
    g_free(fw2);

    GtkWidget *check = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(check, "fw-check");
    gtk_box_append(GTK_BOX(check), gtk_label_new("•  New batteries in the RCX"));
    gtk_box_append(GTK_BOX(check), gtk_label_new("•  RCX on, pointing at the tower, close and still"));
    const char *dl = gtk_label_get_text(GTK_LABEL(st->dev_label));
    const char *port = gtk_editable_get_text(GTK_EDITABLE(st->tower_entry));
    char tline[200];
    if (strstr(dl, "tower"))
        snprintf(tline, sizeof(tline), T("✓  Tower connected · port %s"),
                 (port && *port) ? port : "auto");
    else
        snprintf(tline, sizeof(tline), "%s", T("⚠  Tower not detected: check the USB cable"));
    GtkWidget *tl = gtk_label_new(tline);
    gtk_box_append(GTK_BOX(check), tl);
    for (GtkWidget *c = gtk_widget_get_first_child(check); c; c = gtk_widget_get_next_sibling(c))
        gtk_label_set_xalign(GTK_LABEL(c), 0.0);
    gtk_box_append(GTK_BOX(p1), check);

    f->err_label = gtk_label_new("");
    gtk_widget_add_css_class(f->err_label, "fw-err");
    gtk_label_set_xalign(GTK_LABEL(f->err_label), 0.0);
    gtk_box_append(GTK_BOX(p1), f->err_label);

    GtkWidget *b1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(b1, GTK_ALIGN_END);
    GtkWidget *c1 = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(c1, "browse-btn");
    GtkWidget *go = gtk_button_new_with_label("Install firmware");
    gtk_widget_add_css_class(go, "pill-btn");
    gtk_widget_add_css_class(go, "pill-blue");
    g_signal_connect(c1, "clicked", G_CALLBACK(fw_close_cb), f);
    g_signal_connect(go, "clicked", G_CALLBACK(fw_install_cb), f);
    gtk_box_append(GTK_BOX(b1), c1);
    gtk_box_append(GTK_BOX(b1), go);
    gtk_box_append(GTK_BOX(p1), b1);
    gtk_stack_add_named(GTK_STACK(f->stack), p1, "choose");

    /* ---- page 2: sending ---- */
    GtkWidget *p2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    f->bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(f->bar, "fw-bar");
    gtk_box_append(GTK_BOX(p2), f->bar);
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    f->pct_label = gtk_label_new("");
    gtk_widget_add_css_class(f->pct_label, "fw-sub");
    gtk_widget_set_hexpand(f->pct_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(f->pct_label), 0.0);
    f->eta_label = gtk_label_new("");
    gtk_widget_add_css_class(f->eta_label, "fw-sub");
    gtk_box_append(GTK_BOX(info), f->pct_label);
    gtk_box_append(GTK_BOX(info), f->eta_label);
    gtk_box_append(GTK_BOX(p2), info);
    f->warn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(f->warn, "fw-warn");
    f->warn_text = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(f->warn_text), TRUE);
    gtk_label_set_xalign(GTK_LABEL(f->warn_text), 0.0);
    gtk_label_set_max_width_chars(GTK_LABEL(f->warn_text), 60);
    gtk_box_append(GTK_BOX(f->warn), f->warn_text);
    gtk_box_append(GTK_BOX(p2), f->warn);
    GtkWidget *b2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(b2, GTK_ALIGN_END);
    f->cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(f->cancel_btn, "browse-btn");
    f->again_btn = gtk_button_new_with_label("Try again");
    gtk_widget_add_css_class(f->again_btn, "browse-btn");
    f->close_btn = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(f->close_btn, "pill-btn");
    gtk_widget_add_css_class(f->close_btn, "pill-blue");
    g_signal_connect(f->cancel_btn, "clicked", G_CALLBACK(fw_cancel_cb), f);
    g_signal_connect(f->again_btn, "clicked", G_CALLBACK(fw_again_cb), f);
    g_signal_connect(f->close_btn, "clicked", G_CALLBACK(fw_close_cb), f);
    gtk_box_append(GTK_BOX(b2), f->again_btn);
    gtk_box_append(GTK_BOX(b2), f->cancel_btn);
    gtk_box_append(GTK_BOX(b2), f->close_btn);
    gtk_widget_set_visible(f->again_btn, FALSE);
    gtk_widget_set_visible(f->close_btn, FALSE);
    gtk_box_append(GTK_BOX(p2), b2);
    gtk_stack_add_named(GTK_STACK(f->stack), p2, "send");
    gtk_stack_set_visible_child_name(GTK_STACK(f->stack), "choose");

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(fw_key_cb), f);
    gtk_widget_add_controller(f->win, kc);
    g_signal_connect(f->win, "close-request", G_CALLBACK(fw_close_request), f);
    g_signal_connect(f->win, "destroy", G_CALLBACK(fw_destroy), f);
    gtk_window_present(GTK_WINDOW(f->win));
}

static void lang_changed_cb(GObject *dropdown, GParamSpec *pspec,
                             gpointer user_data) {
    AppState *st = user_data;
    g_lang = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown)) == 1 ? LANG_ES : LANG_EN;
    tr_apply(st->window);
    /* The theme names are list items, so rebuild them in the new language */
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->theme_dropdown));
    const char *names[] = { T("Dark"), T("Light"), NULL };
    GtkStringList *model = GTK_STRING_LIST(
        gtk_drop_down_get_model(GTK_DROP_DOWN(st->theme_dropdown)));
    g_signal_handlers_block_matched(st->theme_dropdown, G_SIGNAL_MATCH_FUNC, 0, 0,
                                    NULL, theme_changed_cb, NULL);
    gtk_string_list_splice(model, 0, 2, names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->theme_dropdown), sel);
    g_signal_handlers_unblock_matched(st->theme_dropdown, G_SIGNAL_MATCH_FUNC, 0, 0,
                                      NULL, theme_changed_cb, NULL);
    update_title(st);
    set_status(st, "Ready.", "status-ok");
    device_poll(st);
}

/* ---- Sidebar helpers ---- */
static GtkWidget *side_button(const char *label, const char *sq_class,
                              GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, "side-btn");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *sq = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sq, "side-sq");
    gtk_widget_add_css_class(sq, sq_class);
    gtk_widget_set_valign(sq, GTK_ALIGN_CENTER);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_box_append(GTK_BOX(row), sq);
    gtk_box_append(GTK_BOX(row), l);
    gtk_button_set_child(GTK_BUTTON(b), row);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static GtkWidget *side_section(const char *label) {
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_add_css_class(l, "side-sec");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    return l;
}

static GtkWidget *side_brick_row(const char *label, GtkWidget **dot_out) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_widget_add_css_class(row, "side-brick");
    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "status-dot");
    gtk_widget_add_css_class(dot, "status-idle");
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(dot, 10, 10);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_box_append(GTK_BOX(row), dot);
    gtk_box_append(GTK_BOX(row), l);
    *dot_out = dot;
    return row;
}

static void seg_toggled_cb(GtkToggleButton *tb, gpointer user_data) {
    AppState *st = user_data;
    if (!gtk_toggle_button_get_active(tb)) return;
    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(tb), "idx"));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->mode_dropdown), idx);
}

static void activate(GtkApplication *app, gpointer user_data) {
    AppState *st = g_new0(AppState, 1);
    st->app = app;
    st->cmd_history = g_ptr_array_new_with_free_func(g_free);
    st->dark_theme = TRUE;
    st->mode = MODE_NQC;

    /* Language: saved choice, otherwise follow the system locale */
    g_lang = lang_from_env();
    {
        GKeyFile *lkf = g_key_file_new();
        char *lpath = settings_path();
        if (g_key_file_load_from_file(lkf, lpath, G_KEY_FILE_NONE, NULL) &&
            g_key_file_has_key(lkf, "ide", "lang", NULL))
            g_lang = kf_int(lkf, "ide", "lang", 0) == 1 ? LANG_ES : LANG_EN;
        g_free(lpath);
        g_key_file_free(lkf);
    }

    gtk_window_set_default_icon_name("brickcode-studio");
    apply_omarchy_style(st);
    watch_omarchy_theme(st);

    st->window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(st->window), 1400, 860);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(st->window), root);

    /* ---- Top bar: project tab, language, brick status, actions ---- */
    GtkWidget *topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(topbar, "topbar");

    GtkWidget *about_btn = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(about_btn), make_icon_widget(34));
    gtk_widget_add_css_class(about_btn, "home-btn");
    gtk_widget_set_tooltip_text(about_btn, "About " APP_NAME);
    gtk_box_append(GTK_BOX(topbar), about_btn);

    GtkWidget *tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(tab, "tab-chip");
    st->badge_label = gtk_label_new("NQC");
    gtk_widget_add_css_class(st->badge_label, "lang-pill");
    st->filename_label = gtk_label_new("untitled");
    gtk_widget_add_css_class(st->filename_label, "filename-label");
    gtk_label_set_ellipsize(GTK_LABEL(st->filename_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(st->filename_label), 28);
    GtkWidget *kebab = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(kebab), "⋮");
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(kebab), FALSE);
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(kebab), FALSE);
    gtk_widget_add_css_class(kebab, "kebab-btn");
    GtkWidget *file_pop = gtk_popover_new();
    gtk_widget_add_css_class(file_pop, "snippet-pop");
    GtkWidget *file_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(file_list), menu_item("New", G_CALLBACK(new_file_cb), st, file_pop));
    gtk_box_append(GTK_BOX(file_list), menu_item("Open…", G_CALLBACK(open_file_cb), st, file_pop));
    gtk_box_append(GTK_BOX(file_list), menu_item("Save", G_CALLBACK(save_file_cb), st, file_pop));
    gtk_box_append(GTK_BOX(file_list), menu_item("Save as…", G_CALLBACK(save_as_cb), st, file_pop));
    gtk_popover_set_child(GTK_POPOVER(file_pop), file_list);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(kebab), file_pop);
    gtk_box_append(GTK_BOX(tab), st->badge_label);
    gtk_box_append(GTK_BOX(tab), st->filename_label);
    gtk_box_append(GTK_BOX(tab), kebab);
    gtk_box_append(GTK_BOX(topbar), tab);

    GtkWidget *top_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(top_spacer, TRUE);
    gtk_box_append(GTK_BOX(topbar), top_spacer);

    const char *mode_opts[] = { MODES[0].name, MODES[1].name, NULL };
    st->mode_dropdown = gtk_drop_down_new_from_strings(mode_opts);
    gtk_widget_add_css_class(st->mode_dropdown, "mode-dd");
    gtk_widget_set_valign(st->mode_dropdown, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(st->mode_dropdown, FALSE);   /* kept as the state holder */
    gtk_box_append(GTK_BOX(topbar), st->mode_dropdown);

    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(seg, "seg");
    gtk_widget_set_valign(seg, GTK_ALIGN_CENTER);
    for (guint i = 0; i < 2; i++) {
        st->seg_btn[i] = gtk_toggle_button_new_with_label(MODES[i].name);
        g_object_set_data(G_OBJECT(st->seg_btn[i]), "idx", GUINT_TO_POINTER(i));
        if (i) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(st->seg_btn[1]),
                                            GTK_TOGGLE_BUTTON(st->seg_btn[0]));
        gtk_box_append(GTK_BOX(seg), st->seg_btn[i]);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->seg_btn[0]), TRUE);
    for (guint i = 0; i < 2; i++)
        g_signal_connect(st->seg_btn[i], "toggled", G_CALLBACK(seg_toggled_cb), st);
    gtk_box_append(GTK_BOX(topbar), seg);

    const char *lang_opts[] = { "English", "Español", NULL };
    st->lang_dropdown = gtk_drop_down_new_from_strings(lang_opts);
    gtk_widget_add_css_class(st->lang_dropdown, "mode-dd");
    gtk_widget_set_valign(st->lang_dropdown, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(st->lang_dropdown, "Language");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->lang_dropdown), g_lang);
    gtk_box_append(GTK_BOX(topbar), st->lang_dropdown);

    GtkWidget *dev_chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(dev_chip, "dev-chip");
    st->dev_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(st->dev_dot, "status-dot");
    gtk_widget_add_css_class(st->dev_dot, "status-idle");
    gtk_widget_set_valign(st->dev_dot, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(st->dev_dot, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(st->dev_dot, 10, 10);
    st->dev_label = gtk_label_new("No brick connected");
    gtk_widget_add_css_class(st->dev_label, "dev-label");
    gtk_box_append(GTK_BOX(dev_chip), st->dev_dot);
    gtk_box_append(GTK_BOX(dev_chip), st->dev_label);
    gtk_box_append(GTK_BOX(topbar), dev_chip);

    st->cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(st->cancel_btn, "browse-btn");
    gtk_widget_set_sensitive(st->cancel_btn, FALSE);
    st->btn_upload = gtk_button_new_with_label("⬇  Download");
    gtk_widget_add_css_class(st->btn_upload, "pill-btn");
    gtk_widget_add_css_class(st->btn_upload, "pill-blue");
    st->btn_run = gtk_button_new_with_label("▶  Run  (F5)");
    gtk_widget_add_css_class(st->btn_run, "pill-btn");
    gtk_widget_add_css_class(st->btn_run, "pill-green");
    gtk_box_append(GTK_BOX(topbar), st->cancel_btn);
    gtk_box_append(GTK_BOX(topbar), st->btn_upload);
    gtk_box_append(GTK_BOX(topbar), st->btn_run);
    gtk_box_append(GTK_BOX(root), topbar);

    st->subtitle_label = gtk_label_new("");

    /* ---- Line numbers + editor ---- */
    st->lineno_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(st->lineno_view, FALSE);
    gtk_text_view_set_cursor_visible(st->lineno_view, FALSE);
    gtk_text_view_set_monospace(st->lineno_view, TRUE);
    gtk_text_view_set_justification(st->lineno_view, GTK_JUSTIFY_RIGHT);
    gtk_widget_add_css_class(GTK_WIDGET(st->lineno_view), "line-numbers");
    gtk_widget_set_size_request(GTK_WIDGET(st->lineno_view), 44, -1);
    gtk_widget_set_margin_start(GTK_WIDGET(st->lineno_view), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(st->lineno_view), 6);
    gtk_widget_set_margin_top(GTK_WIDGET(st->lineno_view), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(st->lineno_view), 8);
    st->lineno_buffer = gtk_text_view_get_buffer(st->lineno_view);
    gtk_text_buffer_set_text(st->lineno_buffer, "1", -1);

    st->editor = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_monospace(st->editor, TRUE);
    gtk_text_view_set_wrap_mode(st->editor, GTK_WRAP_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(st->editor), "code-editor");
    gtk_widget_set_hexpand(GTK_WIDGET(st->editor), TRUE);
    gtk_widget_set_margin_start(GTK_WIDGET(st->editor), 4);
    gtk_widget_set_margin_end(GTK_WIDGET(st->editor), 8);
    gtk_widget_set_margin_top(GTK_WIDGET(st->editor), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(st->editor), 8);
    st->buffer = gtk_text_view_get_buffer(st->editor);

    st->tag_keyword = gtk_text_buffer_create_tag(st->buffer, "keyword",
        "foreground", "#569cd6", NULL);
    st->tag_builtin = gtk_text_buffer_create_tag(st->buffer, "builtin",
        "foreground", "#4ec9b0", NULL);
    st->tag_preproc = gtk_text_buffer_create_tag(st->buffer, "preproc",
        "foreground", "#c586c0", NULL);
    st->tag_number = gtk_text_buffer_create_tag(st->buffer, "number",
        "foreground", "#b5cea8", NULL);
    st->tag_string = gtk_text_buffer_create_tag(st->buffer, "string",
        "foreground", "#ce9178", NULL);
    st->tag_comment = gtk_text_buffer_create_tag(st->buffer, "comment",
        "foreground", "#6a9955", "style", PANGO_STYLE_ITALIC, NULL);
    st->tag_errline = gtk_text_buffer_create_tag(st->buffer, "errline",
        "background", "#7f1d1d", NULL);

    GtkWidget *editor_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(editor_hbox), GTK_WIDGET(st->lineno_view));
    gtk_box_append(GTK_BOX(editor_hbox), GTK_WIDGET(st->editor));

    GtkWidget *editor_panel = gtk_scrolled_window_new();
    gtk_widget_add_css_class(editor_panel, "panel");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(editor_panel), editor_hbox);
    gtk_widget_set_vexpand(editor_panel, TRUE);
    gtk_widget_set_hexpand(editor_panel, TRUE);

    /* ---- Console ---- */
    st->output = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_monospace(st->output, TRUE);
    gtk_text_view_set_editable(st->output, FALSE);
    gtk_text_view_set_wrap_mode(st->output, GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(GTK_WIDGET(st->output), "console-editor");
    gtk_widget_set_margin_start(GTK_WIDGET(st->output), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(st->output), 8);
    gtk_widget_set_margin_top(GTK_WIDGET(st->output), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(st->output), 8);
    st->output_buffer = gtk_text_view_get_buffer(st->output);
    gtk_text_buffer_create_tag(st->output_buffer, "cmd", "foreground", "#61DAFB", NULL);
    gtk_text_buffer_create_tag(st->output_buffer, "ok", "foreground", "#4ADE80",
                                "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(st->output_buffer, "err", "foreground", "#F87171",
                                "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(st->output_buffer, "dim", "foreground", "#8A90A6", NULL);
    gtk_text_buffer_create_tag(st->output_buffer, "info", "foreground", "#FCD34D", NULL);

    GtkWidget *console_panel = gtk_scrolled_window_new();
    gtk_widget_add_css_class(console_panel, "panel");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(console_panel),
                                   GTK_WIDGET(st->output));
    gtk_widget_set_vexpand(console_panel, TRUE);
    gtk_widget_set_hexpand(console_panel, TRUE);

    GtkWidget *btn_clear = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(btn_clear, "browse-btn");
    GtkWidget *console_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(console_head, "console-head");
    GtkWidget *console_label = gtk_label_new("Console");
    gtk_widget_add_css_class(console_label, "console-label");
    gtk_widget_set_hexpand(console_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(console_label), 0.0);
    gtk_box_append(GTK_BOX(console_head), console_label);
    gtk_box_append(GTK_BOX(console_head), btn_clear);
    st->console_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(st->console_box), console_head);
    gtk_box_append(GTK_BOX(st->console_box), console_panel);
    gtk_widget_set_vexpand(console_panel, TRUE);

    /* Right drawer: settings, utilities and the raw command line */
    GtkWidget *drawer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(drawer_box, "drawer");
    GtkWidget *drawer_title = gtk_label_new("Settings");
    gtk_widget_add_css_class(drawer_title, "console-label");
    gtk_label_set_xalign(GTK_LABEL(drawer_title), 0.0);
    gtk_box_append(GTK_BOX(drawer_box), drawer_title);

    /* ---- Settings row: NQC and NXC variants share the space ---- */
    GtkWidget *settings_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(settings_row, "settings-row");

    /* NQC: target + program slot */
    st->nqc_settings = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    const char *target_names[G_N_ELEMENTS(NQC_TARGETS) + 1];
    for (guint i = 0; i < G_N_ELEMENTS(NQC_TARGETS); i++)
        target_names[i] = NQC_TARGETS[i].label;
    target_names[G_N_ELEMENTS(NQC_TARGETS)] = NULL;
    st->target_dropdown = gtk_drop_down_new_from_strings(target_names);
    gtk_widget_add_css_class(st->target_dropdown, "settings-dd");
    st->target_note = gtk_label_new(NQC_TARGETS[0].desc);
    gtk_widget_add_css_class(st->target_note, "about-version");
    gtk_label_set_xalign(GTK_LABEL(st->target_note), 0.0);
    GtkWidget *target_col = settings_column("TARGET", st->target_dropdown);
    gtk_box_append(GTK_BOX(target_col), st->target_note);
    gtk_box_append(GTK_BOX(st->nqc_settings), target_col);

    const char *slot_names[] = { "1", "2", "3", "4", "5", NULL };
    st->slot_dropdown = gtk_drop_down_new_from_strings(slot_names);
    gtk_widget_add_css_class(st->slot_dropdown, "settings-dd");
    gtk_box_append(GTK_BOX(st->nqc_settings),
                   settings_column("PROGRAM SLOT", st->slot_dropdown));
    gtk_box_append(GTK_BOX(settings_row), st->nqc_settings);

    /* NXC: port (USB / Bluetooth) + fixed target */
    st->nxc_settings = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    const char *port_opts[] = { "USB", "Bluetooth", NULL };
    st->port_dropdown = gtk_drop_down_new_from_strings(port_opts);
    gtk_widget_add_css_class(st->port_dropdown, "settings-dd");
    st->bt_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->bt_entry), "/dev/rfcomm0");
    gtk_widget_add_css_class(st->bt_entry, "path-field");
    st->bt_revealer = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(st->bt_revealer), st->bt_entry);
    gtk_revealer_set_reveal_child(GTK_REVEALER(st->bt_revealer), FALSE);
    GtkWidget *port_col = settings_column("PORT", st->port_dropdown);
    gtk_box_append(GTK_BOX(port_col), st->bt_revealer);
    gtk_box_append(GTK_BOX(st->nxc_settings), port_col);

    const char *nxt_opts[] = { "NXT", NULL };
    GtkWidget *nxt_dd = gtk_drop_down_new_from_strings(nxt_opts);
    gtk_widget_add_css_class(nxt_dd, "settings-dd");
    GtkWidget *nxt_desc = gtk_label_new("Standard NXT firmware");
    gtk_widget_add_css_class(nxt_desc, "about-version");
    gtk_label_set_xalign(GTK_LABEL(nxt_desc), 0.0);
    GtkWidget *nxt_col = settings_column("TARGET", nxt_dd);
    gtk_box_append(GTK_BOX(nxt_col), nxt_desc);
    gtk_box_append(GTK_BOX(st->nxc_settings), nxt_col);
    gtk_box_append(GTK_BOX(settings_row), st->nxc_settings);

    /* Theme (both languages) */
    const char *theme_opts[] = { T("Dark"), T("Light"), NULL };
    st->theme_dropdown = gtk_drop_down_new_from_strings(theme_opts);
    gtk_widget_add_css_class(st->theme_dropdown, "settings-dd");
    gtk_box_append(GTK_BOX(settings_row), settings_column("EDITOR THEME", st->theme_dropdown));
    gtk_box_append(GTK_BOX(drawer_box), settings_row);

    /* ---- Utilities (collapsible; contents depend on the language) ---- */
    GtkWidget *utils_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(utils_row, "utils-row");
    st->utils_expander = gtk_expander_new(NULL);
    GtkWidget *utils_title = gtk_label_new("Brick utilities");
    gtk_widget_add_css_class(utils_title, "console-label");
    gtk_expander_set_label_widget(GTK_EXPANDER(st->utils_expander), utils_title);
    gtk_widget_add_css_class(st->utils_expander, "utils-expander");
    GtkWidget *utils_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(utils_content, 8);

    /* NXC utilities */
    st->utils_nxc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(st->utils_nxc),
        chip_button("NXT Explorer", "qa-purple", G_CALLBACK(open_explorer_cb), st));
    gtk_box_append(GTK_BOX(st->utils_nxc),
        chip_button("Test tools", "qa-green", G_CALLBACK(test_tools_cb), st));
    gtk_box_append(GTK_BOX(st->utils_nxc),
        chip_button("Test NXT", "qa-green", G_CALLBACK(test_brick_cb), st));
    gtk_box_append(GTK_BOX(utils_content), st->utils_nxc);

    /* NQC utilities */
    st->utils_nqc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row1 = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(row1), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(row1), 3);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(row1), TRUE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(row1), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(row1), 6);
    gtk_flow_box_append(GTK_FLOW_BOX(row1), nqc_flag_button(st, "Run", "qa-blue", "-run"));
    gtk_flow_box_append(GTK_FLOW_BOX(row1), nqc_flag_button(st, "Clear brick", "qa-pink", "-clear"));
    gtk_flow_box_append(GTK_FLOW_BOX(row1), nqc_flag_button(st, "Datalog", "qa-purple", "-datalog"));
    gtk_flow_box_append(GTK_FLOW_BOX(row1), nqc_flag_button(st, "IR near", "qa-green", "-near"));
    gtk_flow_box_append(GTK_FLOW_BOX(row1), nqc_flag_button(st, "IR far", "qa-green", "-far"));
    gtk_box_append(GTK_BOX(st->utils_nqc), row1);

    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    st->fw_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->fw_entry), "firmware file (.lgo)");
    gtk_widget_add_css_class(st->fw_entry, "path-field");
    gtk_widget_set_hexpand(st->fw_entry, TRUE);
    GtkWidget *fw_pick = gtk_button_new_with_label("…");
    gtk_widget_add_css_class(fw_pick, "browse-btn");
    g_signal_connect(fw_pick, "clicked", G_CALLBACK(pick_firmware_cb), st);
    gtk_box_append(GTK_BOX(row2), st->fw_entry);
    gtk_box_append(GTK_BOX(row2), fw_pick);
    gtk_box_append(GTK_BOX(row2),
        chip_button("Send firmware", "qa-orange", G_CALLBACK(firmware_cb), st));
    gtk_box_append(GTK_BOX(st->utils_nqc), row2);

    GtkWidget *row3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *tower_lbl = gtk_label_new("Tower port");
    gtk_widget_add_css_class(tower_lbl, "settings-label");
    st->tower_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->tower_entry),
        "empty = let nqc decide  ·  usb for the USB tower, or a serial port");
    gtk_widget_add_css_class(st->tower_entry, "path-field");
    gtk_widget_set_hexpand(st->tower_entry, TRUE);
    GtkWidget *detect = gtk_button_new_with_label("Detect");
    gtk_widget_add_css_class(detect, "browse-btn");
    g_signal_connect(detect, "clicked", G_CALLBACK(detect_port_cb), st);
    gtk_box_append(GTK_BOX(row3), tower_lbl);
    gtk_box_append(GTK_BOX(row3), st->tower_entry);
    gtk_box_append(GTK_BOX(row3), detect);
    gtk_box_append(GTK_BOX(st->utils_nqc), row3);

    GtkWidget *row4 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sleep_lbl = gtk_label_new("Auto-shutoff");
    gtk_widget_add_css_class(sleep_lbl, "settings-label");
    st->sleep_spin = gtk_spin_button_new_with_range(0, 255, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->sleep_spin), 15);
    GtkWidget *min_lbl = gtk_label_new("min");
    gtk_widget_add_css_class(min_lbl, "settings-label");
    GtkWidget *apply_sleep = gtk_button_new_with_label("Apply");
    gtk_widget_add_css_class(apply_sleep, "browse-btn");
    g_signal_connect(apply_sleep, "clicked", G_CALLBACK(nqc_sleep_cb), st);
    gtk_box_append(GTK_BOX(row4), sleep_lbl);
    gtk_box_append(GTK_BOX(row4), st->sleep_spin);
    gtk_box_append(GTK_BOX(row4), min_lbl);
    gtk_box_append(GTK_BOX(row4), apply_sleep);
    gtk_box_append(GTK_BOX(st->utils_nqc), row4);

    GtkWidget *row5 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(row5),
        chip_button("Test tools", "qa-green", G_CALLBACK(test_tools_cb), st));
    gtk_box_append(GTK_BOX(row5),
        chip_button("Test brick", "qa-green", G_CALLBACK(test_brick_cb), st));
    gtk_box_append(GTK_BOX(row5),
        chip_button("Remote", "qa-blue", G_CALLBACK(open_remote_cb), st));
    gtk_box_append(GTK_BOX(row5),
        chip_button("VLL", "qa-purple", G_CALLBACK(open_vll_cb), st));
    gtk_box_append(GTK_BOX(row5),
        chip_button("Firmware…", "qa-orange", G_CALLBACK(open_firmware_sheet_cb), st));
    gtk_box_append(GTK_BOX(st->utils_nqc), row5);
    gtk_box_append(GTK_BOX(utils_content), st->utils_nqc);

    gtk_expander_set_child(GTK_EXPANDER(st->utils_expander), utils_content);
    gtk_box_append(GTK_BOX(utils_row), st->utils_expander);
    gtk_box_append(GTK_BOX(drawer_box), utils_row);

    /* ---- Raw command row ---- */
    GtkWidget *command_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(command_row, "command-row");
    st->cmd_prefix = gtk_label_new(NQC_CMD);
    gtk_widget_add_css_class(st->cmd_prefix, "filename-label");
    st->cmd_entry = gtk_entry_new();
    gtk_widget_add_css_class(st->cmd_entry, "command-entry");
    gtk_widget_set_hexpand(st->cmd_entry, TRUE);
    GtkWidget *btn_cmd_run = gtk_button_new_with_label("Run");
    gtk_widget_add_css_class(btn_cmd_run, "browse-btn");
    gtk_box_append(GTK_BOX(command_row), st->cmd_prefix);
    gtk_box_append(GTK_BOX(command_row), st->cmd_entry);
    gtk_box_append(GTK_BOX(command_row), btn_cmd_run);
    gtk_box_append(GTK_BOX(drawer_box), command_row);

    /* ---- Workspace: editor over console | edge buttons | drawer ---- */
    GtkWidget *center = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_hexpand(center, TRUE);
    gtk_paned_set_start_child(GTK_PANED(center), editor_panel);
    gtk_paned_set_resize_start_child(GTK_PANED(center), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(center), FALSE);
    gtk_paned_set_end_child(GTK_PANED(center), st->console_box);
    gtk_paned_set_resize_end_child(GTK_PANED(center), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(center), FALSE);
    gtk_paned_set_position(GTK_PANED(center), 520);

    GtkWidget *edge = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(edge, "edge");
    GtkWidget *gear = gtk_button_new_with_label("⚙");
    gtk_widget_set_tooltip_text(gear, "Settings and brick utilities");
    GtkWidget *term = gtk_button_new_with_label(">_");
    gtk_widget_set_tooltip_text(term, "Show / hide the console");
    gtk_widget_add_css_class(gear, "edge-btn");
    gtk_widget_add_css_class(term, "edge-btn");
    g_signal_connect(gear, "clicked", G_CALLBACK(toggle_drawer_cb), st);
    g_signal_connect(term, "clicked", G_CALLBACK(toggle_console_cb), st);
    gtk_box_append(GTK_BOX(edge), gear);
    gtk_box_append(GTK_BOX(edge), term);

    GtkWidget *drawer_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(drawer_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(drawer_scroll), drawer_box);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(drawer_scroll), 330);
    gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(drawer_scroll), 330);
    gtk_widget_set_size_request(drawer_scroll, 330, -1);
    gtk_widget_set_hexpand(drawer_scroll, FALSE);
    st->drawer = drawer_scroll;
    gtk_widget_set_visible(st->drawer, TRUE);

    /* ---- Left sidebar: bricks and tools ---- */
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 210, -1);
    gtk_box_append(GTK_BOX(sidebar), side_section("Bricks"));
    gtk_box_append(GTK_BOX(sidebar), side_brick_row("RCX tower", &st->side_rcx_dot));
    gtk_box_append(GTK_BOX(sidebar), side_brick_row("NXT", &st->side_nxt_dot));
    gtk_box_append(GTK_BOX(sidebar), side_section("Tools"));

    st->side_nqc_tools = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(st->side_nqc_tools),
        side_button("Remote", "sq-orange", G_CALLBACK(open_remote_cb), st));
    gtk_box_append(GTK_BOX(st->side_nqc_tools),
        side_button("VLL", "sq-blue", G_CALLBACK(open_vll_cb), st));
    GtkWidget *dl_btn = side_button("Datalog", "sq-pink", G_CALLBACK(nqc_util_cb), st);
    g_object_set_data(G_OBJECT(dl_btn), "nqc-flag", (gpointer)"-datalog");
    gtk_box_append(GTK_BOX(st->side_nqc_tools), dl_btn);
    gtk_box_append(GTK_BOX(st->side_nqc_tools),
        side_button("Firmware…", "sq-gray", G_CALLBACK(open_firmware_sheet_cb), st));
    gtk_box_append(GTK_BOX(st->side_nqc_tools),
        side_button("Test brick", "sq-green", G_CALLBACK(test_brick_cb), st));
    gtk_box_append(GTK_BOX(st->side_nqc_tools),
        side_button("Test tools", "sq-blue", G_CALLBACK(test_tools_cb), st));
    gtk_box_append(GTK_BOX(sidebar), st->side_nqc_tools);

    st->side_nxc_tools = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(st->side_nxc_tools),
        side_button("NXT Explorer", "sq-blue", G_CALLBACK(open_explorer_cb), st));
    gtk_box_append(GTK_BOX(st->side_nxc_tools),
        side_button("Test NXT", "sq-green", G_CALLBACK(test_brick_cb), st));
    gtk_box_append(GTK_BOX(st->side_nxc_tools),
        side_button("Test tools", "sq-gray", G_CALLBACK(test_tools_cb), st));
    gtk_box_append(GTK_BOX(sidebar), st->side_nxc_tools);

    GtkWidget *side_fill = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(side_fill, TRUE);
    gtk_box_append(GTK_BOX(sidebar), side_fill);
    gtk_box_append(GTK_BOX(sidebar),
        side_button("Settings", "sq-gray", G_CALLBACK(toggle_drawer_cb), st));

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_box_append(GTK_BOX(body), sidebar);
    gtk_box_append(GTK_BOX(body), center);
    gtk_box_append(GTK_BOX(body), edge);
    gtk_box_append(GTK_BOX(body), st->drawer);
    gtk_box_append(GTK_BOX(root), body);

    /* ---- Status bar ---- */
    GtkWidget *statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(statusbar, "statusbar");
    st->status_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(st->status_dot, "status-dot");
    gtk_widget_add_css_class(st->status_dot, "status-ok");
    gtk_widget_set_valign(st->status_dot, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(st->status_dot, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(st->status_dot, 10, 10);
    st->status_label = gtk_label_new("Ready.");
    gtk_widget_add_css_class(st->status_label, "status-text");
    gtk_box_append(GTK_BOX(statusbar), st->status_dot);
    gtk_box_append(GTK_BOX(statusbar), st->status_label);
    gtk_widget_add_css_class(st->subtitle_label, "status-text");
    gtk_widget_set_hexpand(st->subtitle_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(st->subtitle_label), 1.0);
    gtk_box_append(GTK_BOX(statusbar), st->subtitle_label);
    gtk_box_append(GTK_BOX(root), statusbar);

    /* ---- Shortcuts ---- */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(window_key_cb), st);
    gtk_widget_add_controller(st->window, key_ctrl);

    GtkEventController *cmd_key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(cmd_key_ctrl, "key-pressed", G_CALLBACK(cmd_entry_key_cb), st);
    gtk_widget_add_controller(st->cmd_entry, cmd_key_ctrl);

    /* ---- Remembered settings (before the signals, so they don't fire) ---- */
    GKeyFile *kf = g_key_file_new();
    char *spath = settings_path();
    gboolean have_settings = g_key_file_load_from_file(kf, spath, G_KEY_FILE_NONE, NULL);
    g_free(spath);
    Mode start_mode = MODE_NQC;
    if (have_settings) {
        int m = kf_int(kf, "ide", "mode", 0);
        start_mode = (m == 1) ? MODE_NXC : MODE_NQC;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(st->theme_dropdown),
                                    kf_int(kf, "ide", "theme", 0) == 1 ? 1 : 0);
        int t = kf_int(kf, "nqc", "target2", -1);
        if (t < 0) {                /* old settings: CyberMaster was removed */
            t = kf_int(kf, "nqc", "target", 0);
            if (t == 2) t = 0; else if (t > 2) t--;
        }
        if (t >= 0 && (guint)t < G_N_ELEMENTS(NQC_TARGETS))
            gtk_drop_down_set_selected(GTK_DROP_DOWN(st->target_dropdown), t);
        int slot = kf_int(kf, "nqc", "slot", 0);
        if (slot >= 0 && slot < 5)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(st->slot_dropdown), slot);
        char *s;
        s = kf_str(kf, "nqc", "tower_port");
        gtk_editable_set_text(GTK_EDITABLE(st->tower_entry), s);
        g_free(s);
        s = kf_str(kf, "nqc", "firmware");
        gtk_editable_set_text(GTK_EDITABLE(st->fw_entry), s);
        g_free(s);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(st->port_dropdown),
                                    kf_int(kf, "nxc", "port", 0) == 1 ? 1 : 0);
        s = kf_str(kf, "nxc", "bt_device");
        gtk_editable_set_text(GTK_EDITABLE(st->bt_entry), s);
        g_free(s);
    }
    g_key_file_free(kf);
    if (!have_settings && !st->theme_colors)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(st->theme_dropdown), 1);   /* Light */
    gtk_label_set_text(GTK_LABEL(st->target_note),
        NQC_TARGETS[gtk_drop_down_get_selected(GTK_DROP_DOWN(st->target_dropdown))].desc);
    gtk_revealer_set_reveal_child(GTK_REVEALER(st->bt_revealer),
        gtk_drop_down_get_selected(GTK_DROP_DOWN(st->port_dropdown)) == 1);

    /* ---- Signals ---- */
    g_signal_connect(st->buffer, "changed", G_CALLBACK(on_buffer_changed), st);
    g_signal_connect(btn_clear, "clicked", G_CALLBACK(clear_console_cb), st);
    g_signal_connect(st->cancel_btn, "clicked", G_CALLBACK(cancel_cb), st);
    g_signal_connect(st->btn_run, "clicked", G_CALLBACK(compile_run_cb), st);
    g_signal_connect(st->btn_upload, "clicked", G_CALLBACK(compile_upload_cb), st);
    g_signal_connect(about_btn, "clicked", G_CALLBACK(about_cb), st);
    g_signal_connect(st->mode_dropdown, "notify::selected", G_CALLBACK(mode_changed_cb), st);
    g_signal_connect(st->lang_dropdown, "notify::selected", G_CALLBACK(lang_changed_cb), st);
    g_signal_connect(st->port_dropdown, "notify::selected", G_CALLBACK(port_changed_cb), st);
    g_signal_connect(st->target_dropdown, "notify::selected", G_CALLBACK(target_changed_cb), st);
    g_signal_connect(st->theme_dropdown, "notify::selected", G_CALLBACK(theme_changed_cb), st);
    g_signal_connect(st->cmd_entry, "activate", G_CALLBACK(cmd_run_cb), st);
    g_signal_connect(btn_cmd_run, "clicked", G_CALLBACK(cmd_run_cb), st);
    g_signal_connect(st->window, "close-request", G_CALLBACK(window_close_cb), st);

    g_timeout_add_seconds(1, watchdog_tick, st);
    g_timeout_add_seconds(2, device_poll, st);
    device_poll(st);

    /* Start in the remembered language with its starter program */
    st->mode = (start_mode == MODE_NQC) ? MODE_NXC : MODE_NQC;   /* force apply */
    gtk_text_buffer_set_text(st->buffer, mode_template(st->mode), -1);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->mode_dropdown), start_mode);
    apply_mode(st, start_mode);
    st->dark_theme = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->theme_dropdown)) == 0;
    if (!st->theme_colors) apply_omarchy_style(st);
    else apply_theme(st, st->dark_theme);

    update_line_numbers(st);
    st->dirty = FALSE;
    update_title(st);
    gtk_window_present(GTK_WINDOW(st->window));
}

int main(int argc, char **argv) {
    /* A GUI started from Finder or a launcher has a minimal PATH: add the
     * places where nqc and nbc usually live. */
    const char *home = g_get_home_dir();
    char *extra = g_strdup_printf("%s/.local/brickcode/bin:%s/.local/bin:"
        "/opt/homebrew/bin:/usr/local/bin:%s", home, home,
        g_getenv("PATH") ? g_getenv("PATH") : "/usr/bin:/bin");
    g_setenv("PATH", extra, TRUE);
    g_free(extra);

    GtkApplication *app = gtk_application_new("org.brickcode.Studio",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
