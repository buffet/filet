/* Copyright (c), Niclas Meyer <niclas@countingsort.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define ENT_ALLOC_NUM 64

struct direlement {
    enum {
        TYPE_DIR,
        TYPE_SYML,
        TYPE_EXEC,
        TYPE_NORM,
    } type;

    const char *d_name;
};

static struct termios g_old_termios;
static int g_lines;

/**
 * Got too used to rust. This falls back to fallback, if name isn't set
 */
static const char *
getenv_or(const char *name, const char *fallback)
{
    const char *res = getenv(name);
    if (!res) {
        return fallback;
    }
    return res;
}

/**
 * Comparison function for direlements
 */
static int
direlemcmp(const void *va, const void *vb)
{
    const struct direlement *a = va;
    const struct direlement *b = vb;

    return strcmp(a->d_name, b->d_name);
}

/**
 * Sets the terminal size on g_lines
 */
static bool
get_term_size(void)
{
    struct winsize wsize;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsize) < 0) {
        perror("ioctl");
        return false;
    }

    g_lines = wsize.ws_row;

    return true;
}

/**
 * Used as SIGWINCH (terminal resize handler)
 */
static void
handle_winch(int sig)
{
    signal(sig, SIG_IGN);
    get_term_size();
    signal(sig, handle_winch);
}

/**
 * Resets the terminal to it's prior state
 */
static void
restore_terminal(void)
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old_termios) < 0) {
        perror("tcsetattr");
    }

    printf(
        "\033[?7h"    // enable line wrapping
        "\033[?25h"   // unhide cursor
        "\033[;r"     // reset scroll region
        "\033[?1049l" // restore main screen
    );
}

/**
 * Sets up the terminal for TUI use (read every char, differentiate \r and \n,
 * don't echo, hide the cursor, fix a scroll region, switch to a second screen)
 */
static bool
setup_terminal(void)
{
    setvbuf(stdout, NULL, _IOFBF, 0);

    if (tcgetattr(STDIN_FILENO, &g_old_termios) < 0) {
        perror("tcgetattr");
        return false;
    }

    atexit(restore_terminal);

    struct termios raw = g_old_termios;
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ICANON);

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        perror("tcsetattr");
        return false;
    }

    printf(
        "\033[?1049h" // use alternative screen buffer
        "\033[?7l"    // diable line wrapping
        "\033[?25l"   // hide cursor
        "\033[2J"     // clear screen
        "\033[1;%dr", // limit scrolling to scrolling area
        g_lines);

    return true;
}

/**
 * Read a directory into ents.
 *
 * Returns the number of elements in the dir.
 */
static size_t
read_dir(
    const char *path,
    struct direlement **ents,
    size_t *ents_size,
    DIR **last_dir,
    bool show_hidden)
{
    size_t n = 0;
    DIR *dir;
    if (*last_dir) {
      closedir(*last_dir);
    }
    dir = opendir(path);
    *last_dir = dir;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir))) {
            const char *name = ent->d_name;
            int fd           = dirfd(dir);
            struct stat sb;

            if (name[0] == '.' &&
                (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                continue;
            }

            if (!show_hidden && name[0] == '.') {
                continue;
            }

            if (fstatat(fd, name, &sb, AT_SYMLINK_NOFOLLOW) < 0) {
                continue;
            }

            if (n == *ents_size) {
                *ents_size += ENT_ALLOC_NUM;
                struct direlement *tmp =
                    realloc(*ents, *ents_size * sizeof(*tmp));
                if (!tmp) {
                    perror("realloc");
                    exit(EXIT_FAILURE);
                }
                *ents = tmp;
            }

            (*ents)[n].d_name = ent->d_name;

            if (S_ISDIR(sb.st_mode)) {
                (*ents)[n].type = TYPE_DIR;
            } else if (S_ISLNK(sb.st_mode)) {
                (*ents)[n].type = TYPE_SYML;
            } else {
                if (sb.st_mode & S_IXUSR) {
                    (*ents)[n].type = TYPE_EXEC;
                } else {
                    (*ents)[n].type = TYPE_NORM;
                }
            }

            ++n;
        }
        qsort(*ents, n, sizeof(**ents), direlemcmp);
    }

    return n;
}

/**
 * Redraw the whole UI. Rarely needed
 */
static void
redraw(
    struct direlement *ents,
    size_t n,
    size_t sel,
    const char *path,
    const char *user,
    const char *hostname)
{
    printf("\033[2J\033[H"); // clear the screen
    printf("\033[32;1m%s", user);
    if (hostname) {
        printf("@%s", hostname);
    }

    printf("\033[0m:\033[34;1m%s\r\n\n", path);

    if (n == 0) {
        printf("\033[31;7mdirectory empty\033[27m");
    }

    for (size_t i = 0; i < n; ++i) {
        switch (ents[i].type) {
        case TYPE_DIR:
            printf("\033[34;1m");
            break;
        case TYPE_SYML:
            printf("\033[36;1m");
            break;
        case TYPE_EXEC:
            printf("\033[32;1m");
            break;
        case TYPE_NORM:
            printf("\033[0m");
            break;
        }

        if (i == sel) {
            printf(">  %s\r\n", ents[i].d_name);
        } else {
            printf("  %s\r\n", ents[i].d_name);
        }
    }
}

/**
 * Spawns a new process, waits for it and returns
 */
static void
spawn(const char *path, const char *cmd, const char *argv1)
{
    int status;
    pid_t pid = fork();

    if (pid < 0) {
        return;
    }

    restore_terminal();
    fflush(stdout);

    if (pid == 0) {
        if (chdir(path) < 0) {
            _exit(EXIT_FAILURE);
        }
        execlp(cmd, cmd, argv1, NULL);
        // NOTREACHED
        _exit(EXIT_FAILURE);
    } else {
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }

    setup_terminal();
}

int
main(int argc, char **argv)
{
    if (!(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))) {
        fprintf(stderr, "isatty: not connected to a tty");
        exit(EXIT_FAILURE);
    }

    char *path = malloc(PATH_MAX);
    if (!path) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    if (argc > 1) {
        path = strcpy(path, argv[1]);
    } else {
        if (!getcwd(path, PATH_MAX)) {
            perror("getcwd");
            exit(EXIT_FAILURE);
        }
    }

    const char *editor = getenv_or("EDITOR", "vi");
    const char *shell  = getenv_or("SHELL", "/bin/sh");
    const char *home   = getenv_or("HOME", "/");
    const char *user   = getlogin();

    char *hostname = malloc(HOST_NAME_MAX);
    if (!hostname) {
        perror("malloc");
    }

    if (gethostname(hostname, HOST_NAME_MAX) < 0) {
        perror("gethostname");
        free(hostname);
        hostname = NULL;
    }

    size_t ents_size        = ENT_ALLOC_NUM;
    struct direlement *ents = malloc(ents_size * sizeof(*ents));
    if (!ents) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    if (!get_term_size()) {
        exit(EXIT_FAILURE);
    }

    if (signal(SIGWINCH, handle_winch) == SIG_ERR) {
        perror("signal");
        exit(EXIT_FAILURE);
    }

    if (!setup_terminal()) {
        exit(EXIT_FAILURE);
    }

    bool show_hidden = false;
    bool fetch_dir   = true;
    size_t sel       = 0;
    DIR *last_dir    = 0;
    size_t n;

    for (;;) {
        if (fetch_dir) {
            fetch_dir = false;
            n         = read_dir(path, &ents, &ents_size, &last_dir,
                                 show_hidden);
        }

        redraw(ents, n, sel, path, user, hostname);

        fflush(stdout);

        switch (getchar()) {
        case 'j':
            if (sel < n - 1) {
                ++sel;
            }
            break;
        case 'k':
            if (sel > 0) {
                --sel;
            }
            break;
        case 'h':
            dirname(path);
            sel       = 0;
            fetch_dir = true;
            break;
        case 'l':
            if (n > 0 && ents[sel].type == TYPE_DIR) {
                strcat(path, "/");
                strcat(path, ents[sel].d_name);
                sel       = 0;
                fetch_dir = true;
            }
            break;
        case '~':
            strcpy(path, home);
            sel       = 0;
            fetch_dir = true;
            break;
        case '/':
            strcpy(path, "/");
            sel       = 0;
            fetch_dir = true;
            break;
        case '.':
            show_hidden = !show_hidden;
            sel         = 0;
            fetch_dir   = true;
            break;
        case 'r':
            fetch_dir = true;
            break;
        case 'g':
            sel = 0;
            break;
        case 'G':
            sel = n - 1;
            break;
        case 'e':
            spawn(path, editor, ents[sel].d_name);
            sel       = 0;
            fetch_dir = true;
            break;
        case 's':
            spawn(path, shell, NULL);
            sel       = 0;
            fetch_dir = true;
            break;
        case 'q':
            exit(EXIT_SUCCESS);
            break;
        }
    }
}
