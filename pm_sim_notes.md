# Multithreaded Process Manager — Complete Study Notes
### Everything you need to understand the code line by line and ace your viva

---

## Table of Contents

1. [What This Project Actually Is](#1-what-this-project-actually-is)
2. [The Big Picture — How All Pieces Fit](#2-the-big-picture--how-all-pieces-fit)
3. [C Fundamentals You Must Know](#3-c-fundamentals-you-must-know)
4. [Section by Section — Every Line Explained](#4-section-by-section--every-line-explained)
   - 4.1 [#includes and #defines](#41-includes-and-defines)
   - 4.2 [The ProcessState Enum](#42-the-processstate-enum)
   - 4.3 [The PCB Struct](#43-the-pcb-struct)
   - 4.4 [Global Variables](#44-global-variables)
   - 4.5 [Helper Functions](#45-helper-functions)
   - 4.6 [pm_fork](#46-pm_fork)
   - 4.7 [pm_exit](#47-pm_exit)
   - 4.8 [pm_kill](#48-pm_kill)
   - 4.9 [pm_wait](#49-pm_wait)
   - 4.10 [pm_ps](#410-pm_ps)
   - 4.11 [The Monitor Thread](#411-the-monitor-thread)
   - 4.12 [The Script Interpreter](#412-the-script-interpreter)
   - 4.13 [The Worker Thread](#413-the-worker-thread)
   - 4.14 [main()](#414-main)
5. [Synchronization — The Heart of the Project](#5-synchronization--the-heart-of-the-project)
6. [Process Lifecycle — State Transitions](#6-process-lifecycle--state-transitions)
7. [Tracing a Full Execution Example](#7-tracing-a-full-execution-example)
8. [Common Viva Questions and Answers](#8-common-viva-questions-and-answers)

---

## 1. What This Project Actually Is

Before diving into code, understand what you built at a high level.

An operating system keeps track of every running program using a data structure called a **Process Control Block (PCB)**. In a real OS, each PCB corresponds to an actual running process. In this project, you simulate that management layer in software — the processes are fake (just entries in an array), but the management logic is real.

**You built:**
- A shared table of 64 fake processes
- Multiple threads that concurrently create, terminate, and wait on those fake processes
- A synchronization system that prevents those threads from corrupting the shared table
- A monitor thread that logs every change to a file

**The key insight:** The project is not about processes. It is about **threads safely sharing data**. The processes are just the data being shared. The real learning is in the mutex and condition variable usage.

---

## 2. The Big Picture — How All Pieces Fit

When you run `./pm_sim thread0.txt thread1.txt`, this is what happens:

```
main() starts
  │
  ├── Creates init process (PID=1) in the process table
  │
  ├── Spawns monitor thread ──────────────────────────────────┐
  │     (sleeps until table changes, then prints)              │
  │                                                            │
  ├── Spawns worker thread 0 (reads thread0.txt)              │
  │     fork 1  → calls pm_fork(1)                            │
  │     sleep 200 → usleep(200000)                            │
  │     wait 1 -1 → calls pm_wait(1, -1)  ← may BLOCK here   │
  │                                                            │
  ├── Spawns worker thread 1 (reads thread1.txt)              │
  │     sleep 100 → usleep(100000)                            │
  │     fork 1  → calls pm_fork(1)                            │
  │     exit 2 10 → calls pm_exit(2, 10) ← WAKES worker 0    │
  │                                                            │
  └── Waits (pthread_join) for all workers to finish          │
        │                                                      │
        └── Sets simulation_done = true, wakes monitor ───────┘
              monitor prints final state and exits
```

Every time any worker calls pm_fork, pm_exit, pm_wait, or pm_kill, the table is modified and a snapshot is written to `snapshots.txt`. The monitor thread also wakes up and prints to the terminal.

---

## 3. C Fundamentals You Must Know

These are the C concepts used heavily. If you understand these, you understand the code.

### 3.1 Structs — Bundling Data Together

A `struct` is a way to group related variables under one name. Think of it as a row in a spreadsheet.

```c
typedef struct {
    int pid;
    int ppid;
} PCB;

PCB p;       // declare one PCB
p.pid = 5;   // access a field with dot notation
```

`typedef` just means you don't have to write `struct PCB` every time — you can just write `PCB`.

### 3.2 Arrays — A Row of the Same Type

```c
PCB process_table[64];    // 64 PCBs in a row in memory
process_table[0].pid = 1; // access the first one
```

### 3.3 Pointers — An Address to a Variable

```c
PCB *p = &process_table[5];  // p points TO the 6th entry
p->pid = 10;                 // arrow -> is dot notation through a pointer
```

`&` means "give me the address of". `*` means "go to this address". `->` means "access field through pointer".

### 3.4 Enums — Named Integer Constants

```c
typedef enum { RUNNING, BLOCKED, ZOMBIE, TERMINATED } ProcessState;
```

This just assigns: RUNNING=0, BLOCKED=1, ZOMBIE=2, TERMINATED=3. It makes the code readable — you write `state == ZOMBIE` instead of `state == 2`.

### 3.5 Boolean in C

C has no built-in `true`/`false`. The `#include <stdbool.h>` header gives you `bool`, `true`, and `false`. Without it you'd write `int active` and use 0 and 1.

### 3.6 Mutex — A Lock for Shared Data

When multiple threads access the same data, you need a lock to prevent them from running over each other.

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_lock(&lock);    // LOCK — only one thread can hold this at a time
// ... safely read/write shared data ...
pthread_mutex_unlock(&lock);  // UNLOCK — let next thread in
```

If Thread A holds the lock, Thread B calling `pthread_mutex_lock` will BLOCK (sleep) until Thread A unlocks. This is how you prevent race conditions.

### 3.7 Condition Variables — Sleep and Wake

A mutex prevents concurrent access. A condition variable lets a thread *sleep* until a specific condition becomes true.

```c
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

// THREAD A — wants to wait for something:
pthread_mutex_lock(&lock);
pthread_cond_wait(&cond, &lock);  // atomically: releases lock + sleeps
// ... woken up, lock re-acquired automatically ...
pthread_mutex_unlock(&lock);

// THREAD B — signals that the condition changed:
pthread_mutex_lock(&lock);
// ... make the change ...
pthread_cond_broadcast(&cond);    // wake ALL sleeping threads
pthread_mutex_unlock(&lock);
```

`pthread_cond_wait` does THREE things atomically:
1. Releases the mutex
2. Puts the thread to sleep
3. Re-acquires the mutex when it wakes up

This is critical — if it didn't release the mutex before sleeping, no other thread could ever acquire it, and nothing would ever change.

### 3.8 Thread Functions

Every function you pass to `pthread_create` must have this exact signature:

```c
void *my_function(void *arg) {
    // ...
    return NULL;
}
```

The `void *` return and parameter are C's way of saying "any type". You cast to/from your actual type inside.

---

## 4. Section by Section — Every Line Explained

### 4.1 #includes and #defines

```c
#include <stdio.h>
```
Gives you `printf`, `fprintf`, `fopen`, `fclose`, `fgets`, `fflush`. Anything related to printing or file I/O.

```c
#include <stdlib.h>
```
Gives you `malloc`, `free`, `exit`. Used for dynamic memory allocation in `main()`.

```c
#include <string.h>
```
Gives you `strcmp`, `strncmp`, `memcpy`, `snprintf`. Used in the script interpreter to compare command strings.

```c
#include <unistd.h>
```
Gives you `usleep`. Used to implement the `sleep` command in scripts. `usleep` takes *microseconds*, so `usleep(200 * 1000)` sleeps 200 milliseconds.

```c
#include <pthread.h>
```
The POSIX threads library. Gives you everything with the `pthread_` prefix: `pthread_create`, `pthread_join`, `pthread_mutex_lock`, `pthread_cond_wait`, etc. This is the core library for the whole project.

```c
#include <stdbool.h>
```
Gives you `bool`, `true`, `false`. Without this, you'd write `int active` and check `if (active == 1)`.

```c
#define MAX_PROCESSES 64
```
A preprocessor constant. Before compilation, every occurrence of `MAX_PROCESSES` in the code is literally replaced with `64`. It is NOT a variable — it has no memory address and cannot change at runtime. Using a define instead of the number `64` directly means if you ever need to change it, you change one line instead of hunting through the file.

---

### 4.2 The ProcessState Enum

```c
typedef enum {
    RUNNING,
    BLOCKED,
    ZOMBIE,
    TERMINATED
} ProcessState;
```

This defines the four legal states a process can be in. Under the hood, C assigns integers: RUNNING=0, BLOCKED=1, ZOMBIE=2, TERMINATED=3. But you never use those numbers — you always write the names.

```c
const char *state_to_str(ProcessState s) {
    switch (s) {
        case RUNNING:    return "RUNNING";
        case BLOCKED:    return "BLOCKED";
        case ZOMBIE:     return "ZOMBIE";
        case TERMINATED: return "TERMINATED";
        default:         return "UNKNOWN";
    }
}
```

This is a utility function. Given a `ProcessState` value, it returns the human-readable string version. Used when printing the process table. The `const char *` return type means "a pointer to a string that you should not modify".

**Why `switch` instead of `if-else`?** Both work. `switch` is idiomatic for enum values — it is cleaner and the compiler can warn you if you forget a case.

---

### 4.3 The PCB Struct

```c
typedef struct {
    int pid, ppid;
    ProcessState state;
    int exit_status;
    bool active;
    int children[MAX_PROCESSES];
    int child_count;
} PCB;
```

This is the most important data structure. Each entry in the process table is one of these.

- **`pid`** — Process ID. A unique integer assigned at creation. Starts at 1 for the init process, then 2, 3, 4... incremented by `next_pid++`.
- **`ppid`** — Parent Process ID. The PID of whoever created this process. The init process has PPID=0 (no parent).
- **`state`** — Current state from the enum above. Controls what operations are valid on this process.
- **`exit_status`** — The integer passed to `pm_exit`. Meaningless until the process is in ZOMBIE state.
- **`active`** — A boolean flag meaning "is this slot in the array currently in use?" This is needed because we never move entries in the array — a TERMINATED process just has `active = false`, freeing the slot for reuse.
- **`children[MAX_PROCESSES]`** — An array storing the PIDs of this process's children. Required by the spec.
- **`child_count`** — How many entries in `children[]` are valid. The children array can hold up to 64 entries; `child_count` tells you how many are actually used.

**Why `active` instead of checking `state != TERMINATED`?**
When we reap a process in `pm_wait`, we set both `state = TERMINATED` and `active = false`. The `active` flag is the authoritative "this slot is free" marker. It simplifies `find_free_slot()` and `find_by_pid()`.

---

### 4.4 Global Variables

```c
PCB process_table[MAX_PROCESSES];
int next_pid = 1;
```

`process_table` is the array of 64 PCBs. This is the shared data structure — every thread reads and writes it. `next_pid` is the counter for assigning new PIDs. Every `pm_fork` does `pid = next_pid++` to get the next available PID.

```c
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  table_cond  = PTHREAD_COND_INITIALIZER;
```

`table_mutex` is the single lock protecting the entire process table. Only one thread can hold it at a time. `PTHREAD_MUTEX_INITIALIZER` is a static initializer macro — it sets up the mutex without needing a separate `pthread_mutex_init()` call.

`table_cond` is a single condition variable used for two purposes:
1. Waking a parent blocked in `pm_wait` when its child exits
2. Waking the monitor thread when anything in the table changes

Both purposes use the same condition variable because both are triggered by the same event: a modification to the process table.

```c
bool simulation_done = false;
FILE *snap_file = NULL;
```

`simulation_done` is a flag set by `main()` after all worker threads finish. The monitor thread checks this to know when to exit. `snap_file` is the file handle for `snapshots.txt`, opened in `main()` and used by `write_snapshot()`.

**Why are these global?**
In C, the common pattern for shared state is global variables. The alternative is passing pointers everywhere. For a single-file project like this, globals are clean and appropriate.

---

### 4.5 Helper Functions

#### find_free_slot()

```c
int find_free_slot() {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!process_table[i].active) return i;
    return -1;
}
```

Scans the process table from index 0 to 63. Returns the first index where `active` is false. Returns -1 if the table is full (all 64 slots in use). Used by `pm_fork` before creating a new process.

**Important:** This function must only be called while holding `table_mutex`. It reads `active` fields, which could be modified by another thread simultaneously. The comments in the code say "call only while holding table_mutex" — this is a contract between the function and its callers.

#### find_by_pid()

```c
int find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (process_table[i].active && process_table[i].pid == pid)
            return i;
    return -1;
}
```

Scans the table for an active process with the given PID. Returns its array index, or -1 if not found. Used by almost every PM function to translate from "I want to operate on PID 5" to "PID 5 is at index 3 in the array".

Note it checks `active` first — TERMINATED (inactive) processes might still have their PID field set from when they were alive. We never want to accidentally operate on a dead slot.

#### print_table()

```c
void print_table(FILE *out) {
    fprintf(out, "PID\tPPID\tSTATE\tEXIT_STATUS\n");
    fprintf(out, "---------------------------------\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *p = &process_table[i];
        if (!p->active || p->state == TERMINATED) continue;

        fprintf(out, "%d\t%d\t%s\t", p->pid, p->ppid, state_to_str(p->state));

        if (p->state == ZOMBIE)
            fprintf(out, "%d\n", p->exit_status);
        else
            fprintf(out, "-\n");
    }
    fprintf(out, "\n");
}
```

Prints the process table to any file (`FILE *out`). The `out` parameter means you can call this with `stdout` for terminal output or `snap_file` for file output — same function, different destination.

- `fprintf(out, ...)` is like `printf` but writes to `out` instead of the terminal
- `PCB *p = &process_table[i]` — take the address of entry `i` and store it in pointer `p`. Now `p->pid` is cleaner than `process_table[i].pid` in each line
- `if (!p->active || p->state == TERMINATED) continue;` — skip empty slots and reaped processes. The spec says TERMINATED processes should not appear
- `\t` is a tab character — makes columns align nicely
- Exit status is only printed for ZOMBIE processes. For all others, we print `-`

**Must be called while holding table_mutex**, since it reads the entire table.

#### write_snapshot()

```c
void write_snapshot(const char *label) {
    fprintf(snap_file, "%s\n", label);
    print_table(snap_file);
    fflush(snap_file);
    pthread_cond_broadcast(&table_cond);
}
```

Called by every PM operation after it modifies the table. Writes a labeled snapshot to `snapshots.txt`.

- `fprintf(snap_file, "%s\n", label)` — write the label line, e.g. "Thread 0 calls pm_fork 1"
- `print_table(snap_file)` — write the current table state below it
- `fflush(snap_file)` — force the OS to actually write buffered data to disk immediately. Without this, output might sit in a buffer and not appear in the file until the program exits
- `pthread_cond_broadcast(&table_cond)` — wake ALL threads sleeping on `table_cond`. This wakes the monitor thread so it prints to the terminal

**This function must be called while holding table_mutex**, since it reads the table via `print_table`.

---

### 4.6 pm_fork

```c
int pm_fork(int parent_pid, int tid) {
    pthread_mutex_lock(&table_mutex);
```
Acquire the lock before touching the shared table. No other thread can enter any PM function until we release it.

```c
    int parent_idx = find_by_pid(parent_pid);
    if (parent_idx == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }
```
Find the parent process. If the parent doesn't exist (invalid PID), release the lock and return an error. **Critical:** unlock before every return path, or you deadlock the entire program.

```c
    int slot = find_free_slot();
    if (slot == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }
```
Find a free slot in the table. If all 64 slots are full, fail gracefully.

```c
    int pid = next_pid++;
```
Assign the next available PID. The `++` is post-increment — `pid` gets the current value of `next_pid`, then `next_pid` is incremented. Since this happens under the mutex, no two threads can get the same PID.

```c
    PCB *child = &process_table[slot];
    child->pid         = pid;
    child->ppid        = parent_pid;
    child->state       = RUNNING;
    child->exit_status = 0;
    child->active      = true;
    child->child_count = 0;
```
Fill in the new PCB. The child starts in RUNNING state. We zero out `child_count` because this is a fresh process with no children of its own yet.

```c
    PCB *parent = &process_table[parent_idx];
    parent->children[parent->child_count++] = pid;
```
Add the new child's PID to the parent's children list. `child_count++` post-increments: we store at index `child_count`, then increment it. So first child goes to index 0, second to index 1, etc.

```c
    char label[128];
    snprintf(label, sizeof(label), "Thread %d calls pm_fork %d", tid, parent_pid);
    write_snapshot(label);
```
Build the snapshot label string and write the snapshot. `snprintf` is like `sprintf` but safer — the second argument `sizeof(label)` prevents writing past the end of the buffer.

```c
    pthread_mutex_unlock(&table_mutex);
    return pid;
}
```
Release the lock and return the new child's PID. The snapshot was taken while we still held the lock, so it accurately reflects the table state immediately after the fork.

---

### 4.7 pm_exit

```c
void pm_exit(int pid, int status, int tid) {
    pthread_mutex_lock(&table_mutex);

    int idx = find_by_pid(pid);
    if (idx != -1 && process_table[idx].state != TERMINATED) {
        process_table[idx].state       = ZOMBIE;
        process_table[idx].exit_status = status;
```
Find the process and change its state to ZOMBIE. We check `state != TERMINATED` because calling exit on an already-dead process should do nothing. The process is not removed here — it becomes a ZOMBIE and waits for its parent to call `pm_wait` and collect it (reap it).

```c
        char label[128];
        snprintf(label, sizeof(label), "Thread %d calls pm_exit %d %d", tid, pid, status);
        write_snapshot(label);

        pthread_cond_broadcast(&table_cond);
```
Write the snapshot, then broadcast on the condition variable. **This broadcast is critical.** If the parent is currently sleeping inside `pm_wait` (blocked waiting for a child to exit), it will be woken up by this broadcast. Without this line, the parent sleeps forever — a deadlock.

The broadcast also wakes the monitor thread, which will then print the updated table to the terminal.

```c
    }
    pthread_mutex_unlock(&table_mutex);
}
```

---

### 4.8 pm_kill

```c
void pm_kill(int pid, int tid) {
    pthread_mutex_lock(&table_mutex);

    int idx = find_by_pid(pid);
    if (idx != -1 && process_table[idx].state != TERMINATED) {
        process_table[idx].state       = ZOMBIE;
        process_table[idx].exit_status = -1;

        char label[128];
        snprintf(label, sizeof(label), "Thread %d calls pm_kill %d", tid, pid);
        write_snapshot(label);

        pthread_cond_broadcast(&table_cond);
    }

    pthread_mutex_unlock(&table_mutex);
}
```

Structurally identical to `pm_exit`, with two differences:
1. The exit status is always `-1` (a conventional indicator of abnormal termination)
2. The snapshot label says "pm_kill" instead of "pm_exit"

**Why not just call `pm_exit` from inside `pm_kill`?**
Because `pm_exit` tries to acquire `table_mutex`, but `pm_kill` already holds it. Trying to lock a mutex you already hold is a **deadlock** — the thread blocks waiting for itself to release the lock, which can never happen. So `pm_kill` reimplements the logic directly.

---

### 4.9 pm_wait

This is the most complex function. Read it carefully.

```c
int pm_wait(int parent_pid, int child_pid, int tid) {
    pthread_mutex_lock(&table_mutex);

    while (true) {
```
The outer `while(true)` loop is the standard pattern for condition variable use. We loop because after waking from `pthread_cond_wait`, we must re-check the condition — the wakeup might be spurious (a false alarm that pthreads is allowed to generate), or another thread might have already reaped the zombie before we got to it.

```c
        bool has_children = false;
        int  zombie_idx   = -1;

        for (int i = 0; i < MAX_PROCESSES; i++) {
            PCB *p = &process_table[i];
            if (!p->active || p->ppid != parent_pid) continue;

            has_children = true;

            if ((child_pid == -1 || p->pid == child_pid) && p->state == ZOMBIE) {
                zombie_idx = i;
                break;
            }
        }
```
Scan the table looking for two things:
- **`has_children`**: does this parent have ANY active children? If not, there's nothing to wait for.
- **`zombie_idx`**: is there a child that is already ZOMBIE and can be reaped?

`child_pid == -1` means "any child" — this is the `wait 1 -1` case from the spec.

```c
        if (zombie_idx != -1) {
            int status = process_table[zombie_idx].exit_status;
            process_table[zombie_idx].state  = TERMINATED;
            process_table[zombie_idx].active = false;

            char label[128];
            snprintf(label, sizeof(label), "Thread %d calls pm_wait %d %d", tid, parent_pid, child_pid);
            write_snapshot(label);

            pthread_mutex_unlock(&table_mutex);
            return status;
        }
```
Found a zombie child. Reap it: mark as TERMINATED, set active=false (freeing the slot), take the snapshot, unlock, and return the exit status. This is the "happy path" — the child was already dead when we checked.

```c
        if (!has_children) {
            pthread_mutex_unlock(&table_mutex);
            return -1;
        }
```
No children at all. Return immediately — there is nothing to wait for.

```c
        int p_idx = find_by_pid(parent_pid);
        if (p_idx != -1) process_table[p_idx].state = BLOCKED;

        pthread_cond_wait(&table_cond, &table_mutex);

        if (p_idx != -1) process_table[p_idx].state = RUNNING;
    }
}
```
Children exist but none are zombies yet. We must sleep and wait.

1. Set parent's state to BLOCKED in the table (so `pm_ps` shows it correctly)
2. Call `pthread_cond_wait` — this atomically releases `table_mutex` and puts this thread to sleep
3. When we wake up (because `pm_exit` called `pthread_cond_broadcast`), `pthread_cond_wait` re-acquires the mutex before returning
4. Set parent back to RUNNING
5. Loop back to the top of `while(true)` and re-check — is there a zombie now?

**Why must this be a loop?** Because `pthread_cond_broadcast` wakes ALL sleeping threads. If two parents are waiting for the same child, both wake up, but only one gets to reap the zombie. The other must go back to sleep.

---

### 4.10 pm_ps

```c
void pm_ps(int tid) {
    pthread_mutex_lock(&table_mutex);
    printf("Thread %d calls pm_ps\n", tid);
    print_table(stdout);
    pthread_mutex_unlock(&table_mutex);
}
```

The simplest PM function. Lock, print to stdout (terminal), unlock. It does NOT write to snapshots.txt — it is a read-only operation and the spec only requires snapshots for modifications.

---

### 4.11 The Monitor Thread

```c
void *monitor_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&table_mutex);

    while (true) {
        pthread_cond_wait(&table_cond, &table_mutex);
        printf("[Monitor] Update:\n");
        print_table(stdout);
        if (simulation_done) break;
    }

    pthread_mutex_unlock(&table_mutex);
    return NULL;
}
```

The monitor thread's job: sleep until the table changes, print it, repeat.

- `(void)arg;` — suppresses the compiler warning "unused parameter". The function signature requires `void *arg` but we don't use it.
- The thread acquires `table_mutex` at the start and holds it for its entire life — but it gives it up every time it calls `pthread_cond_wait`.
- Each time `write_snapshot()` (inside a PM function) calls `pthread_cond_broadcast`, the monitor wakes up, prints the current table to stdout, then checks if the simulation is over.
- The check `if (simulation_done) break` comes **after** printing, not before. This ensures the monitor always prints the state that triggered the wakeup, even the final one.

**Why does the monitor hold the mutex while sleeping?**
`pthread_cond_wait` requires you to hold the mutex when you call it. It atomically releases the mutex while sleeping. So even though it "holds" the mutex, other threads can still operate — they just take turns.

---

### 4.12 The Script Interpreter

```c
void run_script(const char *file, int tid) {
    FILE *f = fopen(file, "r");
    if (!f) return;

    char line[256];
    int a, b;

    while (fgets(line, sizeof(line), f)) {
```

- `fopen(file, "r")` — open the file for reading. Returns NULL on failure.
- `fgets(line, sizeof(line), f)` — read one line at a time into `line`. Returns NULL when EOF is reached, ending the loop.
- `sizeof(line)` is 256 — this prevents buffer overflow. `fgets` will never write more than 256 characters.

```c
        if (sscanf(line, "fork %d", &a) == 1)
            pm_fork(a, tid);
```

`sscanf` is like `scanf` but reads from a string instead of stdin. It tries to match the format string `"fork %d"` against `line`. If it matches and successfully reads one integer into `a`, it returns 1. The `== 1` check verifies the parse succeeded.

For `fork 1`, this extracts `a = 1` (the parent PID) and calls `pm_fork(1, tid)`.

```c
        else if (sscanf(line, "exit %d %d", &a, &b) == 2)
            pm_exit(a, b, tid);

        else if (sscanf(line, "wait %d %d", &a, &b) == 2)
            pm_wait(a, b, tid);

        else if (sscanf(line, "kill %d", &a) == 1)
            pm_kill(a, tid);

        else if (sscanf(line, "sleep %d", &a) == 1)
            usleep(a * 1000);
```

Same pattern for each command. `sleep` is handled specially — it calls `usleep` (microseconds), so we multiply milliseconds by 1000. This sleep is just the worker thread waiting — it does NOT affect the process table.

```c
        else if (strncmp(line, "ps", 2) == 0)
            pm_ps(tid);
    }
    fclose(f);
}
```

`strncmp(line, "ps", 2)` checks if the first 2 characters of `line` are "ps". This is safer than `strcmp(line, "ps\n")` because it works regardless of whether the line ends in `\n`, `\r\n`, or nothing.

`fclose(f)` closes the file. Every `fopen` must have a matching `fclose`.

---

### 4.13 The Worker Thread

```c
typedef struct {
    int   tid;
    char *file;
} Args;

void *worker(void *arg) {
    Args *a = arg;
    run_script(a->file, a->tid);
    return NULL;
}
```

`pthread_create` can only pass one argument to a thread function: a `void *`. To pass multiple values (thread ID + filename), we bundle them in a struct.

`Args *a = arg;` is a cast — we tell C "treat this `void *` as a pointer to an `Args` struct". This is valid because that's exactly what `main()` passed in.

The thread just calls `run_script` with its assigned file and ID, then exits by returning NULL.

---

### 4.14 main()

```c
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s thread0.txt ...\n", argv[0]);
        return 1;
    }
```

`argc` = argument count (including the program name). `argv[0]` = program name, `argv[1]` = first script file, etc. If no script files are given, print usage to `stderr` and exit.

```c
    snap_file = fopen("snapshots.txt", "w");
    if (!snap_file) { perror("fopen"); return 1; }
```
Open `snapshots.txt` for writing. `"w"` mode creates the file if it doesn't exist, or truncates (empties) it if it does. `perror` prints a system error message if `fopen` fails.

```c
    for (int i = 0; i < MAX_PROCESSES; i++)
        process_table[i].active = false;
```
Initialize all slots as inactive. Global arrays in C are zero-initialized by default, but `bool active = false` is explicit and clear.

```c
    process_table[0] = (PCB){1, 0, RUNNING, 0, true, {0}, 0};
    next_pid = 2;
```
Create the init process manually at index 0. PID=1, PPID=0 (no parent), RUNNING, exit_status=0, active=true, empty children list. `next_pid = 2` so the next forked process gets PID 2.

The `(PCB){...}` syntax is a **compound literal** — it creates and initializes a struct inline.

```c
    pthread_mutex_lock(&table_mutex);
    write_snapshot("Initial Process Table");
    pthread_mutex_unlock(&table_mutex);
```
Take the initial snapshot before any threads start. We lock even here because `write_snapshot` reads the table (via `print_table`).

```c
    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_thread, NULL);
```
Start the monitor thread. `pthread_create` takes: pointer to thread ID, attributes (NULL = defaults), function to run, argument to pass (NULL here since monitor needs nothing).

```c
    int n = argc - 1;
    pthread_t threads[n];
    Args args[n];

    for (int i = 0; i < n; i++) {
        args[i] = (Args){i, argv[i + 1]};
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }
```
Create one worker thread per script file. `argc - 1` because `argv[0]` is the program name. Each worker gets its index as `tid` and its corresponding filename. We pass `&args[i]` — the address of that worker's args struct.

**Important:** `args` is declared before the loop, so it lives for the entire `main()` function. If we declared `Args a` inside the loop body, it would go out of scope before the thread could use it.

```c
    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);
```
Wait for all worker threads to finish. `pthread_join` blocks until the specified thread exits. Without this, `main()` would continue to the shutdown code before workers finish.

```c
    pthread_mutex_lock(&table_mutex);
    simulation_done = true;
    pthread_cond_broadcast(&table_cond);
    pthread_mutex_unlock(&table_mutex);

    pthread_join(monitor, NULL);
```
Signal the monitor to exit. We set `simulation_done = true` under the mutex, then broadcast to wake the monitor. The monitor will print one final time, see `simulation_done`, and break out of its loop. Then we join it to wait for it to fully exit.

```c
    fclose(snap_file);
    return 0;
}
```
Close the file and exit cleanly.

---

## 5. Synchronization — The Heart of the Project

### Why synchronization is needed

Imagine two worker threads both call `pm_fork(1)` at the exact same time (no mutex):

| Thread 0 | Thread 1 |
|----------|----------|
| `slot = find_free_slot()` → returns 1 | |
| | `slot = find_free_slot()` → also returns 1 (!) |
| `next_pid++` → pid = 2 | |
| fills slot 1 with PID=2 | |
| | `next_pid++` → pid = 2 again (!) |
| | overwrites slot 1 with PID=2 again |

Result: two processes with the same PID, one slot used twice, corrupted table. This is a **race condition**.

The mutex prevents this by ensuring only one thread runs inside any PM function at a time.

### The mutex pattern — always lock/unlock in pairs

```c
pthread_mutex_lock(&table_mutex);
// ... modify shared data ...
pthread_mutex_unlock(&table_mutex);
```

Every code path must unlock. If there are multiple return points in a function, every one of them must unlock first. A function that returns while holding the lock will deadlock everything that tries to acquire it afterward.

### The condition variable pattern

Used in `pm_wait` and `monitor_thread`. The pattern is always:

```c
pthread_mutex_lock(&mutex);
while (!condition_is_true) {
    pthread_cond_wait(&cond, &mutex);  // releases lock, sleeps, re-acquires
}
// condition is now true, do the work
pthread_mutex_unlock(&mutex);
```

And on the signaling side:

```c
pthread_mutex_lock(&mutex);
// make the condition true
pthread_cond_broadcast(&cond);  // wake sleeping threads
pthread_mutex_unlock(&mutex);
```

**Why `while` instead of `if`?**
After `pthread_cond_wait` returns, you do NOT know for certain that your condition is true. Possible reasons for waking up:
1. Another thread called broadcast for a different reason
2. The OS generated a spurious wakeup
3. Another thread already consumed the thing you were waiting for

The `while` loop handles all three cases by re-checking before proceeding.

### The two uses of table_cond

`table_cond` serves double duty:

**Use 1:** Wake parent in `pm_wait`
- `pm_exit` and `pm_kill` call `pthread_cond_broadcast` after setting a process to ZOMBIE
- Any parent sleeping in `pm_wait` wakes up and re-checks for zombie children

**Use 2:** Wake monitor thread
- `write_snapshot` (called inside every PM function) calls `pthread_cond_broadcast`
- The monitor thread wakes and prints the updated table

Both uses are triggered by the same event (table modification), so one condition variable handles both correctly.

---

## 6. Process Lifecycle — State Transitions

```
             pm_fork()
  ─────────────────────────> RUNNING
                                │
                    ┌───────────┴───────────┐
                    │                       │
            pm_wait() called        pm_exit() or pm_kill()
            (no zombie child)               │
                    │                       ▼
                    ▼                     ZOMBIE
                 BLOCKED                    │
                    │                       │
          child calls pm_exit()     parent calls pm_wait()
                    │               (reaps the zombie)
                    ▼                       │
                RUNNING ◄──────────────────┘
                                            │
                                            ▼
                                        TERMINATED
                                      (active = false)
                                    (not shown in pm_ps)
```

### Legal transitions:
- `RUNNING → BLOCKED`: parent calls `pm_wait` but no zombie child exists yet
- `BLOCKED → RUNNING`: child exits (pm_exit/pm_kill), parent is woken up
- `RUNNING → ZOMBIE`: process calls pm_exit, or another process calls pm_kill on it
- `ZOMBIE → TERMINATED`: parent calls pm_wait, finds the zombie, reaps it

### Illegal / impossible transitions:
- TERMINATED → anything: once terminated, a process is gone
- ZOMBIE → RUNNING: a zombie cannot be revived
- BLOCKED → ZOMBIE: a blocked process cannot exit directly (another thread would kill it, but in this simulation processes are just table entries, so it's technically possible via pm_kill — it would set the blocked process to ZOMBIE)

---

## 7. Tracing a Full Execution Example

Using the test files from the spec:

**thread0.txt:** `fork 1` / `sleep 200` / `fork 1` / `wait 1 -1` / `ps`  
**thread1.txt:** `sleep 100` / `fork 1` / `sleep 300` / `exit 2 10` / `ps`

Timeline (approximate, based on sleep values):

```
t=0ms:   Both workers start
         Worker 0: fork 1 → creates PID=2 (child of PID=1)
         Snapshot: "Thread 0 calls pm_fork 1" [PID 1 RUNNING, PID 2 RUNNING]

t=100ms: Worker 1: fork 1 → creates PID=3 (child of PID=1)
         Snapshot: "Thread 1 calls pm_fork 1" [PID 1, 2, 3 all RUNNING]

t=200ms: Worker 0: fork 1 → creates PID=4 (child of PID=1)
         Snapshot: "Thread 0 calls pm_fork 1" [PID 1,2,3,4 all RUNNING]
         Worker 0: wait 1 -1
           → checks table: PID 2,3,4 all RUNNING (none zombie)
           → sets PID 1 to BLOCKED
           → calls pthread_cond_wait → SLEEPS

t=400ms: Worker 1: exit 2 10
           → sets PID 2 to ZOMBIE with exit_status=10
           → Snapshot: "Thread 1 calls pm_exit 2 10"
           → pthread_cond_broadcast → WAKES Worker 0 !
         Worker 0 wakes from cond_wait:
           → sets PID 1 back to RUNNING
           → loops back, finds PID 2 is ZOMBIE
           → reaps it: PID 2 TERMINATED, active=false
           → Snapshot: "Thread 0 calls pm_wait 1 -1"
           → returns exit_status=10

t=400ms: Worker 1: ps → prints table to stdout
         Worker 0: ps → prints table to stdout
```

Final snapshots.txt:
```
Initial Process Table       ← PID 1 only
Thread 0 calls pm_fork 1    ← PID 1,2
Thread 1 calls pm_fork 1    ← PID 1,2,3
Thread 0 calls pm_fork 1    ← PID 1,2,3,4
Thread 1 calls pm_exit 2 10 ← PID 1 BLOCKED, PID 2 ZOMBIE
Thread 0 calls pm_wait 1 -1 ← PID 1 RUNNING (2 gone), PID 3,4
```

---

## 8. Common Viva Questions and Answers

**Q: What is a race condition and where would one occur in this project without synchronization?**

A: A race condition occurs when two threads access shared data concurrently and the result depends on which one runs first. In this project, without the mutex, two threads calling `pm_fork` simultaneously could read the same `next_pid` value and assign the same PID to two different processes, or both find the same free slot and overwrite each other's PCB.

---

**Q: What is a mutex and why do you need it?**

A: A mutex (mutual exclusion lock) ensures that only one thread can execute a critical section at a time. We use `table_mutex` to protect the process table. Any thread that wants to read or modify the table must acquire the lock first. If another thread holds it, the calling thread blocks until the lock is released. This prevents race conditions.

---

**Q: What is a condition variable and how does pm_wait use it?**

A: A condition variable allows a thread to sleep until a specific condition becomes true. In `pm_wait`, if a parent has no zombie children, it calls `pthread_cond_wait`, which atomically releases the mutex and puts the thread to sleep. When `pm_exit` sets a process to ZOMBIE, it calls `pthread_cond_broadcast`, which wakes all sleeping threads. The parent wakes up, re-acquires the mutex, and re-checks whether its child is now a zombie.

---

**Q: Why does pthread_cond_wait need to be inside a while loop?**

A: Because the wakeup does not guarantee the condition is true. A `pthread_cond_broadcast` wakes ALL sleeping threads, but only one can reap a given zombie. The others must re-check and go back to sleep. Also, the POSIX standard permits spurious wakeups — the thread can wake up even without a broadcast. The while loop handles both cases by always re-checking the condition.

---

**Q: What is a deadlock? How could one occur in this code?**

A: A deadlock is when two or more threads each hold a lock the other needs, and both wait forever. In this code, a deadlock would occur if a function tries to acquire `table_mutex` while already holding it. For example, if `pm_kill` called `pm_exit` (which also locks the mutex), `pm_kill` would lock, then `pm_exit` would try to lock again and block forever waiting for itself.

We prevent this by having `pm_kill` implement the termination logic directly rather than calling `pm_exit`.

---

**Q: Why is the zombie state needed? Why not just delete the process immediately on exit?**

A: Because the parent might call `pm_wait` to collect the exit status. If we deleted the process immediately, the parent would have no way to retrieve the exit code. The zombie state is a "waiting room" — the process is dead but its PCB entry persists until the parent acknowledges the death by calling `pm_wait`. Only then is the slot freed.

---

**Q: What does the monitor thread do and how does it avoid busy-waiting?**

A: The monitor thread logs process table updates to the terminal. It avoids busy-waiting (constantly checking in a loop) by sleeping on `table_cond` using `pthread_cond_wait`. It only wakes when another thread calls `pthread_cond_broadcast` inside `write_snapshot`, which happens every time a PM operation modifies the table. So the monitor does zero work between updates.

---

**Q: What happens if a parent exits without waiting for its children?**

A: In this simulation, the children remain in the table as active processes with their original PPID. They will never be reaped (no one will call `pm_wait` for them) and will stay in RUNNING or ZOMBIE state forever. In a real OS, these would become "orphan" processes adopted by the init process. Our simulator does not implement orphan adoption, but it is a valid extension.

---

**Q: Why does write_snapshot call pthread_cond_broadcast?**

A: For two reasons. First, to wake the monitor thread so it prints the updated table to the terminal. Second, any parent sleeping in `pm_wait` might also wake up here, but since `write_snapshot` is called from inside pm_exit and pm_kill (which have their own broadcast), this is actually redundant for the wait case. The broadcast in write_snapshot is primarily for the monitor.

---

**Q: What is snprintf and why use it instead of sprintf?**

A: Both format a string into a buffer. `snprintf` takes an additional size argument and will never write more characters than that limit, preventing a buffer overflow. `sprintf` has no limit and can corrupt memory if the formatted string is longer than the buffer. `snprintf` is always preferred in safe code.

---

**Q: Explain the void* pattern used in thread functions.**

A: `pthread_create` requires all thread functions to have the signature `void *fn(void *arg)`. This is because pthreads is a C library that must work with any function, regardless of what arguments it takes. `void *` is C's "any pointer" type. To pass multiple arguments, we pack them into a struct, pass a pointer to the struct as `void *`, and inside the thread function we cast back: `Args *a = (Args *)arg`. The cast is valid because we know that's what was passed.

---

*End of notes. Good luck with the viva — you know this code.*
