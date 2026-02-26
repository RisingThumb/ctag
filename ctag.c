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
int isRegularFile(char* filename);
int compare(const void* pa, const void *pb);
void resizehandler(int sig);
void terminal_stop();
void terminal_start();
void get_window_dimensions();

/* key functions */
void kbf_quit(void);
void kbf_enter(void);
void kbf_tab(void);
void kbf_up(void);
void kbf_down(void);
void kbf_resize(void);

char dirlines[DIRECTORYLINES][MAXDIRWIDTH]; /* Consider a malloc approach, so it extends to any directory with size>1000 */
char taglines[DIRECTORYLINES][MAXDIRWIDTH];


char *filenameEditing;
int fileSelected = 0;
int fileDirty = 0;
int updateEditor = 0;

char * toptext = "Made by RisingThumb          https://risingthumb.xyz ";
char * bottomtext = "TAB switch menu    Q to quit    E edit    S save";

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
    for(render(); (ch = tolower(getch())) != keyTable[0].key; render()){
        for (i = 1; i < (sizeof(keyTable) / sizeof(struct keyData)); i++) {
            if (ch == keyTable[i].key) {
                (*keyTable[i].kfunc)();
            }
        }
        /* additional keys for edit mode */
        if (windows.state == edit) {
            if (ch == 'e') {
                /* enter simple editing prompt */
                edit_mode = 1;
                echo();
                curs_set(1);
                WINDOW *w = windows.editor->window;
                mvwprintw(w, 3, 1, "Title : ");
                wclrtoeol(w);
                wrefresh(w);
                wgetnstr(w, title_buf, sizeof(title_buf)-1);

                mvwprintw(w, 4, 1, "Artist: ");
                wclrtoeol(w);
                wrefresh(w);
                wgetnstr(w, artist_buf, sizeof(artist_buf)-1);

                mvwprintw(w, 5, 1, "Album : ");
                wclrtoeol(w);
                wrefresh(w);
                wgetnstr(w, album_buf, sizeof(album_buf)-1);

                noecho();
                curs_set(CURSOR_INVIS);
                fileDirty = 1;
                edit_mode = 0;
            }
            else if (ch == 's') {
                if (fileSelected && fileDirty) {
                    /* save using id3v2 commandline */
                    char cmd[2048];
                    /* Escape single quotes in filename by wrapping with double quotes */
                    snprintf(cmd, sizeof(cmd), "id3v2 --song \"%s\" --artist \"%s\" --album \"%s\" \"%s\" 2>/dev/null", title_buf, artist_buf, album_buf, filenameEditing);
                    system(cmd);
                    fileDirty = 0;
                }
            }
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
            filenameEditing = dirlines[*sel_id];
            fileSelected = 1;
            updateEditor = 1;
        }
        else {
            if (chdir(dirlines[*sel_id]) == 0)
                (*sel_id) = 0;
            getDirectoryInfo(size);
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
    }
    return;
}

void kbf_down() {
    if (windows.state == dir) {
        int* sel_id = &windows.directory->sel_id;
        int* size = &windows.directory->dir_size;
        if ((*sel_id) < (*size) - 1)
            (*sel_id) += 1;
    }
    return;
}

int isRegularFile(char* filename) {
    struct stat file_stat;
    stat(filename, &file_stat);
    return S_ISREG(file_stat.st_mode);
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
}

int compare(const void* pa, const void *pb) {
    const char *a = (const char*)pa;
    const char *b = (const char*)pb;
    return strcmp(a, b);
}

/* Load ID3 fields via id3v2 CLI into title_buf/artist_buf/album_buf. */
void load_id3_fields(const char *filename) {
    /* clear buffers */
    title_buf[0] = '\0';
    artist_buf[0] = '\0';
    album_buf[0] = '\0';
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
    drawWindow(windows.bottompanel);
    refresh();
    wrefresh(windows.toppanel->window);
    wrefresh(windows.bottompanel->window);
    wrefresh(windows.directory->window);
    wrefresh(windows.editor->window);
}

void drawDirectory(windowData *wd) {
    WINDOW* w = wd->window;
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
        mvwprintw(w, i+1, 1, dirlines[i+start_i]);
        wattroff(w, A_REVERSE | COLOR_PAIR(panel) | COLOR_PAIR(normal));
    }
}

int ext_match(const char *name, const char *ext)
{
	size_t nl = strlen(name), el = strlen(ext);
	return nl >= el && !strcmp(name + nl - el, ext);
}

void drawEditor(windowData *wd) {
    WINDOW* w = wd->window;
    wclear(w);

    if (!fileSelected) {
        mvwprintw(w, 1, 1, "No file selected. Press Enter on a file to edit.");
        return;
    }

    int mp3 = 0;
    if (filenameEditing)
        mp3 = ext_match(filenameEditing, ".mp3");

    if (!mp3) {
        mvwprintw(w, 1, 1, "Selected: %s", filenameEditing ? filenameEditing : "");
        mvwprintw(w, 2, 1, "Not an MP3 file - metadata editing not supported.");
        return;
    }

    /* load tag fields into buffers */
    load_id3_fields(filenameEditing);

    mvwprintw(w, 1, 1, "File  : %s", filenameEditing);
    mvwprintw(w, 2, 1, "Title : %s", title_buf[0] ? title_buf : "");
    mvwprintw(w, 3, 1, "Artist: %s", artist_buf[0] ? artist_buf : "");
    mvwprintw(w, 4, 1, "Album : %s", album_buf[0] ? album_buf : "");
    mvwprintw(w, 6, 1, "Press 'e' to edit fields, 's' to save changes.");
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
