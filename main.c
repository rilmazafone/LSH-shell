#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/wait.h>

#include "syscalls.h"

#define LSH_PROMPT        "> "
#define LSH_RL_BUFFSIZE   1024
#define LSH_TOK_BUFSIZE   64
#define LSH_TOK_DELIM     " \t\r\n\a"
#define HISTORY_SIZE      200
#define HISTORY_FILE      ".lsh_history"
#define HISTORY_FILE_MAX  (256 * 1024)
#define ESC_TIMEOUT_MS    30

#define KEY_UP     256
#define KEY_DOWN   257
#define KEY_RIGHT  258
#define KEY_LEFT   259
#define KEY_HOME   260
#define KEY_END    261
#define KEY_DELETE 262
#define KEY_IGNORE 263

#define KEY_EOF     (-1)
#define KEY_INTR    (-2)
#define KEY_ERROR   (-3)
#define KEY_TIMEOUT (-4)

#define CTRL_D 4

static struct termios          orig_termios;
static volatile sig_atomic_t   interactive = 0;
static volatile sig_atomic_t   raw_mode_on = 0;
static volatile sig_atomic_t   got_intr = 0;
static volatile sig_atomic_t   got_cont = 0;

static void enable_raw_mode(void);
static void disable_raw_mode(void);
static void lsh_shutdown(int code) __attribute__((noreturn));

static char *edit_buffer = NULL;
static int   edit_cap = 0;
static int   edit_len = 0;
static int   edit_pos = 0;

static void print_prompt(void)
{
    if (interactive)
        sys_write(STDOUT_FILENO, LSH_PROMPT, sizeof LSH_PROMPT - 1);
}

static void print_str(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

static void print_err(const char *s)
{
    sys_write(STDERR_FILENO, s, strlen(s));
}

static void write_cursor_back(int n)
{
    char seq[16];
    int i = 0;

    if (n <= 0)
        return;

    seq[i++] = '\033';
    seq[i++] = '[';

    char digits[8];
    int k = 0;
    while (n > 0 && k < (int)sizeof digits) {
        digits[k++] = (char)('0' + n % 10);
        n /= 10;
    }
    while (k > 0)
        seq[i++] = digits[--k];

    seq[i++] = 'D';
    sys_write(STDOUT_FILENO, seq, (size_t)i);
}

static void edit_repaint(void)
{
    if (!interactive || edit_buffer == NULL)
        return;

    sys_write(STDOUT_FILENO, "\r\033[K", 4);
    sys_write(STDOUT_FILENO, LSH_PROMPT, sizeof LSH_PROMPT - 1);
    sys_write(STDOUT_FILENO, edit_buffer, (size_t)edit_len);
    write_cursor_back(edit_len - edit_pos);
}

static void enable_raw_mode(void)
{
    struct termios raw;

    if (!interactive || raw_mode_on)
        return;

    raw = orig_termios;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
        raw_mode_on = 1;
}

static void disable_raw_mode(void)
{
    if (!interactive || !raw_mode_on)
        return;

    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    raw_mode_on = 0;
}

static void reapply_raw_mode(void)
{
    if (!interactive)
        return;

    raw_mode_on = 0;
    enable_raw_mode();
}

static void sigint_handler(int sig)
{
    (void)sig;
    got_intr = 1;
}

static void sigcont_handler(int sig)
{
    (void)sig;
    got_cont = 1;
}

static void sigtstp_handler(int sig)
{
    (void)sig;

    disable_raw_mode();

    got_cont = 0;
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    signal(SIGTSTP, sigtstp_handler);

    enable_raw_mode();

    if (got_cont) {
        edit_repaint();
    } else {

        print_str("\nlsh: cannot suspend: no job control here\n");
        edit_repaint();
    }
}

static void fatal_signal_handler(int sig)
{
    disable_raw_mode();
    sys_exit(128 + sig);
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    sa.sa_handler = sigtstp_handler;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGTSTP, &sa, NULL);

    sa.sa_handler = sigcont_handler;
    sa.sa_flags = 0;
    sigaction(SIGCONT, &sa, NULL);

    sa.sa_handler = fatal_signal_handler;
    sa.sa_flags = 0;
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int read_byte(void)
{
    unsigned char c;
    ssize_t n = sys_read(STDIN_FILENO, &c, 1);

    if (n == 1)
        return (int)c;
    if (n == 0)
        return KEY_EOF;
    if (errno == EINTR)
        return KEY_INTR;
    return KEY_ERROR;
}

static int read_byte_timeout(int milliseconds)
{
    struct pollfd pfd;

    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, milliseconds) <= 0)
        return KEY_TIMEOUT;
    return read_byte();
}

static int lsh_getchar(void)
{
    int c = read_byte();

    if (c < 0)
        return c;
    if (!interactive || c != '\033')
        return c;

    c = read_byte_timeout(ESC_TIMEOUT_MS);
    if (c == KEY_TIMEOUT)
        return KEY_IGNORE;
    if (c < 0)
        return c;
    if (c != '[' && c != 'O')
        return KEY_IGNORE;

    int ss3 = (c == 'O');
    c = read_byte_timeout(ESC_TIMEOUT_MS);
    if (c == KEY_TIMEOUT)
        return KEY_IGNORE;
    if (c < 0)
        return c;

    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default: break;
    }

    if (!ss3 && c >= '0' && c <= '9') {
        int first = c;
        int last;
        do {
            last = read_byte_timeout(ESC_TIMEOUT_MS);
        } while (last >= '0' && last <= '9');
        if (last == KEY_TIMEOUT)
            return KEY_IGNORE;
        if (last < 0)
            return last;
        if (first == '3' && last == '~')
            return KEY_DELETE;
    }
    return KEY_IGNORE;
}

static char *history[HISTORY_SIZE];
static int   history_len = 0;
static int   history_idx = -1;

static void history_add(const char *line)
{
    if (line == NULL || line[0] == '\0')
        return;

    if (history_len > 0 && strcmp(history[history_len - 1], line) == 0)
        return;

    if (history_len == HISTORY_SIZE) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (HISTORY_SIZE - 1));
        history_len--;
    }

    char *copy = strdup(line);
    if (copy != NULL)
        history[history_len++] = copy;
}

static const char *history_path(void)
{
    static char path[PATH_MAX];
    static int  resolved = 0;

    if (!resolved) {
        const char *home = getenv("HOME");
        resolved = 1;
        if (home != NULL && home[0] != '\0')
            snprintf(path, sizeof path, "%s/%s", home, HISTORY_FILE);
    }
    return path[0] != '\0' ? path : NULL;
}

static void history_save_line(const char *line)
{
    const char *path = history_path();
    if (path == NULL)
        return;

    int fd = sys_open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;

    size_t left = strlen(line);
    const char *p = line;
    while (left > 0) {
        ssize_t n = sys_write(fd, p, left);
        if (n <= 0)
            break;
        p += n;
        left -= (size_t)n;
    }
    sys_write(fd, "\n", 1);
    sys_close(fd);
}

static void history_load(void)
{
    const char *path = history_path();
    if (path == NULL)
        return;

    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;

    off_t size = sys_lseek(fd, 0, SEEK_END);
    if (size > 0) {
        off_t start = 0;
        if (size > (off_t)HISTORY_FILE_MAX)
            start = size - (off_t)HISTORY_FILE_MAX;

        size_t want = (size_t)(size - start);
        char *buf = malloc(want + 1);
        if (buf != NULL && sys_lseek(fd, start, SEEK_SET) == start) {
            size_t got = 0;
            while (got < want) {
                ssize_t n = sys_read(fd, buf + got, want - got);
                if (n <= 0)
                    break;
                got += (size_t)n;
            }
            buf[got] = '\0';

            for (char *p = buf; *p != '\0'; ) {
                char *nl = strchr(p, '\n');
                if (nl != NULL)
                    *nl = '\0';
                history_add(p);
                p = (nl != NULL) ? nl + 1 : NULL;
                if (p == NULL)
                    break;
            }
        }
        free(buf);
    }
    sys_close(fd);
}

static void history_record(const char *line)
{
    history_add(line);
    history_save_line(line);
}

static int edit_ensure_capacity(int needed)
{
    if (needed <= edit_cap)
        return 0;

    int newcap = edit_cap;
    while (newcap < needed)
        newcap += LSH_RL_BUFFSIZE;

    char *grown = realloc(edit_buffer, (size_t)newcap);
    if (grown == NULL)
        return -1;

    edit_buffer = grown;
    edit_cap = newcap;
    return 0;
}

static void edit_set_line(const char *text)
{
    size_t len = strlen(text);

    if (edit_ensure_capacity((int)len + 1) != 0)
        return;

    memcpy(edit_buffer, text, len + 1);
    edit_len = edit_pos = (int)len;
    edit_buffer[edit_len] = '\0';
    edit_repaint();
}

static char *lsh_read_line(void)
{
    edit_cap = LSH_RL_BUFFSIZE;
    edit_buffer = malloc((size_t)edit_cap);
    if (edit_buffer == NULL) {
        print_err("lsh: out of memory\n");
        lsh_shutdown(1);
    }

    edit_len = edit_pos = 0;
    edit_buffer[0] = '\0';
    history_idx = history_len;
    got_intr = 0;

    for (;;) {

        int c = got_intr ? KEY_INTR : lsh_getchar();

        if (c == KEY_INTR) {
            got_intr = 0;
            edit_len = edit_pos = 0;
            edit_buffer[0] = '\0';
            history_idx = history_len;
            if (interactive) {
                sys_write(STDOUT_FILENO, "\n", 1);
                print_prompt();
            }
            continue;
        }

        if (c == KEY_ERROR) {
            edit_buffer[edit_len] = '\0';
            print_err("lsh: read error\n");
            free(edit_buffer);
            edit_buffer = NULL;
            return NULL;
        }

        if (c == KEY_EOF) {
            if (edit_len == 0) {
                free(edit_buffer);
                edit_buffer = NULL;
                return NULL;
            }
            c = '\n';
        }

        if (c == CTRL_D && edit_len == 0) {
            free(edit_buffer);
            edit_buffer = NULL;
            return NULL;
        }

        if (c == '\n' || c == '\r') {
            char *line;

            if (interactive)
                sys_write(STDOUT_FILENO, "\n", 1);

            edit_buffer[edit_len] = '\0';
            line = edit_buffer;
            edit_buffer = NULL;

            if (interactive && line[0] != '\0')
                history_record(line);
            history_idx = history_len;
            return line;
        }

        if (c == 127 || c == '\b') {
            if (edit_pos > 0) {
                memmove(edit_buffer + edit_pos - 1, edit_buffer + edit_pos,
                        (size_t)(edit_len - edit_pos));
                edit_pos--;
                edit_len--;
                edit_buffer[edit_len] = '\0';
                edit_repaint();
            }
            continue;
        }

        if (c == KEY_DELETE) {
            if (edit_pos < edit_len) {
                memmove(edit_buffer + edit_pos, edit_buffer + edit_pos + 1,
                        (size_t)(edit_len - edit_pos - 1));
                edit_len--;
                edit_buffer[edit_len] = '\0';
                edit_repaint();
            }
            continue;
        }

        if (c == KEY_UP) {
            if (history_idx > 0) {
                history_idx--;
                edit_set_line(history[history_idx]);
            }
            continue;
        }

        if (c == KEY_DOWN) {
            if (history_idx < history_len) {
                history_idx++;
                if (history_idx == history_len)
                    edit_set_line("");
                else
                    edit_set_line(history[history_idx]);
            }
            continue;
        }

        if (c == KEY_LEFT) {
            if (edit_pos > 0) {
                edit_pos--;
                sys_write(STDOUT_FILENO, "\033[D", 3);
            }
            continue;
        }

        if (c == KEY_RIGHT) {
            if (edit_pos < edit_len) {
                edit_pos++;
                sys_write(STDOUT_FILENO, "\033[C", 3);
            }
            continue;
        }

        if (c == KEY_HOME) {
            while (edit_pos > 0) {
                edit_pos--;
                sys_write(STDOUT_FILENO, "\033[D", 3);
            }
            continue;
        }

        if (c == KEY_END) {
            while (edit_pos < edit_len) {
                edit_pos++;
                sys_write(STDOUT_FILENO, "\033[C", 3);
            }
            continue;
        }

        if (c == KEY_IGNORE)
            continue;

        if (c < 32 || c > 255)
            continue;

        if (edit_ensure_capacity(edit_len + 2) != 0) {
            print_err("lsh: out of memory\n");
            lsh_shutdown(1);
        }
        memmove(edit_buffer + edit_pos + 1, edit_buffer + edit_pos,
                (size_t)(edit_len - edit_pos));
        edit_buffer[edit_pos] = (char)c;
        edit_pos++;
        edit_len++;
        edit_buffer[edit_len] = '\0';
        edit_repaint();
    }
}

static char **lsh_split_line(char *line)
{
    int buffsize = LSH_TOK_BUFSIZE;
    int position = 0;
    char **tokens = malloc((size_t)buffsize * sizeof(char *));

    if (tokens == NULL) {
        print_err("lsh: out of memory\n");
        lsh_shutdown(1);
    }

    char *token = strtok(line, LSH_TOK_DELIM);
    while (token != NULL) {
        tokens[position++] = token;

        if (position >= buffsize) {
            buffsize += LSH_TOK_BUFSIZE;
            char **grown = realloc(tokens, (size_t)buffsize * sizeof(char *));
            if (grown == NULL) {
                free(tokens);
                print_err("lsh: out of memory\n");
                lsh_shutdown(1);
            }
            tokens = grown;
        }
        token = strtok(NULL, LSH_TOK_DELIM);
    }
    tokens[position] = NULL;
    return tokens;
}

static void reset_child_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCONT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

static int lsh_launch(char **args)
{
    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid == 0) {

        reset_child_signals();
        if (raw_mode_on)
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);

        execvp(args[0], args);
        perror("lsh");
        sys_exit(1);
    }

    if (pid < 0) {
        perror("lsh: fork");
        return 1;
    }

    for (;;) {
        wpid = waitpid(pid, &status, WUNTRACED);
        if (wpid < 0) {
            if (errno == EINTR)
                continue;
            perror("lsh: waitpid");
            break;
        }
        if (WIFSTOPPED(status)) {

            sys_kill(pid, SIGCONT);
            continue;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
    }

    reapply_raw_mode();
    return 1;
}

static int lsh_cd(char **args);
static int lsh_help(char **args);
static int lsh_exit(char **args);

static char *builtin_str[] = {
    "cd",
    "help",
    "exit"
};

static int (*builtin_func[])(char **) = {
    &lsh_cd,
    &lsh_help,
    &lsh_exit
};

static int lsh_num_builtins(void)
{
    return (int)(sizeof(builtin_str) / sizeof(char *));
}

static int lsh_cd(char **args)
{
    const char *dir = args[1];

    if (dir == NULL)
        dir = getenv("HOME");
    if (dir == NULL || dir[0] == '\0') {
        print_err("lsh: cd: expected an argument and $HOME is not set\n");
        return 1;
    }

    if (sys_chdir(dir) != 0) {
        print_err("lsh: cd: ");
        print_err(dir);
        print_err(": ");
        print_err(strerror(errno));
        print_err("\n");
    }
    return 1;
}

static int lsh_help(char **args)
{
    (void)args;

    char buf[48];
    int n;

    print_str("LSH shell\n");
    print_str("Type program names and arguments, and hit enter.\n");
    print_str("The following are built in:\n");

    for (int i = 0; i < lsh_num_builtins(); i++) {
        print_str("  ");
        print_str(builtin_str[i]);
        print_str("\n");
    }
    print_str("Use the man command for information on other programs.\n");

    n = snprintf(buf, sizeof buf, "  (shell pid: %d)\n", (int)sys_getpid());
    if (n > 0)
        sys_write(STDOUT_FILENO, buf, (size_t)n);
    return 1;
}

static int lsh_exit(char **args)
{
    (void)args;
    return 0;
}

static int lsh_execute(char **args)
{
    if (args[0] == NULL)
        return 1;

    for (int i = 0; i < lsh_num_builtins(); i++) {
        if (strcmp(args[0], builtin_str[i]) == 0)
            return (*builtin_func[i])(args);
    }
    return lsh_launch(args);
}

static void lsh_loop(void)
{
    int status = 1;

    while (status) {
        print_prompt();

        char *line = lsh_read_line();
        if (line == NULL)
            break;

        char **args = lsh_split_line(line);
        status = lsh_execute(args);

        free(line);
        free(args);
    }
}

static void lsh_shutdown(int code)
{
    fflush(NULL);
    disable_raw_mode();
    sys_exit(code);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    interactive = isatty(STDIN_FILENO);
    if (interactive && tcgetattr(STDIN_FILENO, &orig_termios) != 0)
        interactive = 0;

    install_signal_handlers();
    atexit(disable_raw_mode);

    if (interactive) {
        history_load();
        enable_raw_mode();
    }

    lsh_loop();
    lsh_shutdown(0);
}
