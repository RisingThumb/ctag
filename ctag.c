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

void createNewWindow(int height, int width, int starty, int startx, int boxed, char* title, int state, windowData *wd);
void render(void);
void drawWindow(windowData *wd);
void initialiseColors(void);
void getDirectoryInfo(int *size);
void drawDirectory(windowData *wd);
void drawEditor(windowData *wd);
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
int get_input_with_cancel(WINDOW *w, int y, int x, char *out, size_t max, const char *initial);

/* key functions */
void kbf_quit(void);
void kbf_enter(void);
void kbf_tab(void);
void kbf_up(void);
void kbf_down(void);
void kbf_resize(void);

char dirlines[DIRECTORYLINES][MAXDIRWIDTH]; /* Consider a malloc approach, so it extends to any directory with size>1000 */
char taglines[DIRECTORYLINES][MAXDIRWIDTH];
char selected_flags[DIRECTORYLINES] = {0};
int track_order[DIRECTORYLINES];


char *filenameEditing;
int fileSelected = 0;
int fileDirty = 0;
int updateEditor = 0;

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
    kb_up = KEY_UP
};

const struct keyData keyTable[] = {
    /* key int,     func pointer... array elements match with states(as they start at 1). More pointers? */
    {kb_quit,       NULL}, /* For loop assumes it is first element. Func pointer unused */
    {kb_enter,      kbf_enter},
    {kb_tab,        kbf_tab},
    {kb_up,         kbf_up},
    {kb_down,       kbf_down},
    {KEY_RESIZE,    kbf_resize}, /* Need to check if this is defined. KEY_RESIZE doesn't always exist... */
};

/* editing state */
int edit_mode = 0; /* 0 = view, 1 = editing field */
char title_buf[512] = {0};
char artist_buf[512] = {0};
char album_buf[512] = {0};
char track_buf[64] = {0};
int editor_field = 0; /* 0=title,1=artist,2=album */
int selected_count = 0;
int next_track_index = 1;
int suppress_enter_in_edit = 0;

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


int main( int argc, char *argv[]) {
    int ch;
    int i;
    windowData dirwin, editwin, panwin, panwinbottom;

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
    while (1) {
        render();
        ch = tolower(getch());
        /* handle quit with confirmation */
        if (ch == keyTable[0].key) {
            int ok = confirm_modal("Quit", "Are you sure you want to quit?", 0);
            if (ok) break;
            else continue;
        }
        for (i = 1; i < (sizeof(keyTable) / sizeof(struct keyData)); i++) {
            if (ch == keyTable[i].key) {
                (*keyTable[i].kfunc)();
            }
        }
        /* clear space-hold repeat flag if any other key is pressed */
        if (ch != ' ' && space_hold_repeat) {
            space_hold_repeat = 0;
        }
        /* directory multi-select toggle (space) preserving selection order */
        if (windows.state == dir && ch == ' ') {
            int sid = windows.directory->sel_id;
            if (sid >= 0 && sid < windows.directory->dir_size) {
                if (selected_flags[sid]) {
                    /* deselect: remove and renumber higher indices */
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
                    /* select: assign next sequential track index */
                    selected_flags[sid] = 1;
                    selected_count++;
                    track_order[sid] = next_track_index++;
                }
            }
            /* move down and set up repeat: capture time and enable hold detection */
            if (windows.directory->sel_id < windows.directory->dir_size - 1) {
                windows.directory->sel_id += 1;
                if (isRegularFile(dirlines[windows.directory->sel_id])) {
                    filenameEditing = dirlines[windows.directory->sel_id];
                    fileSelected = 1;
                }
            }
            /* get current time for repeat detection (space held down) */
            struct timeval tv;
            gettimeofday(&tv, NULL);
            last_space_time = tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
            space_hold_repeat = 1;
            updateEditor = 1;
        }
        /* repeat selection if space is held (check timer every render cycle) */
        else if (windows.state == dir && space_hold_repeat) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            long now = tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
            /* if 50ms elapsed since last space, attempt to select and move again */
            if (now - last_space_time >= 50) {
                int sid = windows.directory->sel_id;
                if (sid >= 0 && sid < windows.directory->dir_size && !selected_flags[sid]) {
                    selected_flags[sid] = 1;
                    selected_count++;
                    track_order[sid] = next_track_index++;
                    /* move down */
                    if (sid < windows.directory->dir_size - 1) {
                        windows.directory->sel_id += 1;
                        if (isRegularFile(dirlines[windows.directory->sel_id])) {
                            filenameEditing = dirlines[windows.directory->sel_id];
                            fileSelected = 1;
                        }
                    }
                    last_space_time = now;
                    updateEditor = 1;
                } else {
                    space_hold_repeat = 0; /* reached end or already selected */
                }
            }
        }
        /* select-all (g) and select-music (m) with toggle behavior */
        if (windows.state == dir && (ch == 'g' || ch == 'm')) {
            int i;
            /* determine how many items would be selected by this command */
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
            /* if already all would-be-selected items are selected, then deselect them */
            if (would_select_count > 0 && selected_count == would_select_count) {
                clear_directory_selection(windows.directory->dir_size);
            } else {
                /* reset previous selection state */
                for (i = 0; i < windows.directory->dir_size && i < DIRECTORYLINES; i++) {
                    selected_flags[i] = 0;
                    track_order[i] = -1;
                }
                selected_count = 0;
                next_track_index = 1;
                for (i = 1; i < windows.directory->dir_size && i < DIRECTORYLINES; i++) {
                    int pick = 0;
                    if (ch == 'g') {
                        pick = 1; /* select everything (skip .. at index 0) */
                    } else {
                        /* select audio files by checking supported extensions */
                        if (isRegularFile(dirlines[i]) && is_supported_audio_file(dirlines[i])) {
                            pick = 1;
                        }
                    }
                    if (pick) {
                        selected_flags[i] = 1;
                        track_order[i] = next_track_index++;
                        selected_count++;
                    }
                }
            }
            updateEditor = 1;
        }
        /* Directory actions: set artist/album recursively to directory name */
        if (windows.state == dir && (ch == 'a' || ch == 'l')) {
            /* Determine target directories: selected folders, or hovered folder if none selected */
            int targets[DIRECTORYLINES];
            int tcnt = 0;
            int j;
            for (j = 0; j < windows.directory->dir_size; j++) {
                if (selected_flags[j]) {
                    /* only consider entries that are directories */
                    if (!isRegularFile(dirlines[j])) {
                        targets[tcnt++] = j;
                    }
                }
            }
            if (tcnt == 0) {
                int sid = windows.directory->sel_id;
                if (sid >= 0 && sid < windows.directory->dir_size && !isRegularFile(dirlines[sid])) {
                    targets[tcnt++] = sid;
                }
            }
                if (tcnt == 0) {
                    /* nothing to do */
                    updateEditor = 1;
                } else {
                    /* Build an input modal to ask for the artist/album name. Prefill when single target */
                    char prompt[1024];
                    if (tcnt == 1) {
                        snprintf(prompt, sizeof(prompt), "%s recursively under '%s'", ch == 'a' ? "Set ARTIST" : "Set ALBUM", dirlines[targets[0]]);
                    } else {
                        snprintf(prompt, sizeof(prompt), "%s recursively on %d selected folders", ch == 'a' ? "Set ARTIST" : "Set ALBUM", tcnt);
                    }

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

                    /* show cursor while editing the modal input */
                    curs_set(1);

                    char inputbuf[1024] = "";
                    if (tcnt == 1) {
                        /* prefill with directory name */
                        strncpy(inputbuf, dirlines[targets[0]], sizeof(inputbuf)-1);
                        inputbuf[sizeof(inputbuf)-1] = '\0';
                    }

                    int accepted = get_input_with_cancel(mw, 4, 9, inputbuf, sizeof(inputbuf), inputbuf);
                    delwin(mw);
                    /* hide cursor again */
                    curs_set(CURSOR_INVIS);
                    if (!accepted) { updateEditor = 1; continue; }

                    /* For each target dir run find inside that directory */
                    for (j = 0; j < tcnt; j++) {
                        const char *tname = dirlines[targets[j]];
                        char esc_t[1024]; shell_escape_double_quotes(tname, esc_t, sizeof(esc_t));
                        char esc_val[1024]; shell_escape_double_quotes(inputbuf, esc_val, sizeof(esc_val));
                        char cmd[4096];
                        if (ch == 'a')
                            snprintf(cmd, sizeof(cmd), "find \"%s\" -type f -iname \"*.mp3\" -exec id3v2 --artist \"%s\" \"{}\" \\; 2>/dev/null", esc_t, esc_val);
                        else
                            snprintf(cmd, sizeof(cmd), "find \"%s\" -type f -iname \"*.mp3\" -exec id3v2 --album \"%s\" \"{}\" \\; 2>/dev/null", esc_t, esc_val);
                        system(cmd);
                    }
                    updateEditor = 1;
                }
        }
        /* Directory action: write track indices to selected files in selection order */
        if (windows.state == dir && ch == 't') {
            if (selected_count <= 0) {
                int ok = confirm_modal("No selection", "No files selected — apply to hovered file?", 0);
                if (!ok) { updateEditor = 1; continue; }
                /* apply to hovered file only if it's a regular file */
                int sid = windows.directory->sel_id;
                if (sid >= 0 && sid < windows.directory->dir_size && isRegularFile(dirlines[sid])) {
                    char cmd[512];
                    int tr = track_order[sid] > 0 ? track_order[sid] : 1;
                    snprintf(cmd, sizeof(cmd), "id3v2 --track \"%d\" \"%s\" 2>/dev/null", tr, dirlines[sid]);
                    system(cmd);
                }
                updateEditor = 1;
            } else {
                char prompt[256];
                snprintf(prompt, sizeof(prompt), "Write track numbers to %d selected files?", selected_count);
                int ok = confirm_modal("Confirm write tracks", prompt, 1);
                if (!ok) { updateEditor = 1; continue; }
                /* apply in selection order using next_track_index as upper bound */
                int k;
                for (k = 1; k < next_track_index; k++) {
                    int j;
                    for (j = 0; j < windows.directory->dir_size; j++) {
                        if (track_order[j] == k && isRegularFile(dirlines[j])) {
                            char cmd[512];
                            snprintf(cmd, sizeof(cmd), "id3v2 --track \"%d\" \"%s\" 2>/dev/null", k, dirlines[j]);
                            system(cmd);
                            break;
                        }
                    }
                }
                updateEditor = 1;
            }
        }
        /* additional keys for edit mode */
        if (windows.state == edit) {
            if (ch == 'e' || ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && !suppress_enter_in_edit)) {
                /* edit currently hovered metadata field only; allow cancel with ESC or Tab */
                edit_mode = 1;
                curs_set(1);
                WINDOW *w = windows.editor->window;
                char tempbuf[512];
                int accepted = 0;

                if (editor_field == 0) {
                    /* Title: line 2, value starts at col 9 */
                    mvwprintw(w, 2, 1, "Title : ");
                    wclrtoeol(w);
                    wrefresh(w);
                    accepted = get_input_with_cancel(w, 2, 9, tempbuf, sizeof(tempbuf), title_buf);
                    if (accepted) {
                        strncpy(title_buf, tempbuf, sizeof(title_buf)-1);
                        title_buf[sizeof(title_buf)-1] = '\0';
                        fileDirty = 1;
                    }
                } else if (editor_field == 1) {
                    mvwprintw(w, 3, 1, "Artists: ");
                    wclrtoeol(w);
                    wrefresh(w);
                    accepted = get_input_with_cancel(w, 3, 9, tempbuf, sizeof(tempbuf), artist_buf);
                    if (accepted) {
                        strncpy(artist_buf, tempbuf, sizeof(artist_buf)-1);
                        artist_buf[sizeof(artist_buf)-1] = '\0';
                        fileDirty = 1;
                    }
                } else if (editor_field == 2) {
                    mvwprintw(w, 4, 1, "Album : ");
                    wclrtoeol(w);
                    wrefresh(w);
                    accepted = get_input_with_cancel(w, 4, 9, tempbuf, sizeof(tempbuf), album_buf);
                    if (accepted) {
                        strncpy(album_buf, tempbuf, sizeof(album_buf)-1);
                        album_buf[sizeof(album_buf)-1] = '\0';
                        fileDirty = 1;
                    }
                } else if (editor_field == 3) {
                    mvwprintw(w, 5, 1, "Track : ");
                    wclrtoeol(w);
                    wrefresh(w);
                    accepted = get_input_with_cancel(w, 5, 9, tempbuf, sizeof(tempbuf), track_buf);
                    if (accepted) {
                        strncpy(track_buf, tempbuf, sizeof(track_buf)-1);
                        track_buf[sizeof(track_buf)-1] = '\0';
                        fileDirty = 1;
                    }
                }

                updateEditor = 1;
                noecho();
                curs_set(CURSOR_INVIS);
                edit_mode = 0;
            }
            else if (ch == 's') {
                if (fileSelected && fileDirty) {
                    /* save using id3v2 commandline across selected files (or current) */
                    int j;
                    if (selected_count > 0) {
                        for (j = 0; j < windows.directory->dir_size; j++) {
                            if (!selected_flags[j]) continue;
                            char *fn = dirlines[j];
                            char track_arg[32];
                            if (track_order[j] > 0) {
                                snprintf(track_arg, sizeof(track_arg), "%d", track_order[j]);
                            } else {
                                strncpy(track_arg, track_buf, sizeof(track_arg)-1);
                                track_arg[sizeof(track_arg)-1] = '\0';
                            }
                            char cmd[2048];
                            snprintf(cmd, sizeof(cmd), "id3v2 --song \"%s\" --artist \"%s\" --album \"%s\" --track \"%s\" \"%s\" 2>/dev/null", title_buf, artist_buf, album_buf, track_arg, fn);
                            system(cmd);
                        }
                    } else if (filenameEditing) {
                        char cmd[2048];
                        /* when saving single file, use track_buf unless track_order for the hovered file exists */
                        char track_arg[32];
                        if (windows.directory && windows.directory->sel_id >= 0 && windows.directory->sel_id < DIRECTORYLINES && track_order[windows.directory->sel_id] > 0) {
                            snprintf(track_arg, sizeof(track_arg), "%d", track_order[windows.directory->sel_id]);
                        } else {
                            strncpy(track_arg, track_buf, sizeof(track_arg)-1);
                            track_arg[sizeof(track_arg)-1] = '\0';
                        }
                        snprintf(cmd, sizeof(cmd), "id3v2 --song \"%s\" --artist \"%s\" --album \"%s\" --track \"%s\" \"%s\" 2>/dev/null", title_buf, artist_buf, album_buf, track_arg, filenameEditing);
                        system(cmd);
                    }
                    fileDirty = 0;
                }
            }
            /* clear the suppress flag after processing this key iteration.
               keep it set while only Enter sequences have been seen so that
               the Enter that switched to edit doesn't immediately re-trigger
               editing a field. Reset once a non-Enter key is observed. */
            if (suppress_enter_in_edit && ch != '\n' && ch != '\r' && ch != KEY_ENTER)
                suppress_enter_in_edit = 0;
        }
    }
    terminal_stop();
    return 0;
}

void terminal_stop() {
    endwin();
}

void terminal_start() {
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
            filenameEditing = dirlines[*sel_id];
            fileSelected = 1;
            updateEditor = 1;
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
    if (windows.state == dir)
        windows.state = edit;
    else if ((windows.state = edit))
        windows.state = dir;
    return;
}

void kbf_up() {
    if (windows.state == dir) {
        int* sel_id = &windows.directory->sel_id;
        if (*sel_id > 0)
            (*sel_id) -= 1;
        /* always refresh editor preview when cursor moves */
        if (isRegularFile(dirlines[*sel_id])) {
            filenameEditing = dirlines[*sel_id];
            fileSelected = 1;
        }
        updateEditor = 1;
    } else if (windows.state == edit) {
        /* move hovered metadata field up */
        if (editor_field > 0)
            editor_field -= 1;
        updateEditor = 1;
    }
    return;
}

void kbf_down() {
    if (windows.state == dir) {
        int* sel_id = &windows.directory->sel_id;
        int* size = &windows.directory->dir_size;
        if ((*sel_id) < (*size) - 1)
            (*sel_id) += 1;
        /* always refresh editor preview when cursor moves */
        if (isRegularFile(dirlines[*sel_id])) {
            filenameEditing = dirlines[*sel_id];
            fileSelected = 1;
        }
        updateEditor = 1;
    } else if (windows.state == edit) {
        /* move hovered metadata field down (four fields total) */
        if (editor_field < 3)
            editor_field += 1;
        updateEditor = 1;
    }
    return;
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

/* Load ID3 fields via id3v2 CLI into title_buf/artist_buf/album_buf. */
void load_id3_fields(const char *filename) {
    /* clear buffers */
    title_buf[0] = '\0';
    artist_buf[0] = '\0';
    album_buf[0] = '\0';
    track_buf[0] = '\0';
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
                strncpy(title_buf, col, sizeof(title_buf)-1);
                /* trim newline */
                title_buf[strcspn(title_buf, "\r\n")] = '\0';
            }
        }
        else if ((p = strstr(line, "TPE1")) != NULL) {
            char *col = strchr(line, ':');
            if (col) {
                while (*(++col) == ' ');
                strncpy(artist_buf, col, sizeof(artist_buf)-1);
                artist_buf[strcspn(artist_buf, "\r\n")] = '\0';
            }
        }
        else if ((p = strstr(line, "TALB")) != NULL) {
            char *col = strchr(line, ':');
            if (col) {
                while (*(++col) == ' ');
                strncpy(album_buf, col, sizeof(album_buf)-1);
                album_buf[strcspn(album_buf, "\r\n")] = '\0';
            }
        }
        else if ((p = strstr(line, "TRCK")) != NULL) {
            char *col = strchr(line, ':');
            if (col) {
                while (*(++col) == ' ');
                strncpy(track_buf, col, sizeof(track_buf)-1);
                track_buf[strcspn(track_buf, "\r\n")] = '\0';
            }
        }
    }
    pclose(fp);
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
    if (updateEditor) {
        drawEditor(windows.editor);
        updateEditor = 0;
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
        mvwprintw(w, 0, 1, "TAB switch menu  Q quit  SP select  A set-artist  L set-album  T write-tracks  G select-all  M select-music  E edit");
    } else if (windows.state == edit) {
        mvwprintw(w, 0, 1, "TAB switch menu  Q quit  E edit  S save  Up/Down move  Enter edit field");
    } else {
        mvwprintw(w, 0, 1, "%s", bottomtext);
    }
    wrefresh(w);
}

void drawDirectory(windowData *wd) {
    WINDOW* w = wd->window;
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

/* new helper to read a line in a window; returns 1 if user accepted (Enter),
   returns 0 if user cancelled via ESC (27) or Tab ('\t') */
int get_input_with_cancel(WINDOW *w, int y, int x, char *out, size_t max, const char *initial)
{
    int ch;
    size_t pos = 0;
    static char clipboard[512] = {0};
    if (initial) {
        size_t initlen = strlen(initial);
        if (initlen > max - 1) initlen = max - 1;
        strncpy(out, initial, initlen);
        out[initlen] = '\0';
        pos = initlen;
    } else {
        out[0] = '\0';
        pos = 0;
    }

    /* display initial and enable keypad for arrow keys */
    mvwprintw(w, y, x, "%s", out);
    wmove(w, y, x + pos);
    wrefresh(w);
    keypad(w, TRUE);

    /* manual input loop with insertion at cursor and simple clipboard */
    while ((ch = wgetch(w)) != '\n' && ch != '\r' && ch != KEY_ENTER) {
        if (ch == 27 || ch == '\t') { /* ESC or Tab => cancel */
            return 0;
        } else if (ch == KEY_LEFT) {
            if (pos > 0) pos--;
        } else if (ch == KEY_RIGHT) {
            size_t curlen = strlen(out);
            if (pos < curlen) pos++;
        } else if (ch == 3) { /* Ctrl-C => copy field to internal clipboard */
            strncpy(clipboard, out, sizeof(clipboard)-1);
            clipboard[sizeof(clipboard)-1] = '\0';
        } else if (ch == 22) { /* Ctrl-V => paste internal clipboard at cursor */
            size_t clen = strlen(clipboard);
            size_t curlen = strlen(out);
            if (clen > 0 && curlen < max - 1) {
                size_t can_insert = (max - 1) - curlen;
                size_t ins = clen > can_insert ? can_insert : clen;
                memmove(out + pos + ins, out + pos, curlen - pos + 1);
                memcpy(out + pos, clipboard, ins);
                pos += ins;
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            size_t curlen = strlen(out);
            if (pos > 0) {
                pos--;
                memmove(out + pos, out + pos + 1, curlen - pos);
            }
        } else if (isprint(ch)) {
            size_t curlen = strlen(out);
            if (curlen < max - 1) {
                if (pos == curlen) {
                    out[pos++] = (char)ch;
                    out[pos] = '\0';
                } else {
                    memmove(out + pos + 1, out + pos, curlen - pos + 1);
                    out[pos++] = (char)ch;
                }
            }
        }
        /* redraw and position cursor */
        mvwprintw(w, y, x, "%s", out);
        wmove(w, y, x + strlen(out));
        wclrtoeol(w);
        wmove(w, y, x + pos);
        wrefresh(w);
    }
    /* Enter pressed => accept */
    return 1;
}

void drawEditor(windowData *wd) {
    WINDOW* w = wd->window;
    wclear(w);

    if (!fileSelected) {
        mvwprintw(w, 1, 1, "No file selected. Press Enter on a file to edit.");
        return;
    }

    int supported = 0;
    if (filenameEditing)
        supported = is_supported_audio_file(filenameEditing);

    if (!supported) {
        mvwprintw(w, 1, 1, "Selected: %s", filenameEditing ? filenameEditing : "");
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
            if (cnt > 0 && !fileDirty) {
                /* only reload common fields when the primary selected file changes */
                if (last_common_loaded[0] == '\0' || strcmp(last_common_loaded, filenames[0]) != 0) {
                    load_common_fields(filenames, cnt, title_buf, artist_buf, album_buf, track_buf);
                    strncpy(last_common_loaded, filenames[0], MAX_PATH_LEN-1);
                    last_common_loaded[MAX_PATH_LEN-1] = '\0';
                    /* clear preview cache when common selection changes */
                    last_preview_file[0] = '\0';
                }
                /* keep filenameEditing as the first selected for reference */
                filenameEditing = filenames[0];
            } else {
                /* no selected files or dirty editing - clear common cache so it reloads later */
                if (cnt == 0) last_common_loaded[0] = '\0';
            }
            mvwprintw(w, 1, 1, "Files : %d selected", cnt);
        } else {
            if (filenameEditing) {
                if (!fileDirty && (last_preview_file[0] == '\0' || strcmp(last_preview_file, filenameEditing) != 0)) {
                    load_id3_fields(filenameEditing);
                    strncpy(last_preview_file, filenameEditing, MAX_PATH_LEN-1);
                    last_preview_file[MAX_PATH_LEN-1] = '\0';
                    /* clear common cache when viewing single file */
                    last_common_loaded[0] = '\0';
                }
            } else {
                last_preview_file[0] = '\0';
                last_common_loaded[0] = '\0';
            }
            mvwprintw(w, 1, 1, "File  : %s", filenameEditing);
        }
    }

    /* Highlight hovered metadata field */
    if (windows.state == edit && editor_field == 0) wattron(w, A_REVERSE);
    /* If multiple selected, draw a two-column split: left = common/current,
       right = preview of hovered file */
    int mid = wd->width / 2;
    if (selected_count > 0) {
        /* left column (common/current) */
        if (windows.state == edit && editor_field == 0) wattron(w, A_REVERSE);
        mvwprintw(w, 2, 1, "Title : ");
        mvwprintw(w, 2, 9, "%s", title_buf[0] ? title_buf : "");
        if (windows.state == edit && editor_field == 0) wattroff(w, A_REVERSE);

        if (windows.state == edit && editor_field == 1) wattron(w, A_REVERSE);
        mvwprintw(w, 3, 1, "Artists: ");
        mvwprintw(w, 3, 9, "%s", artist_buf[0] ? artist_buf : "");
        if (windows.state == edit && editor_field == 1) wattroff(w, A_REVERSE);

        if (windows.state == edit && editor_field == 2) wattron(w, A_REVERSE);
        mvwprintw(w, 4, 1, "Album : ");
        mvwprintw(w, 4, 9, "%s", album_buf[0] ? album_buf : "");
        if (windows.state == edit && editor_field == 2) wattroff(w, A_REVERSE);

        if (windows.state == edit && editor_field == 3) wattron(w, A_REVERSE);
        mvwprintw(w, 5, 1, "Track : ");
        mvwprintw(w, 5, 9, "%s", track_buf[0] ? track_buf : "");
        if (windows.state == edit && editor_field == 3) wattroff(w, A_REVERSE);

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
        if (windows.state == edit && editor_field == 0) wattron(w, A_REVERSE);
        mvwprintw(w, 2, 1, "Title : ");
        mvwprintw(w, 2, 9, "%s", title_buf[0] ? title_buf : "");
        if (windows.state == edit && editor_field == 0) wattroff(w, A_REVERSE);

        if (windows.state == edit && editor_field == 1) wattron(w, A_REVERSE);
        mvwprintw(w, 3, 1, "Artists: ");
        mvwprintw(w, 3, 9, "%s", artist_buf[0] ? artist_buf : "");
        if (windows.state == edit && editor_field == 1) wattroff(w, A_REVERSE);

        if (windows.state == edit && editor_field == 2) wattron(w, A_REVERSE);
        mvwprintw(w, 4, 1, "Album : ");
        mvwprintw(w, 4, 9, "%s", album_buf[0] ? album_buf : "");
        if (windows.state == edit && editor_field == 2) wattroff(w, A_REVERSE);

        if (windows.state == edit && editor_field == 3) wattron(w, A_REVERSE);
        mvwprintw(w, 5, 1, "Track : ");
        mvwprintw(w, 5, 9, "%s", track_buf[0] ? track_buf : "");
        if (windows.state == edit && editor_field == 3) wattroff(w, A_REVERSE);

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
