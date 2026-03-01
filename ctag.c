#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <signal.h>
#include <id3v2lib.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <fcntl.h>

#define MAX_PATH_LEN 4096

/* popen/pclose are POSIX; declare to satisfy strict standards if needed */
extern FILE *popen(const char *command, const char *type);
extern int pclose(FILE *stream);

#define CURSOR_INVIS    0
#define DIRECTORYLINES 1000
#define MAXDIRWIDTH 256

typedef struct windowData {
    WINDOW* window;
    int     boxed;
    char*   titleBar;
    int     state;
    int     id;
    int     sel_id;
    int     dir_size;
    int     x;
    int     y;
    int     width;
    int     height;
} windowData;

int min(int a, int b) {
    return a < b ? a : b;
}
int max(int a, int b) {
    return a > b ? a : b;
}
void createNewWindow(int height, int width, int starty, int startx, int boxed, char* title, int state, windowData *wd);
void render(void);
void drawWindow(windowData *wd);
void initialiseColors(void);
void getDirectoryInfo(int *size);
void drawDirectory(windowData *wd);
void drawEditor(windowData *wd);
int run_fuzzy_modal(void);
int ext_match(const char *name, const char *ext);
int confirm_modal(const char *title, const char *message, int default_yes);
void shell_escape_double_quotes(const char *src, char *dst, size_t dlen);
int isRegularFile(char* filename);
int is_supported_audio_file(const char *filename);
void clear_directory_selection(int count);
int load_id3_into(const char *filename, char *out_title, size_t tlen, char *out_artist, size_t alen, char *out_album, size_t ablen, char *out_track, size_t trlen);
void load_common_fields(char filenames[][MAXDIRWIDTH], int count, char *out_title, char *out_artist, char *out_album, char *out_track);
int compare(const void* pa, const void *pb);
void resizehandler(int sig);
void terminal_stop();
void terminal_start();
void get_window_dimensions();
int get_input_with_cancel(WINDOW *w, int y, int x, wchar_t *out, size_t max, const wchar_t *initial);
/* ID3v1 compatibility helpers */
int has_id3v1(const char *filename);
int convert_id3v1_to_v2(const char *filename);
int convert_directory_id3v1_to_v2(void);
/* fuzzy search helpers */
int build_fuzzy_candidates(char ***out_list, int *out_count);
int run_fuzzy_modal(void);
/* exec helper to run id3v2 without shell */
static int run_id3v2_argv(char *const argv[]);
static int id3v2_set_field(const char *filename, const char *option, const char *value);
/* id3v2_set_tags removed (unused); use id3v2_set_field or run_id3v2_argv */
static void save_pending_tags(void);
/* If ncurses provides wget_wch, declare it so we can use it when available. */
extern int wget_wch(WINDOW *win, wint_t *wch);
/* ensure wcwidth/wcswidth prototypes are available when headers don't expose them */
extern int wcwidth(wchar_t wc);
extern int wcswidth(const wchar_t *pwcs, size_t n);

/* helpers to convert between multibyte (UTF-8) and wide strings */
static void mb_to_wc(const char *in, wchar_t *out, size_t outlen)
{
    if (!in || !out) return;
    size_t r = mbstowcs(out, in, outlen-1);
    if (r == (size_t)-1) out[0] = L'\0';
    else out[r] = L'\0';
}

static void wc_to_mb(const wchar_t *in, char *out, size_t outlen)
{
    if (!in || !out) return;
    size_t r = wcstombs(out, in, outlen-1);
    if (r == (size_t)-1) out[0] = '\0';
    else out[r] = '\0';
}

/* small helpers to reduce repetition */

/* portable strdup replacement to avoid implicit declaration issues */
static char *my_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *p = malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len + 1);
    return p;
}

static void print_wide(WINDOW *w, int y, int x, const wchar_t *ws) {
    char mb[1024] = "";
    if (ws) wc_to_mb(ws, mb, sizeof(mb));
    mvwprintw(w, y, x, "%s", mb[0] ? mb : "");
}


/* key functions */
void kbf_quit(void);
void kbf_enter(void);
void kbf_tab(void);

void move_sel_id(int amount);

void kbf_up(void);
void kbf_down(void);
void kbf_pgup(void);
void kbf_pgdown(void);
void kbf_end(void);
void kbf_home(void);
void kbf_resize(void);

char dirlines[DIRECTORYLINES][MAXDIRWIDTH]; /* Consider a malloc approach, so it extends to any directory with size>1000 */
/* taglines unused; removed to reduce unused data */
char selected_flags[DIRECTORYLINES] = {0};
int track_order[DIRECTORYLINES];


char * toptext = "Made by RisingThumb          https://risingthumb.xyz ";
#define BOTTOM_HINTS_TEXT "  SP select  A set-artist  L set-album  T write-tracks  G select-all  M select-music"
char * bottomtext = "TAB switch menu    Q to quit    E edit    S save" BOTTOM_HINTS_TEXT;
#define BOTTOM_HINTS " SP:select A:set-artist L:set-album"

struct keyData {
    int     key;
    void    (*kfunc)(void);
};

struct windows{
    windowData* directory;
    windowData* editor;
    windowData* toppanel;
    windowData* bottompanel;
    int         state;
} windows;

/* based on ASCII table. We used Hex or Ncurses-provided values */
enum keys {
    kb_quit = 0x71,
    kb_tab = 0x09,
    kb_enter = 0x0a,
    kb_down = KEY_DOWN,
    kb_up = KEY_UP,
    kb_end = KEY_END,
    kb_home = KEY_HOME,
    kb_pgup = KEY_NPAGE,
    kb_pgdown = KEY_PPAGE,
};

const struct keyData keyTable[] = {
    /* key int,     func pointer... array elements match with states(as they start at 1). More pointers? */
    {kb_quit,       NULL}, /* For loop assumes it is first element. Func pointer unused */
    {kb_enter,      kbf_enter},
    {kb_tab,        kbf_tab},
    {kb_up,         kbf_up},
    {kb_down,       kbf_down},
    {kb_end,        kbf_end},
    {kb_home,       kbf_home},
    {kb_pgup,       kbf_pgup},
    {kb_pgdown,     kbf_pgdown},
    {KEY_RESIZE,    kbf_resize}, /* Need to check if this is defined. KEY_RESIZE doesn't always exist... */
};

/* editing state grouped into AppState to reduce globals */
int edit_mode = 0; /* 0 = view, 1 = editing field */
int selected_count = 0;
int next_track_index = 1;
int suppress_enter_in_edit = 0;

typedef struct AppState {
    char *filenameEditing; /* pointer into dirlines[] */
    int fileSelected;
    int fileDirty;
    int updateEditor;
    char title_buf[512];
    char artist_buf[512];
    char album_buf[512];
    char track_buf[64];
    wchar_t title_w[512];
    wchar_t artist_w[512];
    wchar_t album_w[512];
    wchar_t track_w[64];
    int editor_field; /* 0=title,1=artist,2=album */
} AppState;

static AppState app = {0};


/* space-hold repeat tracking */
long last_space_time = 0;  /* timestamp of last space press in milliseconds */
int space_hold_repeat = 0; /* flag to indicate space is being held for repeat */

/* directory navigation tracking */
char return_to_dir[MAXDIRWIDTH] = {0}; /* directory to highlight when navigating back */

/* cached last-loaded filenames to avoid stale/incorrect metadata */
char last_common_loaded[MAX_PATH_LEN] = ""; /* first file used for common-fields cache */
char last_preview_file[MAX_PATH_LEN] = "";  /* last hovered file used for preview cache */
/* supported audio file extensions for ID3 tagging */
static const char *supported_audio_exts[] = {
    ".mp3", ".flac", ".ogg", ".m4a", ".wav", ".aac", ".opus", ".wma", NULL
};

enum states {
    never = 0,
    dir,
    edit
};

enum colorpairs {
    normal = 1,
    highlight,
    panel
};

/* Render a labeled editor field with wide text and optional highlight. */
static void render_field(WINDOW *w, int y, const char *label, const wchar_t *wbuf, int index) {
    if (windows.state == edit && app.editor_field == index) wattron(w, A_REVERSE);
    mvwprintw(w, y, 1, "%s", label);
    print_wide(w, y, 9, wbuf);
    if (windows.state == edit && app.editor_field == index) wattroff(w, A_REVERSE);
}

/* Handle editing a field: present initial value, read wide input and store back.
   Returns 1 if accepted, 0 if cancelled. */
static int edit_field_at(WINDOW *w, int y, const char *label, wchar_t *widebuf, size_t wide_len, char *mbbuf, size_t mblen) {
    mvwprintw(w, y, 1, "%s", label);
    wclrtoeol(w);
    wrefresh(w);
    wchar_t tempw[512];
    mb_to_wc(mbbuf, tempw, sizeof(tempw)/sizeof(wchar_t));
    int accepted = get_input_with_cancel(w, y, 9, tempw, sizeof(tempw)/sizeof(wchar_t), widebuf);
    if (accepted) {
        wc_to_mb(tempw, mbbuf, mblen);
        wcsncpy(widebuf, tempw, wide_len - 1);
        widebuf[wide_len - 1] = L'\0';
        app.fileDirty = 1;
    }
    return accepted;
}

/* Initialize application state and UI */
static int app_init(int argc, char *argv[])
{
    windowData dirwin, editwin, panwin, panwinbottom;

    (void)argc; (void)argv;

        windows.directory = &dirwin;
        windows.editor = &editwin;
        windows.toppanel = &panwin;
        windows.bottompanel = &panwinbottom;
    windows.state = dir;

    terminal_start();
    createNewWindow(LINES-2, COLS/2, 1, 0, 1, "-Directory-", dir, &dirwin);
    createNewWindow(LINES-2, COLS/2, 1, COLS/2, 1, "-Edit tags-", edit, &editwin);
    createNewWindow(1, COLS, 0, 0, 0, toptext, never, &panwin);
    createNewWindow(1, COLS, LINES-1, 0, 0, bottomtext, never, &panwinbottom);
    kbf_resize();

    getDirectoryInfo(&windows.directory->dir_size);
    return 0;
}

/* Main event loop (extracted from original main) */
static void app_run(void)
{
    int ch;
    int i;

    while (1) {
        render();
        {
            int raw = getch();
            if (raw >= 0 && raw <= 255) ch = tolower(raw);
            else ch = raw;
        }
        /* handle quit with confirmation */
        if (ch == keyTable[0].key) {
            int ok = confirm_modal("Quit", "Are you sure you want to quit?", 0);
            if (ok) break;
            else continue;
        }
        for (i = 1; i < (int)(sizeof(keyTable) / sizeof(struct keyData)); i++) {
            if (ch == keyTable[i].key) {
                if (keyTable[i].kfunc) keyTable[i].kfunc();
            }
        }

        /* preserve original behavior: selection, editing and actions */
        /* clear space-hold repeat flag if any other key is pressed */
        if (ch != ' ' && space_hold_repeat) {
            space_hold_repeat = 0;
        }

        /* directory multi-select toggle (space) preserving selection order */
        if (windows.state == dir && ch == ' ') {
            int sid = windows.directory->sel_id;
            if (sid >= 0 && sid < windows.directory->dir_size) {
                if (selected_flags[sid]) {
                    int removed = track_order[sid];
                    selected_flags[sid] = 0;
                    track_order[sid] = -1;
                    if (selected_count > 0) selected_count--;
                    if (removed > 0) {
                        int k;
                        for (k = 0; k < windows.directory->dir_size; k++) {
                            if (track_order[k] > removed)
                                track_order[k]--;
                        }
                        if (next_track_index > 1) next_track_index--;
                    }
                } else {
                    selected_flags[sid] = 1;
                    selected_count++;
                    track_order[sid] = next_track_index++;
                }
            }
            if (windows.directory->sel_id < windows.directory->dir_size - 1) {
                windows.directory->sel_id += 1;
                if (isRegularFile(dirlines[windows.directory->sel_id])) {
                    app.filenameEditing = dirlines[windows.directory->sel_id];
                    app.fileSelected = 1;
                }
            }
            struct timeval tv;
            gettimeofday(&tv, NULL);
            last_space_time = tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
            space_hold_repeat = 1;
            app.updateEditor = 1;
        }
        else if (windows.state == dir && space_hold_repeat) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            long now = tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
            if (now - last_space_time >= 50) {
                int sid = windows.directory->sel_id;
                if (sid >= 0 && sid < windows.directory->dir_size && !selected_flags[sid]) {
                    selected_flags[sid] = 1;
                    selected_count++;
                    track_order[sid] = next_track_index++;
                        if (sid < windows.directory->dir_size - 1) {
                        windows.directory->sel_id += 1;
                        if (isRegularFile(dirlines[windows.directory->sel_id])) {
                            app.filenameEditing = dirlines[windows.directory->sel_id];
                            app.fileSelected = 1;
                        }
                    }
                    last_space_time = now;
                    app.updateEditor = 1;
                } else {
                    space_hold_repeat = 0;
                }
            }
            }

        if (windows.state == dir && (ch == 'g' || ch == 'm')) {
            int i;
            int would_select_count = 0;
            for (i = 1; i < windows.directory->dir_size && i < DIRECTORYLINES; i++) {
                if (ch == 'g') {
                    would_select_count++;
                    } else {
                        if (isRegularFile(dirlines[i]) && is_supported_audio_file(dirlines[i])) {
                            would_select_count++;
                        }
                    }
            }
            if (would_select_count > 0 && selected_count == would_select_count) {
                clear_directory_selection(windows.directory->dir_size);
            } else {
                for (i = 0; i < windows.directory->dir_size && i < DIRECTORYLINES; i++) {
                    selected_flags[i] = 0;
                    track_order[i] = -1;
                }
                selected_count = 0;
                next_track_index = 1;
                for (i = 1; i < windows.directory->dir_size && i < DIRECTORYLINES; i++) {
                    int pick = 0;
                    if (ch == 'g') pick = 1;
                    else if (isRegularFile(dirlines[i]) && is_supported_audio_file(dirlines[i])) pick = 1;
                    if (pick) {
                        selected_flags[i] = 1;
                        track_order[i] = next_track_index++;
                        selected_count++;
                    }
                }
            }
            app.updateEditor = 1;
        }

        if (windows.state == dir && (ch == 'a' || ch == 'l')) {
            int targets[DIRECTORYLINES];
            int tcnt = 0;
            int j;
            for (j = 0; j < windows.directory->dir_size; j++) {
                if (selected_flags[j]) {
                    if (!isRegularFile(dirlines[j])) targets[tcnt++] = j;
                }
            }
            if (tcnt == 0) {
                int sid = windows.directory->sel_id;
                if (sid >= 0 && sid < windows.directory->dir_size && !isRegularFile(dirlines[sid])) targets[tcnt++] = sid;
            }
                if (tcnt == 0) { app.updateEditor = 1; }
            else {
                char prompt[1024];
                if (tcnt == 1) snprintf(prompt, sizeof(prompt), "%s recursively under '%s'", ch == 'a' ? "Set ARTIST" : "Set ALBUM", dirlines[targets[0]]);
                else snprintf(prompt, sizeof(prompt), "%s recursively on %d selected folders", ch == 'a' ? "Set ARTIST" : "Set ALBUM", tcnt);

                int h = 7;
                int w = COLS > 80 ? 80 : COLS - 4;
                int starty = (LINES - h) / 2;
                int startx = (COLS - w) / 2;
                WINDOW *mw = newwin(h, w, starty, startx);
                box(mw, 0, 0);
                mvwprintw(mw, 0, 2, "%s", ch == 'a' ? "Set ARTIST" : "Set ALBUM");
                mvwprintw(mw, 2, 2, "%s", prompt);
                mvwprintw(mw, 4, 2, "Value: ");
                wrefresh(mw);
                keypad(mw, TRUE);
                curs_set(1);

                char inputbuf[1024] = "";
                wchar_t inputw[1024];
                inputw[0] = L'\0';
                if (tcnt == 1) {
                    strncpy(inputbuf, dirlines[targets[0]], sizeof(inputbuf)-1);
                    inputbuf[sizeof(inputbuf)-1] = '\0';
                    mb_to_wc(inputbuf, inputw, sizeof(inputw)/sizeof(wchar_t));
                }

                int accepted = get_input_with_cancel(mw, 4, 9, inputw, sizeof(inputw)/sizeof(wchar_t), inputw);
                delwin(mw);
                curs_set(CURSOR_INVIS);
                if (!accepted) { app.updateEditor = 1; continue; }

                /* convert wide input back to multibyte for shell use */
                wc_to_mb(inputw, inputbuf, sizeof(inputbuf));

                for (j = 0; j < tcnt; j++) {
                    const char *tname = dirlines[targets[j]];
                    /* use find -print0 and call id3v2 per-file to avoid shell quoting/encoding issues */
                    char findcmd[2048];
                    snprintf(findcmd, sizeof(findcmd), "find \"%s\" -type f -iname \"*.mp3\" -print0", tname);
                    FILE *fp = popen(findcmd, "r");
                    if (!fp) continue;
                    char fname[MAX_PATH_LEN];
                    int ci = 0;
                    int cc;
                    while ((cc = fgetc(fp)) != EOF) {
                        if (cc == 0) {
                            if (ci > 0) {
                                fname[ci] = '\0';
                                if (ch == 'a') id3v2_set_field(fname, "--artist", inputbuf);
                                else id3v2_set_field(fname, "--album", inputbuf);
                            }
                            ci = 0;
                        } else {
                            if (ci < (int)sizeof(fname) - 1) fname[ci++] = (char)cc;
                        }
                    }
                    pclose(fp);
                }
                app.updateEditor = 1;
            }
        }

        if (windows.state == dir && ch == 't') {
            if (selected_count <= 0) {
                int ok = confirm_modal("No selection", "No files selected — apply to hovered file?", 0);
                if (!ok) { app.updateEditor = 1; continue; }
                int sid = windows.directory->sel_id;
                if (sid >= 0 && sid < windows.directory->dir_size && isRegularFile(dirlines[sid])) {
                    int tr = track_order[sid] > 0 ? track_order[sid] : 1;
                    char track_arg[32];
                    snprintf(track_arg, sizeof(track_arg), "%d", tr);
                    id3v2_set_field(dirlines[sid], "--track", track_arg);
                }
                app.updateEditor = 1;
            } else {
                char prompt[256];
                snprintf(prompt, sizeof(prompt), "Write track numbers to %d selected files?", selected_count);
                int ok = confirm_modal("Confirm write tracks", prompt, 1);
                if (!ok) { app.updateEditor = 1; continue; }
                int k;
                for (k = 1; k < next_track_index; k++) {
                    int j;
                    for (j = 0; j < windows.directory->dir_size; j++) {
                        if (track_order[j] == k && isRegularFile(dirlines[j])) {
                            char track_arg[32];
                            snprintf(track_arg, sizeof(track_arg), "%d", k);
                            id3v2_set_field(dirlines[j], "--track", track_arg);
                            break;
                        }
                    }
                }
                app.updateEditor = 1;
            }
        }

        /* Convert ID3v1 to ID3v2 for all files in current directory */
        if (windows.state == dir && ch == 'c') {
            int ok = confirm_modal("Convert ID3v1->v2", "Convert id3v1 tags to id3v2 for all files in this directory?", 0);
            if (ok) {
                /* iterate files and convert */
                int i;
                int count = 0;
                for (i = 1; i < windows.directory->dir_size && i < DIRECTORYLINES; i++) {
                    if (!isRegularFile(dirlines[i])) continue;
                    /* limit to mp3 files (id3v1 is usually present on mp3) */
                    if (!ext_match(dirlines[i], ".mp3")) continue;
                    if (convert_id3v1_to_v2(dirlines[i]) == 1) count++;
                }
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Converted %d files", count);
                    confirm_modal("Conversion complete", msg, 1);
                }
            }
            app.updateEditor = 1;
        }

        /* Fuzzy file search modal (press '/') */
        if (windows.state == dir && ch == '/') {
            run_fuzzy_modal();
            app.updateEditor = 1;
        }

        if (windows.state == edit) {
            if (ch == 'e' || ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && !suppress_enter_in_edit)) {
                edit_mode = 1;
                curs_set(1);
                WINDOW *w = windows.editor->window;

                if (app.editor_field == 0) {
                    edit_field_at(w, 2, "Title : ", app.title_w, sizeof(app.title_w)/sizeof(wchar_t), app.title_buf, sizeof(app.title_buf));
                } else if (app.editor_field == 1) {
                    edit_field_at(w, 3, "Artists: ", app.artist_w, sizeof(app.artist_w)/sizeof(wchar_t), app.artist_buf, sizeof(app.artist_buf));
                } else if (app.editor_field == 2) {
                    edit_field_at(w, 4, "Album : ", app.album_w, sizeof(app.album_w)/sizeof(wchar_t), app.album_buf, sizeof(app.album_buf));
                } else if (app.editor_field == 3) {
                    edit_field_at(w, 5, "Track : ", app.track_w, sizeof(app.track_w)/sizeof(wchar_t), app.track_buf, sizeof(app.track_buf));
                }

                app.updateEditor = 1;
                noecho();
                curs_set(CURSOR_INVIS);
                edit_mode = 0;
            }
                else if (ch == 's') {
                save_pending_tags();
            }
            if (suppress_enter_in_edit && ch != '\n' && ch != '\r' && ch != KEY_ENTER)
                suppress_enter_in_edit = 0;
        }
    }
}

/* Clean up application */
static void app_shutdown(void)
{
    terminal_stop();
}

int main(int argc, char *argv[])
{
    if (app_init(argc, argv) != 0) return 1;
    app_run();
    app_shutdown();
    return 0;
}



void terminal_stop() {
    endwin();
}

void terminal_start() {
    setlocale(LC_ALL, "");
    initscr();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(CURSOR_INVIS);
    raw();
    start_color();
    initialiseColors();
    refresh();
}

void kbf_enter() {
    if (windows.state == dir) {
        int* sel_id = &windows.directory->sel_id;
        int* size = &windows.directory->dir_size;
        wclear(windows.directory->window);
        wclear(windows.editor->window);
        if ( isRegularFile(dirlines[*sel_id]) ) {
            windows.state = edit;
            /* entering editor: suppress any lingering Enter so it isn't
               immediately reinterpreted as "edit field". Also clear
               the input queue to drop the Enter that triggered this. */
            suppress_enter_in_edit = 1;
            flushinp();
            app.filenameEditing = dirlines[*sel_id];
            app.fileSelected = 1;
            app.updateEditor = 1;
        }
        else {
            /* Get the basename of the current working directory before navigating away */
            char cwd[MAX_PATH_LEN];
            return_to_dir[0] = '\0';
            
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                char *last_slash = strrchr(cwd, '/');
                if (last_slash != NULL && *(last_slash + 1) != '\0') {
                    strncpy(return_to_dir, last_slash + 1, MAXDIRWIDTH - 1);
                    return_to_dir[MAXDIRWIDTH - 1] = '\0';
                }
            }
            
            if (chdir(dirlines[*sel_id]) == 0) {
                *sel_id = 0;
                getDirectoryInfo(size);
                
                /* Find and highlight the directory we came from */
                if (return_to_dir[0] != '\0') {
                    int i;
                    for (i = 1; i < *size; i++) {
                        if (strcmp(dirlines[i], return_to_dir) == 0) {
                            *sel_id = i;
                            break;
                        }
                    }
                    return_to_dir[0] = '\0'; /* clear for next use */
                }
            }
        }
    }
    return;
}

void kbf_resize() {
    terminal_stop();
    terminal_start();
    wclear(windows.directory->window);
    wclear(windows.editor->window);
    wclear(windows.toppanel->window);
    wclear(windows.bottompanel->window);

    wresize(windows.directory->window, windows.directory->height=LINES/2, windows.directory->width=COLS);
    mvwin(windows.directory->window, windows.directory->y=1, windows.directory->x=0);

    wresize(windows.editor->window, windows.editor->height=LINES/2 - 1, windows.editor->width=COLS);
    mvwin(windows.editor->window, windows.editor->y=LINES/2 + 1, windows.editor->x=0);

    wresize(windows.toppanel->window, windows.toppanel->height=1, windows.toppanel->width=COLS);
    mvwin(windows.toppanel->window, windows.toppanel->y=0, windows.toppanel->x=0);

    wresize(windows.bottompanel->window, windows.bottompanel->height=1, windows.bottompanel->width=COLS);
    mvwin(windows.bottompanel->window, windows.bottompanel->y=LINES-1, windows.bottompanel->x=0);
}

void kbf_tab() {
    if (windows.state == dir) {
        windows.state = edit;
    } else if (windows.state == edit) {
        /* leaving edit mode: if changes pending, save them automatically */
        if (app.fileDirty && app.fileSelected) {
            save_pending_tags();
            app.updateEditor = 1;
        }
        windows.state = dir;
    }
    return;
}

void move_sel_id(int amount) {
    if (windows.state == dir) {
        int* sel_id = &windows.directory->sel_id;
        int* size = &windows.directory->dir_size;
        (*sel_id) = max(min((*sel_id)+amount, (*size) - 1), 0);
        /* always refresh editor preview when cursor moves */
        if (isRegularFile(dirlines[*sel_id])) {
            app.filenameEditing = dirlines[*sel_id];
            app.fileSelected = 1;
        }
        app.updateEditor = 1;
    } else if (windows.state == edit) {
        /* move hovered metadata field up */
        app.editor_field = max(min(app.editor_field + amount, 3), 0);
        app.updateEditor = 1;
    }
}

void kbf_up() {
    move_sel_id(-1);
}

void kbf_home() {
    move_sel_id(-100000);
}

void kbf_end() {
    move_sel_id(100000);
}
void kbf_pgup() {
    move_sel_id((LINES-4)/2);
}
void kbf_pgdown() {
    move_sel_id(-((LINES-4)/2));
}

void kbf_down() {
    move_sel_id(1);
}

int isRegularFile(char* filename) {
    struct stat file_stat;
    stat(filename, &file_stat);
    return S_ISREG(file_stat.st_mode);
}

int is_supported_audio_file(const char *filename) {
    const char **p = supported_audio_exts;
    while (*p) {
        if (ext_match(filename, *p)) {
            return 1;
        }
        p++;
    }
    return 0;
}

void getDirectoryInfo(int *size) {
    int i = 0;
    DIR *dir;
    struct dirent *dp;

    dir = opendir(".");
    strcpy(dirlines[i++], "..");
    while((dp = readdir(dir)) != NULL)
        if (dp->d_name[0] != '.')
            strcpy(dirlines[i++], dp->d_name);
    /* sort array of fixed-width strings */
    qsort(dirlines, i, MAXDIRWIDTH, compare);
    *size = i;
    /* clear any previous selection state */
    clear_directory_selection(i);
}

int compare(const void* pa, const void *pb) {
    const char *a = (const char*)pa;
    const char *b = (const char*)pb;
    return strcmp(a, b);
}

void clear_directory_selection(int count) {
    int i;
    for (i = 0; i < count && i < DIRECTORYLINES; i++) {
        selected_flags[i] = 0;
        track_order[i] = -1;
    }
    selected_count = 0;
    next_track_index = 1;
}

/* Load ID3 into provided buffers; returns 0 on success */
int load_id3_into(const char *filename, char *out_title, size_t tlen, char *out_artist, size_t alen, char *out_album, size_t ablen, char *out_track, size_t trlen)
{
    if (!filename) return -1;
    out_title[0] = '\0'; out_artist[0] = '\0'; out_album[0] = '\0'; out_track[0] = '\0';
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "id3v2 -l \"%s\" 2>/dev/null", filename);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[1024];
    while(fgets(line, sizeof(line), fp)) {
        if (strstr(line, "TIT2")) {
            char *col = strchr(line, ':');
            if (col) { while (*(++col) == ' '); strncpy(out_title, col, tlen-1); out_title[strcspn(out_title, "\r\n")] = '\0'; }
        } else if (strstr(line, "TPE1")) {
            char *col = strchr(line, ':');
            if (col) { while (*(++col) == ' '); strncpy(out_artist, col, alen-1); out_artist[strcspn(out_artist, "\r\n")] = '\0'; }
        } else if (strstr(line, "TALB")) {
            char *col = strchr(line, ':');
            if (col) { while (*(++col) == ' '); strncpy(out_album, col, ablen-1); out_album[strcspn(out_album, "\r\n")] = '\0'; }
        } else if (strstr(line, "TRCK")) {
            char *col = strchr(line, ':');
            if (col) { while (*(++col) == ' '); strncpy(out_track, col, trlen-1); out_track[strcspn(out_track, "\r\n")] = '\0'; }
        }
    }
    pclose(fp);
    return 0;
}

/* Given an array of filenames, set out_* to the common value across all files,
   or empty string if they differ. */
void load_common_fields(char filenames[][MAXDIRWIDTH], int count, char *out_title, char *out_artist, char *out_album, char *out_track)
{
    char tbuf[512], abuf[512], albuf[512], trbuf[64];
    int i;
    out_title[0] = '\0'; out_artist[0] = '\0'; out_album[0] = '\0'; out_track[0] = '\0';
    if (count <= 0) return;
    /* load first file into out_* */
    if (load_id3_into(filenames[0], out_title, 512, out_artist, 512, out_album, 512, out_track, 64) != 0) return;
    for (i = 1; i < count; i++) {
        tbuf[0]=abuf[0]=albuf[0]=trbuf[0]='\0';
        load_id3_into(filenames[i], tbuf, sizeof(tbuf), abuf, sizeof(abuf), albuf, sizeof(albuf), trbuf, sizeof(trbuf));
        if (out_title[0] && tbuf[0] && strcmp(out_title, tbuf) != 0) out_title[0] = '\0';
        else if (!tbuf[0]) out_title[0] = '\0';
        if (out_artist[0] && abuf[0] && strcmp(out_artist, abuf) != 0) out_artist[0] = '\0';
        else if (!abuf[0]) out_artist[0] = '\0';
        if (out_album[0] && albuf[0] && strcmp(out_album, albuf) != 0) out_album[0] = '\0';
        else if (!albuf[0]) out_album[0] = '\0';
        if (out_track[0] && trbuf[0] && strcmp(out_track, trbuf) != 0) out_track[0] = '\0';
        else if (!trbuf[0]) out_track[0] = '\0';
    }
}

/* ID3v1 detection and conversion implementation */
int has_id3v1(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;
    if (fseek(f, -128, SEEK_END) != 0) { fclose(f); return 0; }
    unsigned char tag[128];
    if (fread(tag, 1, 128, f) != 128) { fclose(f); return 0; }
    fclose(f);
    return (tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G');
}

/* Convert an individual file from ID3v1 to ID3v2. Returns 1 if conversion performed. */
int convert_id3v1_to_v2(const char *filename)
{
    if (!has_id3v1(filename)) return 0;
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;
    if (fseek(f, -128, SEEK_END) != 0) { fclose(f); return 0; }
    unsigned char tag[128];
    if (fread(tag, 1, 128, f) != 128) { fclose(f); return 0; }
    fclose(f);

    char title[31] = {0}, artist[31] = {0}, album[31] = {0}, year[5] = {0}, comment[31] = {0};
    unsigned char track = 0;
    memcpy(title, &tag[3], 30); title[30] = '\0';
    memcpy(artist, &tag[33], 30); artist[30] = '\0';
    memcpy(album, &tag[63], 30); album[30] = '\0';
    memcpy(year, &tag[93], 4); year[4] = '\0';
    memcpy(comment, &tag[97], 30); comment[30] = '\0';
    /* ID3v1.1: if comment[28] == 0 then comment[29] is track number */
    if (tag[125] == 0 && tag[126] != 0) track = tag[126];

    /* trim trailing spaces */
    char *p;
    for (p = title + strlen(title) - 1; p >= title && (*p == '\0' || *p == ' '); p--) *p = '\0';
    for (p = artist + strlen(artist) - 1; p >= artist && (*p == '\0' || *p == ' '); p--) *p = '\0';
    for (p = album + strlen(album) - 1; p >= album && (*p == '\0' || *p == ' '); p--) *p = '\0';
    for (p = comment + strlen(comment) - 1; p >= comment && (*p == '\0' || *p == ' '); p--) *p = '\0';

    char esc_title[512] = {0}, esc_artist[512] = {0}, esc_album[512] = {0}, esc_year[64] = {0}, esc_comment[512] = {0}, esc_fn[1024] = {0};
    shell_escape_double_quotes(title, esc_title, sizeof(esc_title));
    shell_escape_double_quotes(artist, esc_artist, sizeof(esc_artist));
    shell_escape_double_quotes(album, esc_album, sizeof(esc_album));
    shell_escape_double_quotes(year, esc_year, sizeof(esc_year));
    shell_escape_double_quotes(comment, esc_comment, sizeof(esc_comment));
    shell_escape_double_quotes(filename, esc_fn, sizeof(esc_fn));

    /* call id3v2 directly via argv to avoid shell quoting/encoding problems */
    char *args[16];
    int ai = 0;
    args[ai++] = "id3v2";
    if (title[0]) { args[ai++] = "--song"; args[ai++] = title; }
    if (artist[0]) { args[ai++] = "--artist"; args[ai++] = artist; }
    if (album[0]) { args[ai++] = "--album"; args[ai++] = album; }
    if (year[0]) { args[ai++] = "--year"; args[ai++] = year; }
    if (comment[0]) { args[ai++] = "--comment"; args[ai++] = comment; }
    if (track) { char trackbuf[16]; snprintf(trackbuf, sizeof(trackbuf), "%u", (unsigned)track); args[ai++] = "--track"; args[ai++] = trackbuf; }
    args[ai++] = (char*)filename;
    args[ai] = NULL;
    if (run_id3v2_argv(args)) return 1;
    return 0;
}

/* Optional directory-level helper (not used directly but kept for completeness) */
int convert_directory_id3v1_to_v2(void)
{
    int converted = 0;
    int i;
    for (i = 1; i < windows.directory->dir_size && i < DIRECTORYLINES; i++) {
        if (!isRegularFile(dirlines[i])) continue;
        if (!ext_match(dirlines[i], ".mp3")) continue;
        if (convert_id3v1_to_v2(dirlines[i])) converted++;
    }
    return converted;
}

/* Load ID3 fields via id3v2 CLI into app buffers. */
void load_id3_fields(const char *filename) {
    /* clear buffers */
    app.title_buf[0] = '\0';
    app.artist_buf[0] = '\0';
    app.album_buf[0] = '\0';
    app.track_buf[0] = '\0';
    if (!filename) return;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "id3v2 -l \"%s\" 2>/dev/null", filename);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char line[1024];
    while(fgets(line, sizeof(line), fp)) {
        char *p = NULL;
        if ((p = strstr(line, "TIT2")) != NULL) {
            char *col = strchr(line, ':');
            if (col) {
                while (*(++col) == ' ');
                strncpy(app.title_buf, col, sizeof(app.title_buf)-1);
                /* trim newline */
                app.title_buf[strcspn(app.title_buf, "\r\n")] = '\0';
            }
        }
        else if ((p = strstr(line, "TPE1")) != NULL) {
            char *col = strchr(line, ':');
            if (col) {
                while (*(++col) == ' ');
                strncpy(app.artist_buf, col, sizeof(app.artist_buf)-1);
                app.artist_buf[strcspn(app.artist_buf, "\r\n")] = '\0';
            }
        }
        else if ((p = strstr(line, "TALB")) != NULL) {
            char *col = strchr(line, ':');
            if (col) {
                while (*(++col) == ' ');
                strncpy(app.album_buf, col, sizeof(app.album_buf)-1);
                app.album_buf[strcspn(app.album_buf, "\r\n")] = '\0';
            }
        }
        else if ((p = strstr(line, "TRCK")) != NULL) {
            char *col = strchr(line, ':');
            if (col) {
                while (*(++col) == ' ');
                strncpy(app.track_buf, col, sizeof(app.track_buf)-1);
                app.track_buf[strcspn(app.track_buf, "\r\n")] = '\0';
            }
        }
    }
    pclose(fp);
    /* convert multibyte to wide for display/editing */
    mb_to_wc(app.title_buf, app.title_w, sizeof(app.title_w)/sizeof(wchar_t));
    mb_to_wc(app.artist_buf, app.artist_w, sizeof(app.artist_w)/sizeof(wchar_t));
    mb_to_wc(app.album_buf, app.album_w, sizeof(app.album_w)/sizeof(wchar_t));
    mb_to_wc(app.track_buf, app.track_w, sizeof(app.track_w)/sizeof(wchar_t));
}

void initialiseColors() {
    init_pair(normal, COLOR_WHITE, COLOR_BLACK);
    init_pair(highlight, COLOR_YELLOW, COLOR_BLACK);
    init_pair(panel, COLOR_BLUE, COLOR_BLACK);
}

void createNewWindow(int height, int width, int starty, int startx, int boxed, char* title, int state, windowData *wd) {
    WINDOW *local_win;
    local_win = newwin(height, width, starty, startx);
    wd->window = local_win;
    wd->boxed = boxed;
    wd->titleBar = title;
    wd->state = state;
    wd->id = 0;
    wd->sel_id = 0;
    wd->dir_size = 0;
}

void render() {
    drawDirectory(windows.directory);
    drawWindow(windows.directory);
    if (app.updateEditor) {
        drawEditor(windows.editor);
        app.updateEditor = 0;
    }
    drawWindow(windows.editor);
    drawWindow(windows.toppanel);
    /* bottom panel is context-aware */
    void drawBottomPanel(void);
    drawBottomPanel();
    refresh();
    wrefresh(windows.toppanel->window);
    wrefresh(windows.bottompanel->window);
    wrefresh(windows.directory->window);
    wrefresh(windows.editor->window);
}

void drawBottomPanel(void)
{
    WINDOW *w = windows.bottompanel->window;
    wclear(w);
    if (edit_mode) {
        mvwprintw(w, 0, 1, "Editing: Enter accept  Esc/Tab cancel");
    } else if (windows.state == dir) {
        mvwprintw(w, 0, 1, "TAB switch menu  Q quit  SP select  A set-artist  L set-album  T write-tracks  G select-all  M select-music  C convert  E edit");
    } else if (windows.state == edit) {
        mvwprintw(w, 0, 1, "TAB switch menu  Q quit  E edit  S save  Up/Down move  Enter edit field");
    } else {
        mvwprintw(w, 0, 1, "%s", bottomtext);
    }
    wrefresh(w);
}

void drawDirectory(windowData *wd) {
    WINDOW* w = wd->window;
    /* ensure directory window has focus when drawing */
int ext_match(const char *name, const char *ext);
    int max_height = wd->height;
    int i = wd->sel_id;
    int extra_item = wd->dir_size - max_height;
    int start_i = 0;
    int end_i = max_height;

    if (extra_item < max_height)
        extra_item = max_height;
    if (extra_item > 0) {
        if (wd->sel_id > 1)
            start_i = wd->sel_id - 2;
        else
            start_i = 0;
        end_i = start_i + max_height;
    }
    if (end_i-1 > wd->dir_size) {
        start_i += -end_i + wd->dir_size + 2;
        end_i += -end_i + wd->dir_size;
        if (start_i < 0)
            start_i = 0;
    }
    wclear(w);
    for (i = 0; i < end_i-start_i; i++) {
        if (wd->sel_id == i+start_i)
            wattron(w, A_REVERSE);
        if (isRegularFile(dirlines[i+start_i]) )
            wattron(w, COLOR_PAIR(normal));
        else
            wattron(w, COLOR_PAIR(panel));
        /* display track order prefix for selected files: up to 3 digits + '*' */
        int ord = track_order[i+start_i];
        char prefix[8] = "    ";
        if (ord > 0 && ord <= 999) {
            snprintf(prefix, sizeof(prefix), "%3d*", ord);
        } else if (ord > 999) {
            /* exceed display width: just show asterisk */
            snprintf(prefix, sizeof(prefix), "   *");
        } else {
            snprintf(prefix, sizeof(prefix), "    ");
        }
        mvwprintw(w, i+1, 1, "%s %s", prefix, dirlines[i+start_i]);
        wattroff(w, A_REVERSE | COLOR_PAIR(panel) | COLOR_PAIR(normal));
    }
}

int ext_match(const char *name, const char *ext)
{
	size_t nl = strlen(name), el = strlen(ext);
	return nl >= el && !strcmp(name + nl - el, ext);
}

/* Portable wide-character reader: assemble multibyte UTF-8 sequences from wgetch().
   Returns 1 on success and stores the wide char into outch; 0 on error. */
static int wget_wide_impl(WINDOW *ww, wint_t *outch)
{
    char buf[MB_CUR_MAX];
    size_t blen = 0;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    while (1) {
#if defined(NCURSES_VERSION_MAJOR)
        /* Prefer ncurses' wide-character input if available. This handles
           composed characters and input methods better than manual byte
           assembly. */
        wint_t wwch;
        int wret = wget_wch(ww, &wwch);
        if (wret == ERR) continue;
        /* wget_wch returns OK and places a wide char in wwch; special keys
           may be returned as KEY_ values which are also handled by our
           callers. */
        *outch = wwch;
        return 1;
#else
        int ch = wgetch(ww);
        if (ch == ERR) continue;
        /* If ESC is received, it may be an Alt-prefixed key (ESC + key).
           Wait briefly for a following byte; if none arrives, treat as ESC. */
        if (ch == 27) {
            wtimeout(ww, 50);
            int ch2 = wgetch(ww);
            wtimeout(ww, -1);
            if (ch2 == ERR) {
                *outch = 27;
                return 1;
            }
            ch = ch2;
        }
        /* capture special keys directly */
        if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == KEY_BACKSPACE || ch == KEY_HOME || ch == KEY_END || ch == KEY_NPAGE || ch == KEY_PPAGE || ch == KEY_RESIZE) {
            *outch = ch;
            return 1;
        }
        unsigned char byte = (unsigned char)ch;
        if (blen < sizeof(buf)) buf[blen++] = (char)byte;
#endif
        wchar_t wc;
        size_t r = mbrtowc(&wc, buf, blen, &st);
        if (r == (size_t)-2) {
            /* incomplete sequence; read more bytes */
            if (blen >= MB_CUR_MAX) {
                /* fallback: decode first byte as single char */
                mbstate_t st2; memset(&st2,0,sizeof(st2));
                mbrtowc(&wc, (const char*)&buf[0], 1, &st2);
                *outch = wc;
                if (blen > 1) memmove(buf, buf+1, blen-1);
                return 1;
            }
            continue;
        } else if (r == (size_t)-1) {
            /* invalid sequence: consume first byte */
            mbstate_t st2; memset(&st2,0,sizeof(st2));
            wchar_t wc2;
            mbrtowc(&wc2, (const char*)&buf[0], 1, &st2);
            *outch = wc2;
            if (blen > 1) memmove(buf, buf+1, blen-1);
            return 1;
        } else {
            /* success: r bytes consumed, produce wc */
            *outch = wc;
            return 1;
        }
    }
    return 0;
}

/* Run id3v2 with argv (argv[0] should be "id3v2"). Returns 1 on success. */
static int run_id3v2_argv(char *const argv[])
{
    if (!argv || !argv[0]) return 0;
    /* parse argv-style options and apply them via id3v2lib */
    const char *title = NULL, *artist = NULL, *album = NULL, *track = NULL, *year = NULL, *comment = NULL;
    int i = 1;
    while (argv[i]) {
        const char *opt = argv[i];
        if (opt[0] == '-') {
            /* option expects a value */
            if (!argv[i+1]) return 0;
            if (strcmp(opt, "--song") == 0 || strcmp(opt, "--title") == 0) { title = argv[++i]; }
            else if (strcmp(opt, "--artist") == 0) { artist = argv[++i]; }
            else if (strcmp(opt, "--album") == 0) { album = argv[++i]; }
            else if (strcmp(opt, "--track") == 0) { track = argv[++i]; }
            else if (strcmp(opt, "--year") == 0) { year = argv[++i]; }
            else if (strcmp(opt, "--comment") == 0) { comment = argv[++i]; }
            else {
                /* unknown option: cannot handle here */
                return 0;
            }
            i++;
            continue;
        }
        break;
    }
    if (!argv[i]) return 0; /* no filename provided */
    int any = 0;
    for (; argv[i]; i++) {
        const char *fn = argv[i];
        if (!fn) continue;
        ID3v2_tag *tag = load_tag(fn);
        if (!tag) tag = new_tag();
        char enc = ISO_ENCODING;
        if (title && title[0]) tag_set_title((char*)title, enc, tag);
        if (artist && artist[0]) tag_set_artist((char*)artist, enc, tag);
        if (album && album[0]) tag_set_album((char*)album, enc, tag);
        if (track && track[0]) tag_set_track((char*)track, enc, tag);
        if (year && year[0]) tag_set_year((char*)year, enc, tag);
        if (comment && comment[0]) tag_set_comment((char*)comment, enc, tag);
        set_tag(fn, tag);
        free_tag(tag);
        any = 1;
    }
    return any ? 1 : 0;
}

/* Convenience wrappers for id3v2 actions. */
static int id3v2_set_field(const char *filename, const char *option, const char *value)
{
    if (!filename || !option) return 0;
    ID3v2_tag *tag = load_tag(filename);
    if (!tag) tag = new_tag();
    /* choose ISO encoding by default for compatibility */
    char enc = ISO_ENCODING;
    if (strcmp(option, "--song") == 0 || strcmp(option, "--title") == 0) {
        tag_set_title((char*)value, enc, tag);
    } else if (strcmp(option, "--artist") == 0) {
        tag_set_artist((char*)value, enc, tag);
    } else if (strcmp(option, "--album") == 0) {
        tag_set_album((char*)value, enc, tag);
    } else if (strcmp(option, "--track") == 0) {
        tag_set_track((char*)value, enc, tag);
    } else if (strcmp(option, "--year") == 0) {
        tag_set_year((char*)value, enc, tag);
    } else if (strcmp(option, "--comment") == 0) {
        tag_set_comment((char*)value, enc, tag);
    } else {
        /* unknown option: fallback to CLI */
        char *args[6]; int ai = 0;
        args[ai++] = "id3v2";
        args[ai++] = (char*)option;
        args[ai++] = (char*)value;
        args[ai++] = (char*)filename;
        args[ai] = NULL;
        if (tag) free_tag(tag);
        return run_id3v2_argv(args);
    }
    set_tag(filename, tag);
    free_tag(tag);
    return 1;
}

/* id3v2_set_tags removed (unused) */

/* Save current pending tags for either selected files or the hovered file. */
static void save_pending_tags(void) {
    if (!app.fileSelected || !app.fileDirty) return;
    int j;
    if (selected_count > 0) {
        for (j = 0; j < windows.directory->dir_size; j++) {
            if (!selected_flags[j]) continue;
            char *fn = dirlines[j];
            char track_arg[32];
            if (track_order[j] > 0) {
                snprintf(track_arg, sizeof(track_arg), "%d", track_order[j]);
            } else {
                strncpy(track_arg, app.track_buf, sizeof(track_arg)-1);
                track_arg[sizeof(track_arg)-1] = '\0';
            }
            char *args[12];
            int ai = 0;
            args[ai++] = "id3v2";
            if (app.title_buf[0]) { args[ai++] = "--song"; args[ai++] = app.title_buf; }
            if (app.artist_buf[0]) { args[ai++] = "--artist"; args[ai++] = app.artist_buf; }
            if (app.album_buf[0]) { args[ai++] = "--album"; args[ai++] = app.album_buf; }
            if (track_arg[0]) { args[ai++] = "--track"; args[ai++] = track_arg; }
            args[ai++] = fn;
            args[ai] = NULL;
            run_id3v2_argv(args);
        }
    } else if (app.filenameEditing) {
        char track_arg[32];
        if (windows.directory && windows.directory->sel_id >= 0 && windows.directory->sel_id < DIRECTORYLINES && track_order[windows.directory->sel_id] > 0) {
            snprintf(track_arg, sizeof(track_arg), "%d", track_order[windows.directory->sel_id]);
        } else {
            strncpy(track_arg, app.track_buf, sizeof(track_arg)-1);
            track_arg[sizeof(track_arg)-1] = '\0';
        }
        {
            char *args[12];
            int ai = 0;
            args[ai++] = "id3v2";
            if (app.title_buf[0]) { args[ai++] = "--song"; args[ai++] = app.title_buf; }
            if (app.artist_buf[0]) { args[ai++] = "--artist"; args[ai++] = app.artist_buf; }
            if (app.album_buf[0]) { args[ai++] = "--album"; args[ai++] = app.album_buf; }
            if (track_arg[0]) { args[ai++] = "--track"; args[ai++] = track_arg; }
            args[ai++] = app.filenameEditing;
            args[ai] = NULL;
            run_id3v2_argv(args);
        }
    }
    app.fileDirty = 0;
}

/* new helper to read a line in a window; returns 1 if user accepted (Enter),
   returns 0 if user cancelled via ESC (27) or Tab ('\t') */
int get_input_with_cancel(WINDOW *w, int y, int x, wchar_t *out, size_t max, const wchar_t *initial)
{
    wint_t wch;
    size_t pos = 0;
    static wchar_t clipboard[512] = {0};
    if (initial) {
        size_t initlen = wcslen(initial);
        if (initlen > max - 1) initlen = max - 1;
        wcsncpy(out, initial, initlen);
        out[initlen] = L'\0';
        pos = initlen;
    } else {
        out[0] = L'\0';
        pos = 0;
    }

    /* display initial and enable keypad for arrow keys */
    char mbout[2048];
    wc_to_mb(out, mbout, sizeof(mbout));
    mvwprintw(w, y, x, "%s", mbout);
    /* compute displayed column width (not byte length) for proper cursor placement */
    {
        int col = 0;
        size_t olen = wcslen(out);
        size_t ii;
        for (ii = 0; ii < olen; ii++) {
            int cw = wcwidth(out[ii]);
            if (cw < 0) cw = 1;
            col += cw;
        }
        wmove(w, y, x + col);
    }
    wrefresh(w);
    keypad(w, TRUE);

    /* manual input loop using wide-character input */
    while (1) {
        int res = wget_wide_impl(w, &wch);
        if (!res) continue;
        if (wch == L'\n' || wch == L'\r') break;
        if (wch == 27 || wch == L'\t') { /* ESC or Tab => cancel */
            return 0;
        }
        if (wch == KEY_LEFT) {
            if (pos > 0) pos--;
        } else if (wch == KEY_RIGHT) {
            size_t curlen = wcslen(out);
            if (pos < curlen) pos++;
        } else if (wch == 3) { /* Ctrl-C => copy field to internal clipboard */
            wcsncpy(clipboard, out, sizeof(clipboard)/sizeof(wchar_t)-1);
            clipboard[sizeof(clipboard)/sizeof(wchar_t)-1] = L'\0';
        } else if (wch == 22) { /* Ctrl-V => paste internal clipboard at cursor */
            size_t clen = wcslen(clipboard);
            size_t curlen = wcslen(out);
            if (clen > 0 && curlen < max - 1) {
                size_t can_insert = (max - 1) - curlen;
                size_t ins = clen > can_insert ? can_insert : clen;
                wchar_t tmp[max];
                size_t i;
                size_t prefix = pos;
                /* copy prefix */
                for (i = 0; i < prefix; i++) tmp[i] = out[i];
                /* copy clipboard */
                for (i = 0; i < ins; i++) tmp[prefix + i] = clipboard[i];
                /* copy suffix including null terminator */
                for (i = prefix; i <= curlen; i++) tmp[prefix + ins + i - prefix] = out[i];
                /* copy back */
                for (i = 0; i < prefix + ins + (curlen - prefix + 1); i++) out[i] = tmp[i];
                pos += ins;
            }
        } else if (wch == KEY_BACKSPACE || wch == 127 || wch == '\b') {
            size_t curlen = wcslen(out);
            if (pos > 0) {
                wchar_t tmp[max];
                size_t i;
                size_t prefix = pos - 1;
                size_t suffix_len = curlen - pos + 1; /* including null */
                /* copy prefix */
                for (i = 0; i < prefix; i++) tmp[i] = out[i];
                /* copy suffix (from pos to end) */
                for (i = 0; i < suffix_len; i++) tmp[prefix + i] = out[pos + i];
                /* copy back */
                for (i = 0; i < prefix + suffix_len; i++) out[i] = tmp[i];
                pos--;
            }
        } else if (iswprint((wint_t)wch)) {
            size_t curlen = wcslen(out);
            if (curlen < max - 1) {
                wchar_t tmp[max];
                size_t i;
                size_t prefix = pos;
                /* copy prefix */
                for (i = 0; i < prefix; i++) tmp[i] = out[i];
                /* insert new char */
                tmp[prefix] = (wchar_t)wch;
                /* copy suffix including null */
                for (i = prefix; i <= curlen; i++) tmp[prefix + 1 + i - prefix] = out[i];
                /* copy back */
                for (i = 0; i < prefix + 1 + (curlen - prefix + 1); i++) out[i] = tmp[i];
                pos++;
            }
        }
        /* redraw and position cursor using column widths (wcwidth) */
        wc_to_mb(out, mbout, sizeof(mbout));
        mvwprintw(w, y, x, "%s", mbout);
        wclrtoeol(w);
        /* compute display column of prefix */
        {
            wchar_t prefix[1024];
            if (pos >= (int)(sizeof(prefix)/sizeof(wchar_t))) prefix[0] = L'\0';
            else {
                wcsncpy(prefix, out, pos);
                prefix[pos] = L'\0';
            }
            int col = 0;
            size_t ii;
            size_t plen = wcslen(prefix);
            for (ii = 0; ii < plen; ii++) {
                int cw = wcwidth(prefix[ii]);
                if (cw < 0) cw = 1;
                col += cw;
            }
            wmove(w, y, x + col);
            wrefresh(w);
        }
    }
    /* Enter pressed => accept */
    return 1;
}

void drawEditor(windowData *wd) {
    WINDOW* w = wd->window;
    wclear(w);

    if (!app.fileSelected) {
        mvwprintw(w, 1, 1, "No file selected. Press Enter on a file to edit.");
        return;
    }

    int supported = 0;
    if (app.filenameEditing)
        supported = is_supported_audio_file(app.filenameEditing);

    if (!supported) {
        mvwprintw(w, 1, 1, "Selected: %s", app.filenameEditing ? app.filenameEditing : "");
        mvwprintw(w, 2, 1, "Supported formats: .mp3 .flac .ogg .m4a .wav .aac .opus");
        return;
    }

    /* For multiple selected files, compute common fields (left column);
       also show a preview (right column) for the currently hovered file. */
    {
        if (selected_count > 0) {
            char filenames[DIRECTORYLINES][MAXDIRWIDTH];
            int cnt = 0;
            int j;
            for (j = 0; j < windows.directory->dir_size && cnt < DIRECTORYLINES; j++) {
                if (selected_flags[j]) {
                    strncpy(filenames[cnt], dirlines[j], MAXDIRWIDTH-1);
                    filenames[cnt][MAXDIRWIDTH-1] = '\0';
                    cnt++;
                }
            }
            if (cnt > 0 && !app.fileDirty) {
                /* only reload common fields when the primary selected file changes */
                if (last_common_loaded[0] == '\0' || strcmp(last_common_loaded, filenames[0]) != 0) {
                    load_common_fields(filenames, cnt, app.title_buf, app.artist_buf, app.album_buf, app.track_buf);
                        /* convert common multibyte results into wide for display/edit */
                        mb_to_wc(app.title_buf, app.title_w, sizeof(app.title_w)/sizeof(wchar_t));
                        mb_to_wc(app.artist_buf, app.artist_w, sizeof(app.artist_w)/sizeof(wchar_t));
                        mb_to_wc(app.album_buf, app.album_w, sizeof(app.album_w)/sizeof(wchar_t));
                        mb_to_wc(app.track_buf, app.track_w, sizeof(app.track_w)/sizeof(wchar_t));
                    strncpy(last_common_loaded, filenames[0], MAX_PATH_LEN-1);
                    last_common_loaded[MAX_PATH_LEN-1] = '\0';
                    /* clear preview cache when common selection changes */
                    last_preview_file[0] = '\0';
                }
                /* keep filenameEditing as the first selected for reference */
                app.filenameEditing = filenames[0];
            } else {
                /* no selected files or dirty editing - clear common cache so it reloads later */
                if (cnt == 0) last_common_loaded[0] = '\0';
            }
            mvwprintw(w, 1, 1, "Files : %d selected", cnt);
        } else {
            if (app.filenameEditing) {
                if (!app.fileDirty && (last_preview_file[0] == '\0' || strcmp(last_preview_file, app.filenameEditing) != 0)) {
                    load_id3_fields(app.filenameEditing);
                    strncpy(last_preview_file, app.filenameEditing, MAX_PATH_LEN-1);
                    last_preview_file[MAX_PATH_LEN-1] = '\0';
                    /* clear common cache when viewing single file */
                    last_common_loaded[0] = '\0';
                }
            } else {
                last_preview_file[0] = '\0';
                last_common_loaded[0] = '\0';
            }
            mvwprintw(w, 1, 1, "File  : %s", app.filenameEditing);
        }
    }

    /* Highlight hovered metadata field */
    if (windows.state == edit && app.editor_field == 0) wattron(w, A_REVERSE);
    /* If multiple selected, draw a two-column split: left = common/current,
       right = preview of hovered file */
    int mid = wd->width / 2;
        if (selected_count > 0) {
        /* left column (common/current) */
        render_field(w, 2, "Title : ", app.title_w, 0);
        render_field(w, 3, "Artists: ", app.artist_w, 1);
        render_field(w, 4, "Album : ", app.album_w, 2);
        render_field(w, 5, "Track : ", app.track_w, 3);

        /* right column: preview hovered file */
        int sid = windows.directory->sel_id;
        char ptitle[512] = ""; char partist[512] = ""; char palbum[512] = ""; char ptrack[64] = "";
        const char *preview_name = NULL;
        if (sid >= 0 && sid < windows.directory->dir_size && isRegularFile(dirlines[sid])) {
            preview_name = dirlines[sid];
            load_id3_into(preview_name, ptitle, sizeof(ptitle), partist, sizeof(partist), palbum, sizeof(palbum), ptrack, sizeof(ptrack));
        }
        /* draw vertical separator */
        int r;
        for (r = 1; r < wd->height; r++)
            mvwaddch(w, r, mid, ACS_VLINE);

        mvwprintw(w, 2, mid+2, "Preview: %s", preview_name ? preview_name : "");
        mvwprintw(w, 3, mid+2, "Title : %s", ptitle[0] ? ptitle : "");
        mvwprintw(w, 4, mid+2, "Artist: %s", partist[0] ? partist : "");
        mvwprintw(w, 5, mid+2, "Album : %s", palbum[0] ? palbum : "");
        mvwprintw(w, 6, mid+2, "Track : %s", ptrack[0] ? ptrack : "");
    } else {
        render_field(w, 2, "Title : ", app.title_w, 0);
        render_field(w, 3, "Artists: ", app.artist_w, 1);
        render_field(w, 4, "Album : ", app.album_w, 2);
        render_field(w, 5, "Track : ", app.track_w, 3);

        mvwprintw(w, 7, 1, "Use Up/Down to move, 'e' to edit field (Esc/Tab to cancel), 's' to save changes.");
    }
}

void drawWindow(windowData *wd) {
    WINDOW* w = wd->window;
    if (windows.state && windows.state == wd->state)
        wattron(w, COLOR_PAIR(highlight));
    if (wd->boxed)
        box(w, 0, 0);
    wattroff(w, COLOR_PAIR(highlight));
    wattron(w, A_BOLD | A_REVERSE | COLOR_PAIR(panel));
    mvwprintw(w, 0, 1, wd->titleBar);
    wattroff(w, A_BOLD | A_REVERSE | COLOR_PAIR(panel));

    wrefresh(w);
}

/* modal confirm dialog; returns 1=yes, 0=no. default_yes when Enter pressed */
int confirm_modal(const char *title, const char *message, int default_yes)
{
    int h = 7, w = COLS > 60 ? 60 : COLS - 4;
    int starty = (LINES - h) / 2;
    int startx = (COLS - w) / 2;
    WINDOW *mw = newwin(h, w, starty, startx);
    box(mw, 0, 0);
    mvwprintw(mw, 0, 2, "%s", title);
    mvwprintw(mw, 2, 2, "%s", message);
    mvwprintw(mw, 4, 2, "Proceed? [Y/n] (Enter = %s)", default_yes ? "Y" : "n");
    wrefresh(mw);
    keypad(mw, TRUE);
    int ch = wgetch(mw);
    delwin(mw);
    if (ch == '\n' || ch == '\r') return default_yes ? 1 : 0;
    ch = tolower(ch);
    if (ch == 'y') return 1;
    if (ch == 'n') return 0;
    return default_yes ? 1 : 0;
}

/* Simple double-quote escaper for shell use. Escapes backslashes and double quotes. */
void shell_escape_double_quotes(const char *src, char *dst, size_t dlen)
{
    size_t i = 0;
    const char *s = src;
    while (*s && i + 1 < dlen) {
        if (*s == '\\' || *s == '"') {
            if (i + 2 >= dlen) break;
            dst[i++] = '\\';
            dst[i++] = *s++;
        } else {
            dst[i++] = *s++;
        }
    }
    dst[i] = '\0';
}

/* Build a list of candidate files under the current directory using find.
   out_list will be allocated and must be freed by caller (free each string then free(list)).
   Returns 1 on success, 0 on failure. */
int build_fuzzy_candidates(char ***out_list, int *out_count)
{
    if (!out_list || !out_count) return 0;
    *out_list = NULL; *out_count = 0;
    FILE *fp = popen("find . -type f -print0", "r");
    if (!fp) return 0;
    int cap = 128;
    char **list = malloc(sizeof(char*) * cap);
    if (!list) { pclose(fp); return 0; }
    int count = 0;
    char buf[MAX_PATH_LEN];
    int bi = 0;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == 0) {
            buf[bi] = '\0';
            /* strip leading ./ */
            char *p = buf;
            if (p[0] == '.' && p[1] == '/') p += 2;
            if (count >= cap) {
                cap *= 2;
                char **tmp = realloc(list, sizeof(char*) * cap);
                if (!tmp) break;
                list = tmp;
            }
            list[count++] = my_strdup(p);
            bi = 0;
        } else {
            if (bi < (int)sizeof(buf)-1) buf[bi++] = (char)c;
        }
    }
    pclose(fp);
    *out_list = list;
    *out_count = count;
    return 1;
}

/* Very small subsequence fuzzy test: returns 1 if pattern is subsequence of s (case-insensitive). */
/* Case-insensitive substring check using simple loop (portable). */
static const char *ci_strstr(const char *hay, const char *needle)
{
    if (!*needle) return hay;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    size_t i;
    for (i = 0; i + nlen <= hlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            if (tolower((unsigned char)hay[i+j]) != tolower((unsigned char)needle[j])) break;
        }
        if (j == nlen) return hay + i;
    }
    return NULL;
}

/* (previous simple fuzzy matcher removed; compute_fuzzy_score is used instead) */

/* Compute an integer relevance score for a candidate `s` given `pattern`.
   Higher is better. Returns 0 for no match. */
static int compute_fuzzy_score(const char *pattern, const char *s)
{
    if (!pattern || !*pattern) return 100000;
    if (!s) return 0;
    int plen = strlen(pattern);
    int slen = strlen(s);
    /* substring match gets top preference; earlier is better */
    const char *sub = ci_strstr(s, pattern);
    if (sub) {
        int pos = (int)(sub - s);
        int score = 100000 - pos * 10 + plen * 50 - slen;
        return score > 0 ? score : 1;
    }
    /* subsequence match: measure span and start position */
    int p = 0;
    int start = -1, end = -1;
    for (int i = 0; s[i]; i++) {
        if (tolower((unsigned char)s[i]) == tolower((unsigned char)pattern[p])) {
            if (start < 0) start = i;
            end = i;
            p++;
            if (p == plen) break;
        }
    }
    if (p != plen) return 0;
    int span = end - start + 1;
    int score = 50000 - span * 5 - start * 5 + plen * 30 - slen;
    return score > 0 ? score : 1;
}

/* Run a simple modal that allows fuzzy searching files. On Enter, change into
   the selected file's directory and set selection to that file. */
int run_fuzzy_modal(void)
{
    char **cands = NULL;
    int cnt = 0;
    if (!build_fuzzy_candidates(&cands, &cnt) || cnt <= 0) {
        if (cands) free(cands);
        return 0;
    }

    int max_h = LINES - 4;
    if (max_h < 8) max_h = LINES - 2;
    int h = max_h > 20 ? 20 : max_h;
    int w = COLS - 6;
    if (w < 40) w = COLS - 4;
    if (w > 120) w = 120;
    int starty = (LINES - h) / 2;
    int startx = (COLS - w) / 2;
    WINDOW *mw = newwin(h, w, starty, startx);
    keypad(mw, TRUE);
    box(mw, 0, 0);
    mvwprintw(mw, 0, 2, "Fuzzy Search");
    char query[256] = "";
    int sel = 0; /* index into matches */
    int offset = 0; /* scroll offset into matches */
    int ch;
    /* matches array will hold indices into cands[] (assigned after scoring) */
    int *matches = NULL;

    while (1) {
            /* build scored matches list */
            typedef struct { int idx; int score; } pair_t;
            pair_t *pairs = malloc(sizeof(pair_t) * cnt);
            if (!pairs) break;
            int mcnt = 0;
            for (int i = 0; i < cnt; i++) {
                int sc = compute_fuzzy_score(query, cands[i]);
                if (sc > 0) { pairs[mcnt].idx = i; pairs[mcnt].score = sc; mcnt++; }
            }
            /* sort pairs by score desc, tiebreak by lexicographic path (simple O(n^2) sort) */
            for (int a = 0; a < mcnt - 1; a++) {
                for (int b = a + 1; b < mcnt; b++) {
                    int swap = 0;
                    if (pairs[b].score > pairs[a].score) swap = 1;
                    else if (pairs[b].score == pairs[a].score) {
                        if (strcmp(cands[pairs[b].idx], cands[pairs[a].idx]) < 0) swap = 1;
                    }
                    if (swap) {
                        pair_t t = pairs[a]; pairs[a] = pairs[b]; pairs[b] = t;
                    }
                }
            }
            /* populate matches[] with sorted indices */
            int *sorted_matches = malloc(sizeof(int) * mcnt);
            if (!sorted_matches) { free(pairs); break; }
            for (int i = 0; i < mcnt; i++) sorted_matches[i] = pairs[i].idx;
            free(pairs);
            /* use sorted_matches as our matches array */
            matches = sorted_matches;
        int visible = h - 5; if (visible < 3) visible = 3;
        if (sel < 0) sel = 0;
        if (sel >= mcnt) sel = mcnt > 0 ? mcnt - 1 : 0;
        if (sel < offset) offset = sel;
        if (sel >= offset + visible) offset = sel - visible + 1;

        /* draw header and query */
        mvwprintw(mw, 1, 2, "Search: %-.200s", query);
        wclrtoeol(mw);
        /* list visible matches */
        for (int line = 0; line < visible; line++) {
            int mi = offset + line;
            int y = 3 + line;
            char disp[512] = "";
            if (mi < mcnt) {
                const char *full = cands[matches[mi]];
                size_t fl = strlen(full);
                int avail = w - 4;
                if ((int)fl <= avail) {
                    strncpy(disp, full, sizeof(disp)-1);
                    disp[sizeof(disp)-1] = '\0';
                } else {
                    /* show tail of path with ellipsis */
                    if (avail > 4) {
                        int tail = avail - 3;
                        snprintf(disp, sizeof(disp), "...%s", full + fl - tail);
                    } else {
                        strncpy(disp, full + fl - avail, sizeof(disp)-1);
                        disp[sizeof(disp)-1] = '\0';
                    }
                }
                if (mi == sel) wattron(mw, A_REVERSE);
                mvwprintw(mw, y, 2, "%-*s", w-4, disp);
                if (mi == sel) wattroff(mw, A_REVERSE);
            } else {
                mvwprintw(mw, y, 2, "%-*s", w-4, "");
            }
        }
        mvwprintw(mw, h-2, 2, "Enter=choose  Esc=cancel  Backspace=edit  Up/Down=move  %d results", mcnt);
        wclrtoeol(mw);
        wrefresh(mw);

        ch = wgetch(mw);
        if (ch == 27) { /* ESC */
            break;
        } else if (ch == '\n' || ch == '\r') {
            if (mcnt <= 0) break;
            int idx = matches[sel];
            const char *path = cands[idx];
            char dirbuf[MAX_PATH_LEN];
            strncpy(dirbuf, path, sizeof(dirbuf)-1); dirbuf[sizeof(dirbuf)-1] = '\0';
            char *slash = strrchr(dirbuf, '/');
            char fname[MAX_PATH_LEN];
            if (slash) {
                strncpy(fname, slash+1, sizeof(fname)-1); fname[sizeof(fname)-1] = '\0';
                *slash = '\0';
                if (chdir(dirbuf) == 0) {
                    getDirectoryInfo(&windows.directory->dir_size);
                    for (int j = 0; j < windows.directory->dir_size; j++) {
                        if (strcmp(dirlines[j], fname) == 0) { windows.directory->sel_id = j; break; }
                    }
                    app.updateEditor = 1;
                }
            } else {
                /* file in current dir */
                strncpy(fname, path, sizeof(fname)-1); fname[sizeof(fname)-1] = '\0';
                if (chdir(".") == 0) {
                    getDirectoryInfo(&windows.directory->dir_size);
                    for (int j = 0; j < windows.directory->dir_size; j++) {
                        if (strcmp(dirlines[j], fname) == 0) { windows.directory->sel_id = j; break; }
                    }
                    app.updateEditor = 1;
                }
            }
            delwin(mw);
            if (matches) free(matches);
            for (int k=0;k<cnt;k++) free(cands[k]);
            free(cands);
            return 1;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            size_t ql = strlen(query);
            if (ql) query[ql-1] = '\0';
            sel = 0;
            offset = 0;
        } else if (ch == KEY_UP) {
            if (sel > 0) sel--;
        } else if (ch == KEY_DOWN) {
            if (sel < mcnt - 1) sel++;
        } else if (isprint(ch) && (int)strlen(query) < (int)sizeof(query)-1) {
            size_t ql = strlen(query);
            query[ql] = (char)ch; query[ql+1] = '\0';
            sel = 0; offset = 0;
        }
    }

    delwin(mw);
    if (matches) free(matches);
    for (int k=0;k<cnt;k++) free(cands[k]);
    free(cands);
    return 0;
}
