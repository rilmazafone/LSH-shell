#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>

#define LSH_RL_BUFFSIZE 1024
#define LSH_TOK_BUFSIZE 64
#define LSH_TOK_DELIM " \t\r\n\a"
#define HISTORY_SIZE 100
#define KEY_UP    256
#define KEY_DOWN  257
#define KEY_RIGHT 258
#define KEY_LEFT  259

void sys_exit(int code);
ssize_t sys_write(int fd,const void *buf,size_t len);
ssize_t sys_read(int fd,void *buf,size_t len);

struct termios orig_termios;

static char *history[HISTORY_SIZE];
static int history_len = 0;
static int history_idx = -1;

void history_add(const char *line){
    if(history_len == HISTORY_SIZE){
        free(history[0]);
        memmove(history, history + 1, sizeof(char*) * (HISTORY_SIZE - 1));
        history_len--;
    }
    history[history_len++] = strdup(line);
}

static void redraw_line(const char *buf, int len, int pos) {
    write(STDOUT_FILENO, "\r\033[K", 4);
    write(STDOUT_FILENO, "> ", 2);
    write(STDOUT_FILENO, buf, len);

    if (len > pos) {
        char seq[16];
        int n = snprintf(seq, sizeof(seq), "\033[%dD", len - pos);
        write(STDOUT_FILENO, seq, n);
    }
}

void disable_raw_mode() {
   tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode); //restore on exit

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON); //turn off echo and canonical mode
    raw.c_cc[VMIN] = 1; //non-blocking read
    raw.c_cc[VTIME] = 0; //no timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int lsh_getchar(void) {
    unsigned char c;
    ssize_t n = sys_read(0, &c, 1);

    if (n <= 0)
        return EOF;

    if (c == 27) {  // ESC
        char seq[2];
        if (sys_read(0, seq, 2) == 2) {
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A': return KEY_UP;
                    case 'B': return KEY_DOWN;
                    case 'C': return KEY_RIGHT;
                    case 'D': return KEY_LEFT;
                }
            }
        }
        return EOF;
    }

    return c;
}

void sigint_handler(int sig){
    write(STDOUT_FILENO, "\n> ", 3);
}

void print(const char *s){
    size_t len = 0;
    while(s[len] != '\0') len++;
    sys_write(1,s,len);
}

char *lsh_read_line(void) {
    int buffsize = LSH_RL_BUFFSIZE;
    char *buffer = malloc(buffsize);
    if (!buffer) {
        sys_write(2, "Out of memory!\n", 15);
        sys_exit(1);
    }

    int len = 0;   /* number of chars in buffer */
    int pos = 0;   /* cursor position           */
    history_idx = history_len; /* start past end so "up" gives last entry */

    while (1) {
        int c = lsh_getchar();   /* returns 'w','s','a','d' for arrows */

        if (c == EOF) {
            buffer[len] = '\0';
            return buffer;
        }

        if (c == '\n' || c == '\r') {
            write(STDOUT_FILENO, "\n", 1);
            buffer[len] = '\0';
            if (len > 0) history_add(buffer);
            history_idx = history_len;
            return buffer;
        }

        /* Backspace */
        if (c == 127 || c == '\b') {
            if (pos > 0) {
                memmove(buffer + pos - 1, buffer + pos, len - pos);
                pos--;
                len--;
                buffer[len] = '\0';
                redraw_line(buffer, len, pos);
            }
            continue;
        }

        /* Arrow keys (mapped by lsh_getchar) */
        if (c == KEY_UP) {
            if (history_idx > 0) {
                history_idx--;
                int hlen = strlen(history[history_idx]);
                memcpy(buffer, history[history_idx], hlen + 1);
                len = pos = hlen;
                redraw_line(buffer, len, pos);
            }
            continue;
        }
        if (c == KEY_DOWN) {          
            if (history_idx < history_len) {
                history_idx++;
                if (history_idx == history_len) {
                    len = pos = 0;
                    buffer[0] = '\0';
                } else {
                    int hlen = strlen(history[history_idx]);
                    memcpy(buffer, history[history_idx], hlen + 1);
                    len = pos = hlen;
                }
                redraw_line(buffer, len, pos);
            }
            continue;
        }
        if (c == KEY_LEFT) {          
            if (pos > 0) {
                pos--;
                write(STDOUT_FILENO, "\033[D", 3);
            }
            continue;
        }
        if (c == KEY_RIGHT) { 
            if (pos < len) {
                pos++;
                write(STDOUT_FILENO, "\033[C", 3);
            }
            continue;
        }

        /* Ordinary printable character — insert at cursor */
        if (len + 1 >= buffsize) {
            buffsize += LSH_RL_BUFFSIZE;
            buffer = realloc(buffer, buffsize);
            if (!buffer) {
                sys_write(2, "Out of memory!\n", 15);
                sys_exit(1);
            }
        }
        memmove(buffer + pos + 1, buffer + pos, len - pos);
        buffer[pos] = c;
        pos++;
        len++;
        buffer[len] = '\0';
        redraw_line(buffer, len, pos);
    }
}

char **lsh_split_line(char *line) {
    int buffsize = LSH_RL_BUFFSIZE, position = 0;
    char **tokens = malloc(buffsize * sizeof(char*));
    char *token;

    if (!tokens) {
        sys_write(2, "Out of memory!\n",16);
        sys_exit(1);
    }
    
    token = strtok(line, LSH_TOK_DELIM);
    while (token != NULL) {
        tokens[position] = token;
        position++;

        if (position >= buffsize) {
            buffsize += LSH_TOK_BUFSIZE;
            tokens = realloc(tokens, buffsize * sizeof(char*));
            if (!tokens) {
                sys_write(2, "Out of memory!\n",16);
                sys_exit(1);
            }
        }
        token = strtok(NULL, LSH_TOK_DELIM);
    }
    tokens[position] = NULL;
    return tokens;
}

int lsh_launch(char **args) {
    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid == 0) {
        // Child process
        signal(SIGINT, SIG_DFL);
        if (execvp(args[0], args) == -1) {
            perror("lsh");
        }
        sys_exit(1);
    } else if (pid < 0) {
        // Fork failed
        perror("lsh");
    } else {
        // Parent process
        do {
            wpid = waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
    return 1;
}

int lsh_cd(char **args);
int lsh_help(char **args);
int lsh_exit(char **args);

char *builtin_str[] = {
    "cd",
    "help",
    "exit"
};

int(*builtin_func[])(char **) = {
    &lsh_cd,
    &lsh_help,
    &lsh_exit
};

int lsh_num_builtins() {
    return sizeof(builtin_str) / sizeof(char *);
}

int lsh_cd(char **args) {
    if (args[1] == NULL) {
        sys_write(2, "lsh: expected argument to \"cd\"\n", 32);
    } else {
        if (chdir(args[1]) != 0) {
            perror("lsh");
        }
    }
    return 1;
}

int lsh_help(char **args) {
    int i;
    print("LSH Shell\n");
    print("Type program names and arguments, and hit enter.\n");
    print("The following are built in:\n");

    for (i = 0; i < lsh_num_builtins(); i++) {
        printf("  %s\n", builtin_str[i]);
    }
    print("Use the man command for information on other programs.\n");
    return 1;
}

int lsh_exit(char **args) {
    return 0;
}

int lsh_execute(char **args) {
    int i;

    if (args[0] == NULL) {
        return 1;
    }
    
    for (i = 0; i < lsh_num_builtins(); i++) {
        if (strcmp(args[0], builtin_str[i]) == 0) {
            return (*builtin_func[i])(args);
        }
    }
    return lsh_launch(args);
}

void lsh_loop() {
    char *line;
    char **args;
    int status;
    enable_raw_mode();
    do {
        print("> ");
        line = lsh_read_line();
        args = lsh_split_line(line);
        status = lsh_execute(args);

        free(line);
        free(args);
    } while (status);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);
    lsh_loop();
    sys_exit(0);
}
