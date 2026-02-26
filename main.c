#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>


#define LSH_RL_BUFFSIZE 1024
#define LSH_TOK_BUFSIZE 64
#define LSH_TOK_DELIM " \t\r\n\a"
void sys_exit(int code);
ssize_t sys_write(int fd,const void *buf,size_t len);
ssize_t sys_read(int fd,void *buf,size_t len);

int lsh_getchar(void){
    unsigned char c;
    ssize_t n = sys_read(0,&c,1);

    if(n<=0){
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
    int position = 0;
    char *buffer = malloc(sizeof(char) * buffsize);
    int c;

    if (!buffer) {
        sys_write(2, "Out of memory!\n",16);
        sys_exit(1);
    }
    
    while (1) {
        c = lsh_getchar();
        if (c == EOF || c == '\n') {
            buffer[position] = '\0';
            return buffer;
        } else {
            buffer[position] = c;
        }
        position++;

        if (position >= buffsize) {
            buffsize += LSH_RL_BUFFSIZE;
            buffer = realloc(buffer, buffsize);
            if (!buffer) {
                sys_write(2, "Out of memory!\n",16);
                sys_exit(1);
            }
        }
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
