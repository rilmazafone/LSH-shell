# LSH

A small interactive shell written in C, with the syscalls it needs hand-written in
ARM64 assembly — just for fun, and because "just call libc" is less interesting than
putting the syscall number in `x16` and trapping with `svc #0x80` yourself.

**Apple silicon only.** Everything here is arm64 macOS: `syscalls.S` issues raw Darwin
syscalls, and the constraint is enforced rather than documented — the `Makefile`
stops on a non-`Darwin`/non-`arm64` host, and both `syscalls.S` and `syscalls.h`
`#error` out if compiled for another target.

**The source carries no comments.** Everything that would normally be explained
inline — the syscall conventions, the terminal and signal handling, the line editor,
the history file, the full bug log — lives in [`INTERNALS.md`](INTERNALS.md), which
is the reference document for this project. Change the code, change that file.

## Build and run

```sh
make          # builds ./shell
./shell

make asan     # same sources with AddressSanitizer
make clean
```

## What it does

* interactive line editor: arrows, Home/End, Delete, backspace, insert in the middle
  of a line, redrawn with ANSI escapes
* command history: up/down arrows, persisted in `~/.lsh_history` and read back on the
  next start
* `^C` abandons the line instead of killing the shell, `^D` on an empty line exits,
  `^Z` restores the terminal before suspending
* built-ins: `cd` (no argument goes to `$HOME`), `help`, `exit`
* works non-interactively too: `echo "ls" | ./shell`, no prompt noise, clean exit

Not implemented (on purpose, so far): quoting, globbing, pipes, redirection,
variables, job control, tab completion.

## The syscalls (syscalls.S)

| wrapper | Darwin syscall | used for |
| --- | --- | --- |
| `sys_exit` | `exit` (1) | leaving the process |
| `sys_read` | `read` (3) | keyboard input, reading the history file |
| `sys_write` | `write` (4) | prompt, redraw, messages, history file |
| `sys_open` | `open` (5) | opening `~/.lsh_history` |
| `sys_close` | `close` (6) | closing it |
| `sys_lseek` | `lseek` (199) | sizing the history file before reading it |
| `sys_chdir` | `chdir` (12) | the `cd` built-in |
| `sys_getpid` | `getpid` (20) | reported by `help` |
| `sys_kill` | `kill` (37) | resuming a stopped child (`SIGCONT`) |

The kernel does not use libc's error convention: on failure it returns a **positive
errno with the carry flag set**, so `read()` against a bad descriptor hands you `9`
(EBADF) as if it were a byte count. Every wrapper translates that into `-1` plus
`errno`, and syscall numbers come from the SDK's `<sys/syscall.h>` rather than being
written as literals.

`fork(2)`/`execvp()` still come from libc, deliberately: PATH lookup, `argv`/`envp`
marshalling and the ENOEXEC fallback are not worth hand-rolling here.

## Fixed on 2026-10-15

Seventeen defects were found and fixed, thirteen of them pre-existing — including a
heap buffer overflow on history recall (ASAN: `WRITE of size 1206` into a 1024-byte
buffer), an infinite prompt loop on EOF (`./shell < /dev/null` wrote 3.4 MB of
prompts in three seconds), and the terminal being left in raw mode after `exit`,
because the shell exits through a raw `exit(2)` that never runs `atexit(3)` handlers.

The full list — symptom, cause, evidence and fix for each, plus the four bugs
introduced and caught in this session's own new code — is in
[`INTERNALS.md` §15](INTERNALS.md#15-bugs-found-and-fixed-on-2026-10-15), together
with the testing methodology that found them (a pty harness with a real controlling
terminal, plus ASAN) and the note that `detect_leaks=1` is unsupported on macOS.

## Credits

Started from [Write a Shell in C](https://brennan.io/2015/01/16/write-a-shell-in-c/)
by Stephen Brennan, then extended as I learned more about how a shell — and Darwin's
syscall interface — actually works.
