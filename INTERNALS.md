# LSH internals

Reference document for this project. **The source files carry no comments on
purpose** — `main.c`, `syscalls.S`, `syscalls.h` and the `Makefile` are bare code,
and everything that would normally live in a comment lives here instead. If you
change the code, change this file in the same commit.

Contents:

1. [What LSH is and the hard constraint](#1-what-lsh-is-and-the-hard-constraint)
2. [File map](#2-file-map)
3. [Build system](#3-build-system)
4. [Control flow at a glance](#4-control-flow-at-a-glance)
5. [The raw syscall layer](#5-the-raw-syscall-layer)
6. [Terminal handling](#6-terminal-handling)
7. [Signal handling](#7-signal-handling)
8. [Line editor](#8-line-editor)
9. [History](#9-history)
10. [Parsing](#10-parsing)
11. [Launching a command](#11-launching-a-command)
12. [Main loop, built-ins, exit paths](#12-main-loop-built-ins-exit-paths)
13. [Non-interactive mode](#13-non-interactive-mode)
14. [How this was tested](#14-how-this-was-tested)
15. [Bugs found and fixed on 2026-10-15](#15-bugs-found-and-fixed-on-2026-10-15)
16. [Known limitations](#16-known-limitations)
17. [Adding another syscall](#17-adding-another-syscall)
18. [Credits](#18-credits)

---

## 1. What LSH is and the hard constraint

LSH is a small interactive shell: a read/eval loop, a raw-mode line editor with
history, a tokenizer, and `fork`/`execvp` for external commands. It is a learning
project, so it deliberately talks to the kernel itself instead of libc wherever
that is interesting.

**It only runs on macOS on Apple silicon (arm64), and that is not a temporary
limitation.** `syscalls.S` issues raw Darwin syscalls — the number goes into `x16`
and the CPU traps with `svc #0x80`. That instruction and that calling convention
exist only on arm64, and the syscall numbers are Darwin's, so there is no x86_64 or
Linux variant of the same file. The constraint is enforced at three levels so it
cannot rot silently:

| Where | Guard |
| --- | --- |
| `Makefile` | `$(error ...)` if `uname -s` is not `Darwin`, or `uname -m` is not `arm64` |
| `syscalls.h` | `#error` unless `__APPLE__` **and** `__aarch64__` |
| `syscalls.S` | `#error` unless `__aarch64__` |

Verified behaviour: `make UNAME_S=Linux`, `make UNAME_M=x86_64`, and
`clang -arch x86_64 -c main.c` all fail with a clear message rather than producing
something broken.

## 2. File map

| File | Lines | Role |
| --- | --- | --- |
| `main.c` | ~801 | everything C: terminal, signals, editor, history, parsing, launch, built-ins, loop |
| `syscalls.S` | 48 | the raw arm64 syscall wrappers and the errno translation helper |
| `syscalls.h` | 24 | prototypes for the wrappers, arm64/macOS `#error` guard |
| `Makefile` | 37 | build, host guards, `asan` target, `clean` |
| `README.md` | — | short overview for someone arriving at the repo |
| `INTERNALS.md` | — | this document |
| `.gitignore` | 3 | `shell`, `*.o`, `*.dSYM/` |

## 3. Build system

```
make            # ./shell, clang -Wall -Wextra -std=c11 -O2, assembly with -Wall
make asan       # same sources with -g -fsanitize=address
make clean      # removes shell, main.o, syscalls.o
```

The `asan` target re-invokes make with extended `CFLAGS`/`LDFLAGS` after a `clean`,
because the objects have to be rebuilt for the sanitizer to be linked in.
`main.o` depends on `main.c` and `syscalls.h`; `syscalls.o` depends only on
`syscalls.S` (which pulls its numbers from the SDK's `<sys/syscall.h>`).

Both configurations build with **zero warnings** under `-Wall -Wextra`; that is
treated as part of correctness here, not as cosmetic tidiness.

## 4. Control flow at a glance

```
main()
  interactive = isatty(0) && tcgetattr(0, &orig_termios) == 0
  install_signal_handlers()
  atexit(disable_raw_mode)            safety net for any libc exit() path
  if (interactive) { history_load(); enable_raw_mode(); }
  lsh_loop()
      print_prompt()
      line = lsh_read_line()          NULL => end of input, leave the loop
      args = lsh_split_line(line)
      status = lsh_execute(args)      0 => leave the loop
      free(line); free(args)
  lsh_shutdown(0)
      fflush(NULL); disable_raw_mode(); sys_exit(0)
```

`lsh_execute` checks the three built-ins first (`cd`, `help`, `exit`) and otherwise
calls `lsh_launch`, which `fork`s, `execvp`s, and waits.

## 5. The raw syscall layer

### 5.1 The two conventions

Darwin's arm64 syscall interface is not the libc interface:

| | Kernel (arm64 Darwin) | libc |
| --- | --- | --- |
| number | in `x16` | — |
| trap | `svc #0x80` | — |
| success | result in `x0`, carry **clear** | result, `errno` untouched |
| failure | **positive errno in `x0`**, carry **set** | `-1`, `errno` set |

Conflating the two is not theoretical: with the original wrappers,
`sys_write(-1, "x", 1)` returned `9` — EBADF — *as if it were a byte count*, and
`errno` was never set. Any caller checking `n <= 0` saw a "successful" call, and
`lsh_getchar` would then return an uninitialised stack byte as the character the
user typed.

Every wrapper in `syscalls.S` therefore funnels failures through
`_lsh_syscall_fail`, which converts the kernel's convention into the C one
(`-1` with `errno` set through `__error()`), so `errno` after a failed call is
usable for `EINTR` checks and `strerror`.

### 5.2 The wrapper macro

`SYSCALL name, number` expands to a global symbol `_name` that moves the number
into `x16`, traps, and either returns the result (`b.cc 1f`) or branches to the
failure path. The failure path is entered with `b`, not `bl`, so `x30` still holds
the address the wrapper was called from.

`sys_exit` is written out by hand: `exit(2)` never returns and cannot fail, so it
needs no error path, and the status is masked with `and x0, x0, #0xff` because C
passes an `int` in `w0` and the upper half of `x0` is not guaranteed to be
meaningful.

The syscall numbers are not magic constants: `syscalls.S` includes
`<sys/syscall.h>` and uses `SYS_read`, `SYS_open`, and so on, so the table stays
readable and can be diffed against `bsd.syscalls.master`. All the numbers actually
used here were additionally confirmed at runtime on the target machine.

### 5.3 The wrappers

| Wrapper | Syscall | Prototype | Used for |
| --- | --- | --- | --- |
| `sys_exit` | `exit` (1) | `void sys_exit(int)` | leaving the process |
| `sys_read` | `read` (3) | `ssize_t sys_read(int, void *, size_t)` | keystrokes, reading the history file |
| `sys_write` | `write` (4) | `ssize_t sys_write(int, const void *, size_t)` | prompt, redraws, messages, history file |
| `sys_open` | `open` (5) | `int sys_open(const char *, int, int)` | opening `~/.lsh_history` |
| `sys_close` | `close` (6) | `int sys_close(int)` | closing it |
| `sys_lseek` | `lseek` (199) | `off_t sys_lseek(int, off_t, int)` | measuring the history file |
| `sys_chdir` | `chdir` (12) | `int sys_chdir(const char *)` | the `cd` built-in |
| `sys_getpid` | `getpid` (20) | `pid_t sys_getpid(void)` | reported by `help` |
| `sys_kill` | `kill` (37) | `int sys_kill(pid_t, int)` | resuming a stopped child with `SIGCONT` |

### 5.4 Why `fork`/`execvp` are still libc

Only the syscalls that are genuinely useful to the shell were hand-written. Process
creation is left to libc because `execvp` brings PATH searching, `argv`/`envp`
marshalling, and the ENOEXEC fallback (`execvp` re-runs the file through
`/bin/sh` when the kernel rejects it), and because calling raw `fork(2)` from a
libc-linked process skips libc's own bookkeeping for the child. `fork` here is
immediately followed by `execvp`, so the classic fork-in-a-libc-program hazards are
small, but they are not zero, and hand-rolling them buys nothing.

## 6. Terminal handling

### 6.1 What "raw mode" means here

The shell keeps the original settings in `orig_termios` and derives its own mode
from them when `enable_raw_mode` runs:

| Field | Change | Why |
| --- | --- | --- |
| `c_lflag` | clear `ECHO`, `ICANON` | the shell does its own line editing and drawing; no tty echo, no line assembly |
| `c_iflag` | clear `IXON`, `ICRNL` | `^S`/`^Q` must not freeze the terminal; `Enter` arrives as `\r` and is handled as such |
| `c_cc[VMIN]` | 1 | a read returns as soon as one byte is available |
| `c_cc[VTIME]` | 0 | no inter-byte timer |

`ISIG` is deliberately **left on**. Turning it off would stop the tty from
generating signals, which would break `^C` inside any child process the shell
starts — the child inherits the terminal, not just the shell's own key handling.

State is tracked with two globals: `interactive` (stdin is a usable terminal) and
`raw_mode_on` (the shell believes the tty is currently raw).

### 6.2 Lifecycle

| Transition | Function | Notes |
| --- | --- | --- |
| cooked → raw | `enable_raw_mode` | no-op unless `interactive`; no-op if already raw; sets `raw_mode_on` only if `tcsetattr` succeeded |
| raw → cooked | `disable_raw_mode` | no-op unless `interactive` and raw; used on every exit path and before a child `exec`s |
| raw → raw (forced) | `reapply_raw_mode` | clears `raw_mode_on` first, then enables: the tty may have been changed behind the shell's back |

`reapply_raw_mode` exists because of a bug found on 2026-10-15 (item 14 in the
change log): the child restores the cooked settings before `exec`, but the parent's
`raw_mode_on` still said "on", so the plain `enable_raw_mode` call after `waitpid`
returned immediately and the shell silently stayed cooked — no per-keystroke
redraw, no cursor movement, the tty echoing input instead.

### 6.3 Signal safety

`tcsetattr` is not on the POSIX list of async-signal-safe functions, yet the
`SIGTSTP` handler calls it. That is the same pragmatic choice bash makes: the
handler must hand back a usable terminal *before* the process stops, and nothing
shorter is possible. `edit_repaint` and the message writes in handlers are
`sys_write` calls and a small hand-rolled CSI formatter, so no `snprintf`, no
`malloc`, and no buffered stdio is involved there.

## 7. Signal handling

| Signal | Handler | Behaviour |
| --- | --- | --- |
| `SIGINT` | `sigint_handler` | sets `got_intr`; the line editor discards the line |
| `SIGQUIT` | `sigint_handler` | same as `^C`, so `^\` cannot kill the shell and leave the tty raw |
| `SIGTSTP` | `sigtstp_handler` | restores the tty, stops the shell, re-enters raw mode on resume |
| `SIGCONT` | `sigcont_handler` | sets `got_cont`: distinguishes "really stopped and resumed" from "stop was dropped" |
| `SIGHUP`, `SIGTERM` | `fatal_signal_handler` | restores the tty and exits with `128 + sig` |

### 7.1 No `SA_RESTART`

Handlers are installed with `sigaction` and `sa_flags = 0`, i.e. **without**
`SA_RESTART`. This is what makes `^C` immediate: the blocked `read(2)` fails with
`EINTR`, the wrapper returns `-1` with `errno = EINTR`, `read_byte` maps that to
`KEY_INTR`, and the editor abandons the line. With `SA_RESTART` the read would
silently resume and the interrupt would only be noticed at the next keystroke.

Because a signal can also land *between* two reads, the flag is authoritative: the
read loop checks `got_intr` before it tries to read anything, and the `KEY_INTR`
branch clears it. Without that, a `^C` that arrived while an escape sequence was
being decoded would be lost. The same reasoning applies inside `lsh_getchar`,
which propagates `KEY_EOF`/`KEY_INTR`/`KEY_ERROR` instead of swallowing them as
"an unrecognised sequence".

### 7.2 `^Z`, `SA_NODEFER`, and orphaned process groups

The shell has no job control, so `^Z` suspends the whole shell rather than a job.
The handler restores the terminal, then stops the process itself:

```
disable_raw_mode()
got_cont = 0
signal(SIGTSTP, SIG_DFL)
raise(SIGTSTP)          <-- the process stops here
signal(SIGTSTP, sigtstp_handler)
enable_raw_mode()
```

`SIGTSTP` is installed with **`SA_NODEFER`** for this to work at all: the signal
being handled is normally blocked for the duration of its handler, so
`raise(SIGTSTP)` would just leave the signal pending, the handler would run to
completion, and the code after `raise` would never see a stop. With `SA_NODEFER`
the stop happens inside `raise()`, and execution continues on `SIGCONT`.

There is a second trap, from POSIX: **a stop signal sent to an orphaned process
group is discarded.** If the shell is a session leader whose parent is in another
session — which is exactly what happens when a test harness or `setsid` starts it —
the kernel throws the stop away and `raise` returns immediately. That is invisible
to the user, so the handler reports it: if `got_cont` is still clear after the
`raise` returns, the stop was dropped and the shell prints
`lsh: cannot suspend: no job control here`. Under a normal job-control shell the
stop works, that shell says `suspended`, and `fg` sends `SIGCONT`, after which the
handler re-enters raw mode and repaints the line.

### 7.3 Child processes

`reset_child_signals` puts `SIGINT`, `SIGQUIT`, `SIGTSTP`, `SIGCONT`, `SIGHUP` and
`SIGTERM` back to `SIG_DFL` in the child, so a child never runs the shell's
handlers (which would try to touch the terminal or the editor state on its behalf).

## 8. Line editor

### 8.1 State

The editor keeps the current line in globals rather than locals:
`edit_buffer`, `edit_cap`, `edit_len`, `edit_pos` (cursor, `0..edit_len`). They are
globals because the `SIGTSTP` handler repaints the line after a resume and needs to
see it. `edit_buffer` is owned by `lsh_read_line` and handed to the caller on
success, with the global set to `NULL` so a handler cannot repaint a freed line.

Capacity starts at `LSH_RL_BUFFSIZE` (1024) and grows in 1024-byte steps via
`edit_ensure_capacity`, which is used by both the insert path and `edit_set_line`
(the history-recall path). Growth on recall is a deliberate fix: history entries
can be longer than the buffer the line started with, and the original code copied
them in regardless.

### 8.2 Keys

`lsh_getchar` returns either a byte (`0..255`) or one of the codes below, so no
value can be confused with real input:

| Code | Value | Meaning |
| --- | --- | --- |
| `KEY_UP` … `KEY_LEFT` | 256–259 | arrow keys |
| `KEY_HOME`, `KEY_END` | 260, 261 | go to start / end of line |
| `KEY_DELETE` | 262 | delete the character under the cursor |
| `KEY_IGNORE` | 263 | a sequence that was understood and consciously ignored |
| `KEY_EOF` | −1 | end of input (`read` returned 0, or `^D` on an empty line) |
| `KEY_INTR` | −2 | interrupted by a signal (`errno == EINTR`) |
| `KEY_ERROR` | −3 | hard read error |
| `KEY_TIMEOUT` | −4 | nothing arrived within the escape timeout |

`KEY_TIMEOUT` exists as its own value rather than being folded into `KEY_EOF`
(−1): `read_byte_timeout` has to report "nothing came" without that being
confused with "input ended", otherwise a truncated escape sequence would look like
end of input and the shell would exit (item 17 in the change log).

Escape sequences are decoded with a 30 ms `poll` timeout (`ESC_TIMEOUT_MS`) so that
a bare `ESC` keypress cannot block the shell, and so a sequence cannot be split
across a blocking read. Handled: `CSI` (`ESC [`) and `SS3` (`ESC O`) forms of the
four arrows plus Home/End, and `CSI <n> ~` with `n = 3` for Delete. Everything
else consumes what it needs and returns `KEY_IGNORE`, which is why Home/End/Delete
no longer leak their trailing bytes into the command line.

Editing operations available: insert anywhere, backspace, delete under the cursor,
left/right, Home/End, up/down through history. Redrawing is done by
`edit_repaint`, which emits `\r`, `ESC [ K` (clear line), the prompt, the buffer,
and then a hand-formatted `ESC [ <n> D` to put the cursor back where it belongs.
It repaints after every keystroke, which is simple and correct but O(n²) in output
volume for a long line.

Control bytes that mean nothing to the editor (`c < 32`, except the ones handled
explicitly) are dropped rather than inserted, so a stray `^A` can no longer end up
inside a filename. `^D` on an empty line ends the shell; on a non-empty line it is
ignored. `Tab` does nothing — there is no completion yet.

Multibyte UTF-8 input arrives byte by byte and is stored and counted as bytes, so
the cursor moves by byte, not by character. Everything works, but the cursor can
land inside a multibyte character. Proper character handling is not implemented.

## 9. History

### 9.1 In memory

`history` is a fixed array of `HISTORY_SIZE` (200) `char *` copies, oldest first,
with `history_len` entries and `history_idx` as the recall cursor. When the array
is full the oldest entry is freed and the rest are shifted down, so the newest 200
commands are kept. `history_add` ignores empty lines and skips a line identical to
the previous one, so holding Enter or re-running the same command does not fill the
list with copies.

### 9.2 On disk

The file is `$HOME/.lsh_history`, one command per line, created with mode 0644.

* **Writing** — `history_save_line` opens it `O_WRONLY | O_CREAT | O_APPEND` and
  writes the line plus `\n`, looping over `sys_write` because a single call may
  transfer fewer bytes than asked. It is an append-only log: every accepted line is
  appended, including an immediate repeat that `history_add` skipped in memory, so
  the file is a faithful record of what was typed.
* **Reading** — `history_load` opens the file, `lseek(fd, 0, SEEK_END)` to size it,
  `lseek` back to the start (or to `size - HISTORY_FILE_MAX` when the file is
  larger than 256 KiB, keeping the newest entries), reads it in a loop into a
  heap buffer, splits on `\n`, and feeds each line to `history_add`. Because
  `history_add` evicts the oldest when full, memory ends up holding the newest 200
  lines of the tail.

Both directions use the assembly wrappers — `open`, `lseek`, `read`, `write`,
`close`. Nothing is loaded or saved when `HOME` is unset, and errors are silent: a
shell that cannot write its history should not nag.

The file grows without a hard limit across sessions. The read side is capped, so a
large file only costs startup time; trimming on exit is a possible future
improvement.

## 10. Parsing

`lsh_split_line` splits the line in place with `strtok` on
`LSH_TOK_DELIM` (`space`, `tab`, `\r`, `\n`, bell), returning a `NULL`-terminated
array of pointers into the line. The array starts at `LSH_TOK_BUFSIZE` (64)
pointers and grows by 64 as needed; on allocation failure the old array is freed
before exiting, and `position >= buffsize` is checked before the next store, so the
terminator write is always in bounds.

`strtok` means: no quoting, no escaping, no globbing, no variable expansion, no
operators, and no way to pass an argument containing a space. The caller owns both
the line and the array (`lsh_loop` frees both after each command) and the array
only contains pointers into the line, so the line must outlive it.

## 11. Launching a command

`lsh_launch` is the only place `fork` is used:

1. **Child** — reset the signals, restore the original terminal settings if the
   shell had them raw (so `cat`, `python`, `vim`, and anything else that reads the
   terminal gets echo and canonical line editing), `execvp`, and on failure
   `perror` plus `sys_exit(1)`. The raw `exit` is deliberate: it skips `atexit`
   handlers, so the child cannot run the parent's terminal cleanup or flush an
   inherited stdio buffer a second time.
2. **Parent** — `waitpid(pid, &status, WUNTRACED)` in a loop:
   * `wpid < 0` with `EINTR` → retry; any other error → `perror` and stop waiting,
     so an error cannot turn into an infinite loop over a stale `status`;
   * `WIFSTOPPED` → send `SIGCONT` with the raw `kill` wrapper and keep waiting.
     Without job control a stopped child would block this loop forever;
   * `WIFEXITED` or `WIFSIGNALED` → done.
3. Then `reapply_raw_mode`, because the child handed back a cooked terminal (see
   §6.2).

Children stay in the shell's process group, which is what makes `^C` reach them —
the tty signals the foreground group, not just the shell.

## 12. Main loop, built-ins, exit paths

| Built-in | Behaviour |
| --- | --- |
| `cd` | with an argument, `sys_chdir`; without one, `$HOME`. Failure prints `lsh: cd: <dir>: <strerror(errno)>` using the `errno` set by the wrapper. Always returns 1 (keep running) |
| `help` | lists the built-ins and the shell's pid (`sys_getpid`) |
| `exit` | returns 0, which ends the read loop |

`lsh_loop` runs while `status != 0`; a `NULL` line from the editor (end of input)
breaks out. Everything the loop allocates is freed each iteration.

Every exit goes through `lsh_shutdown(code)`: `fflush(NULL)` for anything stdio may
still hold, `disable_raw_mode()`, then `sys_exit(code)`. This is the fix for the
worst user-visible bug in the original shell: `sys_exit` is a raw `exit(2)`, so
`atexit(3)` handlers never run, and the terminal used to stay in raw mode after
`exit` — the classic "my shell broke my terminal" symptom. `atexit(disable_raw_mode)`
is still registered as a safety net for any path that would use libc `exit()`.

## 13. Non-interactive mode

`interactive` is false when stdin is not a terminal, or when its settings cannot be
read. In that mode:

* no prompt is printed and no repaint or cursor escape is ever emitted, so piping
  produces exactly the commands' output;
* escape-sequence decoding is skipped, so bytes are taken literally;
* end of input ends the shell. The original treated a zero-length read as an empty
  command and looped forever: `./shell < /dev/null` produced 3.4 MB of prompts in
  three seconds and never exited. Now `read` returning 0 means "no more input";
* a partial line at end of input is executed like a line ended by Enter.

`echo "ls" | ./shell` therefore runs `ls` and exits 0, and `./shell < /dev/null`
prints nothing and exits immediately.

## 14. How this was tested

No test framework: a Python harness drives the binary over a pty and asserts on
bytes, terminal state, side effects, and process status. Three lessons from
building it, because they were the difference between finding these bugs and
missing them:

* **Use a real controlling terminal.** `pty.fork()` gives the child a controlling
  tty as session leader. An earlier version used `pty.openpty()` plus
  `subprocess` with `preexec_fn=os.setsid`, which leaves the child with *no*
  controlling terminal: the tty then delivers `^C`/`^Z` to nobody, and the tests
  silently pass while the real thing is broken.
* **Assert on effects, not on echoes.** "The abandoned line is not executed" is
  checked with `touch` and a file-existence check, not by looking for the command
  text in the transcript — the text is always in the transcript, because the editor
  echoes what is typed. One test did pass spuriously this way until it was
  tightened.
* **Assert the terminal mode too.** Behaviour checks passed while the tty was
  silently in canonical mode; reading `tcgetattr` on the pty and asserting on
  `ECHO`/`ICANON` is what caught item 14. Note that a job-control parent shell
  (zsh) also runs its own editor with `ECHO`/`ICANON` off, so that value alone is
  ambiguous under zsh — check lsh directly, or check for its redraw sequence.

The suite covers 30 checks: terminal restore on `exit`, `^D`, and fatal signals;
`^C` discarding the line; `^Z` both with and without job control, including the
`SIGCONT` resume path; recall of an oversized history entry; child echo plus raw
mode restoration and cursor editing afterwards; `cd` success, `$HOME` fallback, and
error text; history file write/read round-trip and duplicate handling; piped input,
clean exit, and EOF. It passes 30/30 against both the release and `make asan`
builds.

A separate check feeds truncated escape sequences — `ESC`, `ESC [`, `ESC [ 1`,
`ESC [ 1;5` — and then asserts that the shell is still alive and still runs the next
command. It is not part of the 30 because it needs a pause longer than the 30 ms
sequence timeout; the pre-fix build died on `ESC [ 1` (item 17).

AddressSanitizer notes: `detect_leaks=1` is **not supported on macOS** and the ASAN
runtime aborts at startup with `detect_leaks is not supported on this platform`, so
leak checking is off; heap bounds, use-after-free, and stack checks all work and
reported nothing after the fixes.

The comment removal in this document's companion change was verified to be
semantics-preserving rather than eyeballed: `clang -E -P` output for `main.c` and
`syscalls.S` was compared before and after — 8469 identical preprocessor tokens for
`main.c`, byte-identical output for `syscalls.S` and `syscalls.h` — followed by a
full rebuild and a 30/30 suite run.

## 15. Bugs found and fixed on 2026-10-15

Everything below was reproduced before being fixed. Items 1–13 are pre-existing
bugs in the shell; items 14–17 were introduced by this session's own work and are
listed because they are part of the record.

| # | Severity | Symptom | Cause | Fix |
| --- | --- | --- | --- | --- |
| 1 | critical | heap corruption, possible crash | recalling a history entry `memcpy`'d the whole entry into a 1024-byte buffer that only grew while typing | `edit_ensure_capacity` / `edit_set_line` grow the buffer before the copy |
| 2 | critical | `./shell < /dev/null` spun forever writing 3.4 MB of prompts in 3 s | `read` returning 0 was treated as an empty command, and `lsh_execute` returned 1 | zero-length read ends input; `lsh_read_line` returns `NULL` |
| 3 | critical | terminal left in raw mode after `exit` | the shell exits through raw `exit(2)`, which never runs the registered `atexit` handler | explicit `lsh_shutdown` on every path, plus `SIGHUP`/`SIGTERM`/`SIGQUIT` handlers |
| 4 | high | `^C` ran the line you were cancelling, and doubled the prompt | the handler only wrote a prompt and left the buffer intact | handler sets `got_intr`; the loop discards the line and reprints one prompt |
| 5 | high | `^D` inserted as a literal control byte; `^Z` left the tty raw | no handling of either | `^D` on an empty line ends input; `^Z` restores the tty before stopping |
| 6 | high | children ran with no echo or line editing (`cat`, `python`, `vim`) | children inherited the shell's raw mode | child restores `orig_termios` before `execvp` |
| 7 | high | `waitpid` could spin or hang; a `WIFSTOPPED` child froze the shell | loop ignored `waitpid` errors and never resumed stopped children | `EINTR` retried, other errors reported, stopped child resumed via `sys_kill(SIGCONT)` |
| 8 | high | syscall errors looked like success; `errno` never set | wrappers ignored the carry flag and returned the kernel's positive errno | `_lsh_syscall_fail` translates to `(-1, errno)` |
| 9 | medium | `Home`/`End`/`Delete`/`F5` leaked bytes into the line; a bare `ESC` blocked the shell | fixed 2-byte blocking read after `ESC` | timeout-based decoder over `CSI`/`SS3`, unknown sequences consumed and ignored |
| 10 | medium | `sys_write(2, "…cd\"\n", 32)` sent 32 bytes for a 31-byte literal | hardcoded lengths | lengths computed from the strings |
| 11 | medium | memory leak when `realloc` failed | result assigned straight to the pointer, losing the old block | temporary pointer, old block freed |
| 12 | low | `help` output could be reordered when stdout was not a terminal | mixing raw `write(2)` with buffered `printf` | one output path for everything |
| 13 | low | 8 KB allocated per command | token array sized by `LSH_RL_BUFFSIZE` (1024) and grown by 64 | starts at `LSH_TOK_BUFSIZE` (64) |
| 14 | high | after **any** external command the shell stayed cooked: no redraw, arrows echoed as `^[[D` | child restored cooked mode but the parent's `raw_mode_on` still said raw, so re-enabling was skipped | `reapply_raw_mode` forces the settings back after `waitpid` |
| 15 | critical (new code) | my first `_lsh_syscall_fail` crashed with `EXC_BAD_ACCESS` at `0xffffffffffffffff` | it called `___error` with `bl` without saving `x30`, so `ret` jumped back into the helper and looped until `x0` was `-1` | proper frame: `stp x29, x30`, `mov x29, sp`, restored before `ret` |
| 16 | low (new code) | a `^C` or EOF landing inside an escape sequence was swallowed | the escape decoder mapped every negative result to `KEY_IGNORE` | `KEY_EOF`/`KEY_INTR`/`KEY_ERROR` are propagated; `got_intr` is also checked at the top of the read loop |
| 17 | medium (new code) | a truncated escape sequence killed the shell: feeding `ESC [ 1` and pausing exited it | the escape timeout used `-1` as its sentinel, which is also `KEY_EOF`, so "nothing arrived" was read as "end of input" on an empty line | timeouts return their own `KEY_TIMEOUT` (−4), and an incomplete `CSI … ~` sequence returns `KEY_IGNORE` |

Details worth keeping for the four that took real work:

* **The history overflow (#1).** ASAN named it exactly:
  `heap-buffer-overflow ... WRITE of size 1206 at ... lsh_read_line main.c:143`.
  The reproducer is simply: type a 1205-character command, press Enter, press Up.
  A 1206-byte write into a 1024-byte allocation, from a line that had never been
  long enough to grow the buffer.
* **The terminal not being restored (#3).** The `atexit` hook was registered and
  correct; it just never ran, because `sys_exit` is a raw syscall. Measured with a
  pty: `lflag=0x200005cb` before, `lflag=0x4c3` after `exit` — ECHO and ICANON
  still cleared.
* **The stop that never happened (#5/§7.2).** `raise(SIGTSTP)` inside its own
  handler does nothing unless the handler is installed with `SA_NODEFER`, and even
  then POSIX discards stop signals sent to an orphaned process group. Both cases
  are now handled explicitly, and the second one tells the user instead of failing
  silently.
* **The errno helper that crashed (#15).** The lesson is that arm64 `bl` clobbers
  `x30`: any function that calls another function must save it. It took a
  disassembly and a register dump to see that `ret` was branching back into `ldr w1,
  [sp]`, and that the crash address `-1` was `x0` after `mov x0, #-1` on the
  previous loop iteration.

## 16. Known limitations

Behavioural: no quoting, escaping, globbing, variables, expansion, pipes,
redirection, background jobs, job control, or tab completion; `^Z` suspends the
whole shell rather than a job; `history` is not searchable (`^R`) and `!!`/`!n` are
not implemented; UTF-8 is handled as bytes, so the cursor can sit inside a
multi-byte character; every keystroke repaints the whole line.

Structural: the history file grows unbounded (the read side is capped, so this only
costs startup time); a crash from a hard fault (e.g. `SIGSEGV`) cannot restore the
terminal, since nothing runs; `SIGTSTP` and `SIGCONT` handlers call `tcsetattr`,
which is not strictly async-signal-safe.

## 17. Adding another syscall

1. In `syscalls.S`, add a line next to the existing ones — the macro does the rest:
   `SYSCALL sys_unlink, SYS_unlink` (the number comes from `<sys/syscall.h>`, so it
   is worth checking there and, ideally, calling it once with a bad argument to
   confirm it fails with the errno you expect rather than a random value).
2. Declare the prototype in `syscalls.h` with the comment-free house style, and
   remember the documented convention: `-1` and `errno` on failure.
3. Call it from `main.c`. If it takes struct arguments (like `wait4`), the struct
   layout has to come from the system headers rather than being invented.
4. Rebuild, re-run the suite, and add a line to §5.3 and to the change log in §15.

## 18. Credits

Started from [Write a Shell in C](https://brennan.io/2015/01/16/write-a-shell-in-c/)
by Stephen Brennan, then extended as I learned more about how a shell — and
Darwin's syscall interface — actually works.
