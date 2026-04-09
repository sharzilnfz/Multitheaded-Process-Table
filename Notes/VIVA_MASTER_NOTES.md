# Multithreaded Process Table Simulator — Complete Viva Master Notes

> **Goal**: Take you from zero understanding to confidently explaining every single line,
> every OS concept, and every design decision in this project.

---

## Table of Contents

1. [What This Project Actually Is](#1-what-this-project-actually-is)
2. [The OS Concepts You Must Know](#2-the-os-concepts-you-must-know)
3. [File-by-File Breakdown](#3-file-by-file-breakdown)
4. [Line-by-Line Code Walkthrough](#4-line-by-line-code-walkthrough)
5. [How the Pieces Connect — Full Execution Flow](#5-how-the-pieces-connect--full-execution-flow)
6. [Synchronization Deep Dive](#6-synchronization-deep-dive)
7. [Worked Example — Tracing test_complex.txt](#7-worked-example--tracing-test_complextxt)
8. [Worked Example — Two Threads (thread_a + thread_b)](#8-worked-example--two-threads)
9. [Common Viva Questions & Killer Answers](#9-common-viva-questions--killer-answers)
10. [Glossary](#10-glossary)

---

## 1. What This Project Actually Is

This is a **user-space simulation** of an operating system's process manager. In a real OS (Linux, Windows), the kernel maintains a **Process Table** — an array of **Process Control Blocks (PCBs)** that track every running program. Our simulator recreates this in a single C program.

**What it simulates:**
- `fork` — creating a child process (like Linux `fork()`)
- `exit` — a process finishing and becoming a zombie
- `kill` — forcefully terminating a process
- `wait` — a parent collecting a dead child's exit status (reaping a zombie)
- `ps` — printing the current process table

**The multithreaded part:** Multiple **pthreads** read commands from separate text files *simultaneously*, all modifying the *same shared process table*. This creates a classic **concurrent programming** scenario where we need **mutexes** and **condition variables** to prevent race conditions.

**Why this matters in OS:**
- Demonstrates process lifecycle (RUNNING → ZOMBIE → TERMINATED)
- Shows how a real OS kernel protects shared data structures
- Illustrates producer-consumer patterns (threads produce state changes, monitor consumes/displays them)

---

## 2. The OS Concepts You Must Know

### 2.1 Process vs Thread

| Aspect | Process | Thread |
|--------|---------|--------|
| Memory | Own address space | Shares parent's address space |
| Creation cost | Expensive (copy memory) | Cheap (share memory) |
| Communication | IPC (pipes, sockets) | Shared variables |
| Crash impact | Isolated | Crashes entire process |

In our project, we use **threads** (pthreads) to simulate multiple users/schedulers issuing process management commands concurrently. The "processes" in the table are *simulated data entries*, not real OS processes.

### 2.2 Process States (The Lifecycle)

```
                  fork()
                    │
                    ▼
              ┌──────────┐
              │  RUNNING  │◄─────────────────┐
              └────┬──────┘                  │
                   │                         │
            exit()/kill()              cond_wait returns
                   │                   (child became zombie)
                   ▼                         │
              ┌──────────┐            ┌──────────┐
              │  ZOMBIE   │            │ BLOCKED  │
              └────┬──────┘            └──────────┘
                   │                    parent calls wait()
            parent calls wait()         but no zombie child
                   │                    exists yet
                   ▼
            ┌─────────────┐
            │ TERMINATED  │
            └─────────────┘
            (slot freed, removed from table)
```

- **RUNNING**: Process is alive and executing
- **BLOCKED**: Parent is waiting for a child to die (only happens during `pm_wait`)
- **ZOMBIE**: Process has exited but parent hasn't collected its status yet
- **TERMINATED**: Parent collected the status; slot is now free for reuse

**Why zombies exist:** When a child exits, the OS can't just delete it — the parent might want to know *how* it died (exit status). So it stays as a zombie until the parent calls `wait()`.

### 2.3 Process Control Block (PCB)

A PCB is the OS's "identity card" for a process. Ours contains:

```
┌─────────────────────────────────┐
│           PCB Structure         │
├─────────────────────────────────┤
│ pid          → unique process ID│
│ ppid         → parent's PID     │
│ state        → RUNNING/BLOCKED/ │
│                ZOMBIE/TERMINATED│
│ exit_status  → return code      │
│ active       → is slot in use?  │
│ children[]   → array of child   │
│                PIDs             │
│ child_count  → number of kids   │
└─────────────────────────────────┘
```

### 2.4 Mutex (Mutual Exclusion)

A mutex is a **lock**. Only one thread can hold it at a time.

```
Thread A:  lock() ──── [critical section] ──── unlock()
Thread B:  lock() ... BLOCKED ... ... ... ... lock acquired ── [critical section] ── unlock()
```

Without a mutex, two threads could modify the process table simultaneously, causing corrupted data (a **race condition**).

### 2.5 Condition Variable

A condition variable lets a thread **sleep until something happens**, without burning CPU in a loop.

**Pattern:**
```c
pthread_mutex_lock(&mutex);
while (condition_not_met) {
    pthread_cond_wait(&cond, &mutex);  // atomically: unlock mutex + sleep
    // when woken: re-acquire mutex automatically
}
// do work
pthread_mutex_unlock(&mutex);
```

**Critical detail:** `pthread_cond_wait` **releases the mutex** while sleeping and **re-acquires it** when woken up. This is atomic — no other thread can sneak in between the release and sleep.

---

## 3. File-by-File Breakdown

### Project Structure
```
Multitheaded-Process-Table/
├── pm_sim.c              ← THE source code (everything is here)
├── pm_sim                ← compiled binary
├── snapshots.txt         ← output log of every state change
├── thread1.txt           ← script file for worker thread 0
├── thread2.txt           ← script file for worker thread 1
├── thread_a.txt          ← another test script
├── thread_b.txt          ← another test script
├── test_complex.txt      ← single-thread comprehensive test
├── thread/               ← subdirectory with duplicate scripts
│   ├── thread1.txt
│   ├── thread2.txt
│   └── snapshots.txt
└── .gitignore            ← tells git to ignore compiled files
```

### Script File Format

Each `.txt` file contains one command per line. Commands:

| Command | Format | What it does |
|---------|--------|-------------|
| fork | `fork <parent_pid>` | Create child under parent |
| exit | `exit <pid> <status>` | Process exits with status code |
| kill | `kill <pid>` | Force-kill a process |
| wait | `wait <parent_pid> <child_pid>` | Parent reaps child (-1 = any child) |
| sleep | `sleep <ms>` | Pause thread for milliseconds |
| ps | `ps` | Print process table to stdout |

---

## 4. Line-by-Line Code Walkthrough

### 4.1 Header Includes (Lines 1–6)

```c
#include <stdio.h>      // printf, fprintf, fopen, fclose, fgets, sscanf, snprintf
#include <stdlib.h>      // general utilities (not heavily used here but standard)
#include <string.h>      // strncmp — used for comparing "ps" command
#include <unistd.h>      // usleep() — microsecond sleep for simulating delays
#include <pthread.h>     // ALL threading: pthread_create, pthread_join, mutex, cond
#include <stdbool.h>     // bool, true, false — C99 boolean type
```

**Viva tip:** If asked "why pthread.h?" — it's the POSIX Threads library. POSIX is a standard that Unix/Linux/macOS follow. Windows uses a different threading API.

### 4.2 The MAX_PROCESSES Constant (Line 8)

```c
#define MAX_PROCESSES 64
```

This is a **compile-time constant** set by the preprocessor. It limits our process table to 64 entries. Before compilation, the preprocessor replaces every `MAX_PROCESSES` with `64`. This is a fixed-size array approach (simpler than dynamic allocation). A real OS would use dynamic structures, but for simulation, 64 is plenty.

### 4.3 Process State Enum (Lines 10–15)

```c
typedef enum {
    RUNNING,      // = 0 (C enums start at 0 by default)
    BLOCKED,      // = 1
    ZOMBIE,       // = 2
    TERMINATED    // = 3
} ProcessState;
```

`typedef` creates an alias so we can write `ProcessState` instead of `enum {...}`. Each name maps to an integer internally (RUNNING=0, BLOCKED=1, etc.), but using names makes code readable.

### 4.4 State-to-String Converter (Lines 17–25)

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

Converts the enum integer back to a human-readable string for printing. Returns a `const char *` (pointer to a string literal stored in read-only memory). The `default` case is defensive programming — should never be reached, but protects against future enum additions.

### 4.5 The PCB Structure (Lines 27–36)

```c
typedef struct {
    int pid, ppid;                  // process ID and parent process ID
    ProcessState state;             // current lifecycle state
    int exit_status;                // the code the process exited with
    bool active;                    // is this slot occupied?

    int children[MAX_PROCESSES];    // PIDs of all children this process created
    int child_count;                // how many children exist
} PCB;
```

**Why `active`?** The process table is a fixed-size array. When a process terminates, we don't shift elements — we just mark `active = false` so the slot can be reused by `find_free_slot()`.

**Why `children[]`?** To track parent-child relationships. When process 1 forks, the child's PID is added to process 1's `children` array. This mirrors how Linux tracks child processes in `task_struct`.

### 4.6 Global Variables (Lines 38–45)

```c
PCB process_table[MAX_PROCESSES];    // THE shared data structure — array of 64 PCBs
int next_pid = 1;                    // counter for assigning unique PIDs

pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;  // the lock
pthread_cond_t  table_cond  = PTHREAD_COND_INITIALIZER;   // the condition variable

bool simulation_done = false;        // flag to tell monitor thread to exit
FILE *snap_file = NULL;              // file pointer for snapshots.txt
```

**`PTHREAD_MUTEX_INITIALIZER`**: A macro that statically initializes a mutex. Alternative is `pthread_mutex_init()` at runtime. Static init is simpler for global mutexes.

**`PTHREAD_COND_INITIALIZER`**: Same idea for condition variables.

**Why global?** All threads need access to the same process table, mutex, and condition variable. Global scope is the simplest way to share these. In production code, you'd pass them via thread arguments.

### 4.7 Helper: find_free_slot() (Lines 49–53)

```c
int find_free_slot() {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!process_table[i].active) return i;  // found an empty slot
    return -1;                                     // table is full
}
```

Linear scan through all 64 slots. Returns the **index** (not PID) of the first inactive slot. Returns -1 if all slots are in use. This is O(n) but n=64, so it's fast enough.

### 4.8 Helper: find_by_pid() (Lines 55–60)

```c
int find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (process_table[i].active && process_table[i].pid == pid)
            return i;
    return -1;
}
```

Searches for a process by its PID. Checks both `active` (slot in use) AND `pid` matches. Returns the **index** into `process_table[]`, not the PID itself. This distinction is important — PID is the logical ID, index is the physical location.

### 4.9 Helper: print_table() (Lines 62–79)

```c
void print_table(FILE *out) {
    fprintf(out, "PID\tPPID\tSTATE\tEXIT_STATUS\n");
    fprintf(out, "---------------------------------\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *p = &process_table[i];                        // pointer to current PCB
        if (!p->active || p->state == TERMINATED) continue; // skip empty/dead slots

        fprintf(out, "%d\t%d\t%s\t",
                p->pid, p->ppid, state_to_str(p->state));

        if (p->state == ZOMBIE)
            fprintf(out, "%d\n", p->exit_status);   // show exit code for zombies
        else
            fprintf(out, "-\n");                     // running/blocked = no exit code yet
    }
    fprintf(out, "\n");
}
```

**`FILE *out`**: Accepts any output — `stdout` (screen) or a file. This makes the function reusable for both `pm_ps` (prints to screen) and `write_snapshot` (prints to file).

**`&process_table[i]`**: Takes the address of element `i`, storing it in pointer `p`. This avoids copying the entire struct and lets us use `p->field` syntax.

**Why skip TERMINATED?** Once a parent has reaped a child, there's no reason to show it. It's effectively deleted.

### 4.10 Helper: write_snapshot() (Lines 81–86)

```c
void write_snapshot(const char *label) {
    fprintf(snap_file, "%s\n", label);      // write a header like "Thread 0 calls pm_fork 1"
    print_table(snap_file);                 // dump current table state to file
    fflush(snap_file);                      // force write to disk immediately
    pthread_cond_broadcast(&table_cond);    // wake ALL waiting threads
}
```

**`fflush`**: Normally, file I/O is buffered — data sits in memory until the buffer is full. `fflush` forces it to disk immediately. Important because if the program crashes, we don't lose data.

**`pthread_cond_broadcast`**: Wakes up **ALL** threads waiting on `table_cond`. This notifies the monitor thread that something changed, and also wakes any parent blocked in `pm_wait`. Compare with `pthread_cond_signal` which wakes only ONE thread.

### 4.11 pm_fork() — Creating a Child Process (Lines 90–125)

```c
int pm_fork(int parent_pid, int tid) {
    pthread_mutex_lock(&table_mutex);           // LOCK — entering critical section

    int parent_idx = find_by_pid(parent_pid);   // find parent in table
    if (parent_idx == -1) {                     // parent doesn't exist?
        pthread_mutex_unlock(&table_mutex);     // UNLOCK before returning
        return -1;                              // error: invalid parent
    }

    int slot = find_free_slot();                // find empty slot for child
    if (slot == -1) {                           // table full?
        pthread_mutex_unlock(&table_mutex);     // UNLOCK before returning
        return -1;                              // error: no room
    }

    int pid = next_pid++;                       // assign next available PID

    PCB *child = &process_table[slot];          // pointer to the new slot
    child->pid         = pid;                   // set child's PID
    child->ppid        = parent_pid;            // record who the parent is
    child->state       = RUNNING;               // new processes start RUNNING
    child->exit_status = 0;                     // no exit status yet
    child->active      = true;                  // mark slot as occupied
    child->child_count = 0;                     // new process has no children

    PCB *parent = &process_table[parent_idx];
    parent->children[parent->child_count++] = pid;  // add child to parent's list

    char label[128];
    snprintf(label, sizeof(label),
             "Thread %d calls pm_fork %d", tid, parent_pid);
    write_snapshot(label);                      // log the state change

    pthread_mutex_unlock(&table_mutex);         // UNLOCK — leaving critical section
    return pid;                                 // return new child's PID
}
```

**Key pattern**: Lock → validate → modify → snapshot → unlock. Every function follows this pattern. The mutex is ALWAYS unlocked before returning, even on error paths. Forgetting to unlock = **deadlock** (all other threads wait forever).

**`next_pid++`**: Post-increment. Assigns current value to `pid`, THEN increments `next_pid`. So first fork gets PID 2 (since init is PID 1), next gets 3, etc. PIDs are never reused in our simulation.

**`snprintf`**: Like `sprintf` but safer — limits output to `sizeof(label)` bytes, preventing buffer overflow.

### 4.12 pm_exit() — Process Exits Voluntarily (Lines 127–145)

```c
void pm_exit(int pid, int status, int tid) {
    pthread_mutex_lock(&table_mutex);

    int idx = find_by_pid(pid);
    if (idx != -1 && process_table[idx].state != TERMINATED) {
        process_table[idx].state       = ZOMBIE;     // NOT terminated — becomes zombie
        process_table[idx].exit_status = status;     // store the exit code

        // ... snapshot logging ...

        pthread_cond_broadcast(&table_cond);         // FIX 1: wake blocked parents
    }

    pthread_mutex_unlock(&table_mutex);
}
```

**Why ZOMBIE and not TERMINATED?** This mirrors real Unix. When a process calls `exit()`, it doesn't vanish — it becomes a zombie. The parent must call `wait()` to read the exit status and truly remove it. This is essential for inter-process communication.

**The broadcast after exit**: If a parent is blocked in `pm_wait()` waiting for this child to die, we wake it up so it can reap the zombie.

### 4.13 pm_kill() — Force-Kill a Process (Lines 147–165)

```c
void pm_kill(int pid, int tid) {
    pthread_mutex_lock(&table_mutex);

    int idx = find_by_pid(pid);
    if (idx != -1 && process_table[idx].state != TERMINATED) {
        process_table[idx].state       = ZOMBIE;
        process_table[idx].exit_status = -1;         // -1 signals "killed, not normal exit"

        // ... snapshot logging ...

        pthread_cond_broadcast(&table_cond);
    }

    pthread_mutex_unlock(&table_mutex);
}
```

Almost identical to `pm_exit`, but `exit_status = -1` indicates abnormal termination. In real Linux, `kill` sends a **signal** (like SIGKILL=9). Our simulation simplifies this to immediate zombie state.

### 4.14 pm_wait() — Parent Reaps a Child (Lines 167–215) ⭐ MOST COMPLEX FUNCTION

```c
int pm_wait(int parent_pid, int child_pid, int tid) {
    pthread_mutex_lock(&table_mutex);

    while (true) {                              // loop until we find a zombie or give up
        bool has_children = false;
        int  zombie_idx   = -1;

        // SCAN: look through ALL processes for children of this parent
        for (int i = 0; i < MAX_PROCESSES; i++) {
            PCB *p = &process_table[i];
            if (!p->active || p->ppid != parent_pid) continue;  // not our child

            has_children = true;                // at least one child exists

            if ((child_pid == -1 || p->pid == child_pid) &&     // match specific or any
                p->state == ZOMBIE) {
                zombie_idx = i;                 // found a zombie child!
                break;
            }
        }

        // CASE 1: Found a zombie — reap it
        if (zombie_idx != -1) {
            int status = process_table[zombie_idx].exit_status;

            process_table[zombie_idx].state  = TERMINATED;   // mark as fully dead
            process_table[zombie_idx].active = false;        // free the slot

            // ... snapshot logging ...

            pthread_mutex_unlock(&table_mutex);
            return status;                      // return the child's exit code
        }

        // CASE 2: No children at all — nothing to wait for
        if (!has_children) {
            pthread_mutex_unlock(&table_mutex);
            return -1;                          // error: no children
        }

        // CASE 3: Children exist but none are zombies yet — BLOCK and wait
        int p_idx = find_by_pid(parent_pid);
        if (p_idx != -1) process_table[p_idx].state = BLOCKED;  // mark parent as blocked

        pthread_cond_wait(&table_cond, &table_mutex);  // SLEEP until broadcast

        if (p_idx != -1) process_table[p_idx].state = RUNNING;  // wake up → running again
    }
    // Loop back to re-scan for zombies
}
```

**This is the most important function for the viva.** It demonstrates:

1. **The wait-loop pattern**: Check condition → if not met → sleep → wake up → re-check. This handles **spurious wakeups** (a thread might be woken for no reason).

2. **`child_pid == -1` means "any child"**: Like Linux's `wait(-1)` or `waitpid(-1, ...)`. If `child_pid` is a specific PID, only that child is reaped.

3. **Blocking**: The parent transitions to BLOCKED state while waiting. In a real OS, blocked processes are taken off the CPU scheduler's ready queue.

4. **`pthread_cond_wait` magic**: It atomically (a) unlocks the mutex, (b) puts the thread to sleep. When woken, it atomically re-locks the mutex. This prevents the "lost wakeup" problem where a signal arrives between unlocking and sleeping.

### 4.15 pm_ps() — Print Table (Lines 217–222)

```c
void pm_ps(int tid) {
    pthread_mutex_lock(&table_mutex);
    printf("Thread %d calls pm_ps\n", tid);
    print_table(stdout);                    // stdout = screen
    pthread_mutex_unlock(&table_mutex);
}
```

Simple: lock, print, unlock. The lock ensures we get a **consistent snapshot** — no other thread can modify the table while we're reading it.

### 4.16 Monitor Thread (Lines 226–240)

```c
void *monitor_thread(void *arg) {
    (void)arg;                                       // suppress "unused parameter" warning
    pthread_mutex_lock(&table_mutex);

    while (true) {
        pthread_cond_wait(&table_cond, &table_mutex); // sleep until any change
        printf("[Monitor] Update:\n");
        print_table(stdout);                          // print current state
        if (simulation_done) break;                   // all workers finished? exit
    }

    pthread_mutex_unlock(&table_mutex);
    return NULL;
}
```

**This is an observer/listener pattern.** The monitor doesn't modify anything — it just watches for changes and prints them. It sleeps on the condition variable and wakes up whenever `write_snapshot` or `pm_exit`/`pm_kill` calls `broadcast`.

**`(void)arg`**: A C idiom to tell the compiler "I know I'm not using this parameter." Without it, you'd get a warning with `-Wall`.

**Why check `simulation_done` AFTER printing?** (Comment says "FIX 2") If we checked before printing, we'd miss the final state change. The flag is set by `main()` after all workers finish.

### 4.17 Script Parser: run_script() (Lines 242–272)

```c
void run_script(const char *file, int tid) {
    FILE *f = fopen(file, "r");           // open script file for reading
    if (!f) return;                        // silently skip if file doesn't exist

    char line[256];                        // buffer for one line (256 chars max)
    int a, b;                              // variables for parsed arguments

    while (fgets(line, sizeof(line), f)) { // read one line at a time

        if (sscanf(line, "fork %d", &a) == 1)            // try: "fork <pid>"
            pm_fork(a, tid);

        else if (sscanf(line, "exit %d %d", &a, &b) == 2) // try: "exit <pid> <status>"
            pm_exit(a, b, tid);

        else if (sscanf(line, "wait %d %d", &a, &b) == 2) // try: "wait <parent> <child>"
            pm_wait(a, b, tid);

        else if (sscanf(line, "kill %d", &a) == 1)        // try: "kill <pid>"
            pm_kill(a, tid);

        else if (sscanf(line, "sleep %d", &a) == 1)       // try: "sleep <ms>"
            usleep(a * 1000);              // usleep takes MICROSECONDS, so × 1000

        else if (strncmp(line, "ps", 2) == 0)             // try: "ps"
            pm_ps(tid);
    }

    fclose(f);
}
```

**`fgets`**: Reads one line including the newline character `\n`. Safe because it limits to `sizeof(line)` bytes.

**`sscanf`**: The opposite of `printf` — it **parses** a string. Returns the number of items successfully matched. So `sscanf(line, "fork %d", &a) == 1` means "this line matched the pattern `fork <number>`".

**`strncmp(line, "ps", 2)`**: Compares only the first 2 characters. This is FIX 3 — using `strcmp` would fail because `fgets` includes the trailing `\n`, so the line is actually `"ps\n"`, not `"ps"`.

**`usleep(a * 1000)`**: `usleep` takes microseconds. The script specifies milliseconds. 1 ms = 1000 μs.

### 4.18 Worker Thread Args & Function (Lines 274–283)

```c
typedef struct {
    int   tid;      // thread ID (0, 1, 2, ...)
    char *file;     // path to the script file
} Args;

void *worker(void *arg) {
    Args *a = arg;                    // cast generic void* to our Args*
    run_script(a->file, a->tid);     // execute the script
    return NULL;                      // thread exits
}
```

**`void *arg`**: All pthread functions require `void *` argument. We cast it back to `Args *` inside the function. This is C's way of achieving "generic" parameters.

### 4.19 main() — Orchestrator (Lines 287–331)

```c
int main(int argc, char *argv[]) {

    // ---- ARGUMENT CHECK ----
    if (argc < 2) {
        fprintf(stderr, "Usage: %s thread0.txt [thread1.txt ...]\n", argv[0]);
        return 1;                               // exit with error
    }

    // ---- OPEN SNAPSHOT FILE ----
    snap_file = fopen("snapshots.txt", "w");    // "w" = write mode (overwrites)
    if (!snap_file) { perror("fopen"); return 1; }

    // ---- INITIALIZE PROCESS TABLE ----
    for (int i = 0; i < MAX_PROCESSES; i++)
        process_table[i].active = false;         // all slots empty

    // ---- CREATE INIT PROCESS (PID 1) ----
    process_table[0] = (PCB){1, 0, RUNNING, 0, true, {0}, 0};
    next_pid = 2;                                // next fork gets PID 2

    // ---- WRITE INITIAL SNAPSHOT ----
    pthread_mutex_lock(&table_mutex);
    write_snapshot("Initial Process Table");
    pthread_mutex_unlock(&table_mutex);

    // ---- START MONITOR THREAD ----
    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_thread, NULL);

    // ---- START WORKER THREADS ----
    int n = argc - 1;                            // number of script files
    pthread_t threads[n];                        // VLA: variable-length array
    Args args[n];

    for (int i = 0; i < n; i++) {
        args[i] = (Args){i, argv[i + 1]};       // thread 0 gets argv[1], etc.
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    // ---- WAIT FOR ALL WORKERS TO FINISH ----
    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);          // blocks until thread i finishes

    // ---- SIGNAL MONITOR TO EXIT ----
    pthread_mutex_lock(&table_mutex);
    simulation_done = true;                      // set the flag
    pthread_cond_broadcast(&table_cond);         // wake monitor so it sees the flag
    pthread_mutex_unlock(&table_mutex);

    pthread_join(monitor, NULL);                 // wait for monitor to exit

    fclose(snap_file);                           // clean up
    return 0;
}
```

**`argc` and `argv`**: `argc` = argument count, `argv` = argument values. `argv[0]` is the program name, `argv[1]` onwards are the script files.

**`(PCB){1, 0, RUNNING, 0, true, {0}, 0}`**: A **compound literal** — creates a temporary PCB struct inline. Fields map to: pid=1, ppid=0 (no parent, it's init), state=RUNNING, exit_status=0, active=true, children={0}, child_count=0.

**Why PID 1?** In Unix, PID 1 is always `init` (or `systemd`) — the ancestor of all processes. Our simulation mirrors this.

**`pthread_create(&threads[i], NULL, worker, &args[i])`**:
- `&threads[i]` — where to store the thread handle
- `NULL` — default thread attributes
- `worker` — the function to run
- `&args[i]` — argument passed to worker

**`pthread_join`**: Blocks the calling thread until the target thread finishes. Like `wait()` for threads. Without join, `main()` could exit before workers finish.

---

## 5. How the Pieces Connect — Full Execution Flow

```
./pm_sim thread1.txt thread2.txt
         │
         ▼
    main() starts
         │
    ┌────┴────┐
    │ Init    │  Create PID 1 (init process)
    │ table   │  Write "Initial Process Table" snapshot
    └────┬────┘
         │
    ┌────┴──────────────────────────────┐
    │      pthread_create (×3)          │
    ├───────────┬───────────┬───────────┤
    │ Monitor   │ Worker 0  │ Worker 1  │
    │ thread    │ thread    │ thread    │
    │           │           │           │
    │ waits on  │ reads     │ reads     │
    │ cond_var  │ thread1   │ thread2   │
    │           │ .txt      │ .txt      │
    │           │           │           │
    │ ◄─woken───┤           │           │
    │  prints   │ pm_fork   │           │
    │  table    │           │           │
    │           │           │           │
    │ ◄─woken───┤───────────┤           │
    │  prints   │           │ pm_fork   │
    │  table    │           │           │
    │  ...      │  ...      │  ...      │
    │           │           │           │
    │           │ DONE      │ DONE      │
    │           ▼           ▼           │
    │      main: pthread_join ×2        │
    │           │                       │
    │ ◄─woken───┤ simulation_done=true  │
    │  breaks   │                       │
    │  loop     │                       │
    ▼           ▼                       │
  monitor     main: pthread_join(monitor)
  exits             │
                    ▼
               fclose, return 0
```

---

## 6. Synchronization Deep Dive

### Why We Need Synchronization

Imagine two threads running simultaneously without locks:

```
Thread 0: slot = find_free_slot()    → returns slot 1
Thread 1: slot = find_free_slot()    → ALSO returns slot 1 (hasn't been marked yet!)
Thread 0: process_table[1].pid = 2   → writes to slot 1
Thread 1: process_table[1].pid = 3   → OVERWRITES slot 1!
```

Result: Process 2 is lost. This is a **race condition**.

### How Our Mutex Prevents This

```
Thread 0: lock()
Thread 0: slot = find_free_slot()    → returns slot 1
Thread 0: process_table[1].active = true
Thread 0: unlock()
Thread 1: lock()                     ← was waiting, now proceeds
Thread 1: slot = find_free_slot()    → returns slot 2 (1 is taken now)
Thread 1: unlock()
```

### The Condition Variable Dance in pm_wait

```
Parent thread:                         Child thread:
    lock()
    scan for zombie → none found
    state = BLOCKED
    cond_wait() ─── releases lock ───→
                                          lock() ← acquires it
                                          state = ZOMBIE
                                          cond_broadcast() ─── wakes parent
                                          unlock()
    ←── re-acquires lock ────────────
    state = RUNNING
    scan for zombie → FOUND!
    reap it (TERMINATED)
    unlock()
    return exit_status
```

---

## 7. Worked Example — Tracing test_complex.txt

Script:
```
fork 1       →  PID 1 creates child PID 2
fork 1       →  PID 1 creates child PID 3
sleep 100    →  pause 100ms (let things settle)
exit 2 10    →  PID 2 exits with status 10 → ZOMBIE
exit 3 20    →  PID 3 exits with status 20 → ZOMBIE
wait 1 -1    →  PID 1 reaps ANY zombie child → finds PID 2 → TERMINATED, returns 10
wait 1 -1    →  PID 1 reaps ANY zombie child → finds PID 3 → TERMINATED, returns 20
ps           →  print table: only PID 1 remains RUNNING
```

**Process table after each step:**

| Step | PID 1 | PID 2 | PID 3 |
|------|-------|-------|-------|
| Initial | RUNNING | — | — |
| fork 1 (first) | RUNNING | RUNNING | — |
| fork 1 (second) | RUNNING | RUNNING | RUNNING |
| exit 2 10 | RUNNING | ZOMBIE(10) | RUNNING |
| exit 3 20 | RUNNING | ZOMBIE(10) | ZOMBIE(20) |
| wait 1 -1 | RUNNING | TERMINATED | ZOMBIE(20) |
| wait 1 -1 | RUNNING | — | TERMINATED |
| ps | RUNNING | — | — |

---

## 8. Worked Example — Two Threads (thread_a + thread_b)

This is the **concurrency** showcase. Two threads modify the table simultaneously.

**thread_a.txt:**
```
fork 1        → create PID 2 under PID 1
fork 1        → create PID 3 under PID 1
sleep 100     → wait 100ms
exit 2 5      → PID 2 becomes ZOMBIE
exit 3 10     → PID 3 becomes ZOMBIE
```

**thread_b.txt:**
```
wait 1 -1     → PID 1 waits for ANY child zombie
wait 1 -1     → PID 1 waits for ANY child zombie
ps            → print final table
```

**What happens concurrently:**

1. Thread A does `fork 1` → PID 2 created
2. Thread A does `fork 1` → PID 3 created
3. Thread B does `wait 1 -1` → no zombies yet → PID 1 goes BLOCKED → `cond_wait`
4. Thread A sleeps 100ms
5. Thread A does `exit 2 5` → PID 2 becomes ZOMBIE → `broadcast` → Thread B wakes up
6. Thread B re-scans → finds PID 2 ZOMBIE → reaps it → returns 5
7. Thread B does `wait 1 -1` → PID 3 still RUNNING → BLOCKED again → `cond_wait`
8. Thread A does `exit 3 10` → PID 3 ZOMBIE → `broadcast` → Thread B wakes
9. Thread B re-scans → finds PID 3 ZOMBIE → reaps it → returns 10
10. Thread B does `ps` → only PID 1 RUNNING

**The timing depends on the scheduler!** Steps 3 and 4 could swap, but the `cond_wait` mechanism guarantees correctness regardless of ordering.

---

## 9. Common Viva Questions & Killer Answers

### Q: What is a Process Control Block?
**A:** A PCB is a data structure the OS kernel maintains for every active process. It stores the process's PID, parent PID, current state, CPU register values, memory mappings, open file descriptors, and scheduling info. In our simulation, we model it as a C struct with pid, ppid, state, exit_status, active flag, and children array.

### Q: Why do we need a mutex here?
**A:** Multiple worker threads read and write the same global `process_table` array concurrently. Without a mutex, two threads could simultaneously find the same free slot and overwrite each other's data (a race condition). The mutex ensures only one thread modifies the table at a time, providing mutual exclusion.

### Q: What's the difference between `pthread_cond_signal` and `pthread_cond_broadcast`?
**A:** `signal` wakes exactly ONE waiting thread (chosen by the OS). `broadcast` wakes ALL waiting threads. We use `broadcast` because multiple threads might be waiting: the monitor thread AND a parent in `pm_wait`. Using `signal` could wake the wrong one.

### Q: Why does a process become ZOMBIE instead of TERMINATED on exit?
**A:** This mirrors real Unix behavior. When a process exits, the parent may need its exit status. If we immediately deleted it, the parent would lose that information. So the exited process stays as a ZOMBIE until the parent calls `wait()` to collect the status. Only then does it become TERMINATED and the slot is freed.

### Q: What happens if a parent never calls wait()?
**A:** The zombie stays in the table forever, wasting a slot. This is called a "zombie leak." In real Linux, if the parent exits without waiting, `init` (PID 1) "adopts" the orphaned zombies and reaps them. Our simulation doesn't implement orphan adoption.

### Q: Explain `pthread_cond_wait`. Why does it need the mutex?
**A:** `pthread_cond_wait` does three things atomically: (1) releases the mutex, (2) puts the thread to sleep, (3) when woken, re-acquires the mutex before returning. It needs the mutex to avoid the "lost wakeup" problem: without it, another thread could send a signal between when we check the condition and when we go to sleep, and we'd miss it.

### Q: What is a race condition?
**A:** A race condition occurs when the program's outcome depends on the unpredictable timing of thread execution. Two threads "race" to access/modify shared data, and the result depends on who gets there first. Example: two threads calling `find_free_slot()` simultaneously could get the same slot index, causing one process to overwrite the other.

### Q: What is a deadlock? Could it happen here?
**A:** A deadlock is when two or more threads are waiting for each other forever. For deadlock you need: (1) mutual exclusion, (2) hold and wait, (3) no preemption, (4) circular wait. In our code, there's only ONE mutex, so circular wait is impossible. However, if we forgot to unlock the mutex before returning (say on an error path), that thread would hold it forever and all others would wait — a simpler form of blocking.

### Q: Why do you use `strncmp` instead of `strcmp` for the "ps" command?
**A:** `fgets()` includes the trailing newline `\n` in the string. So the line is actually `"ps\n"`, not `"ps"`. `strcmp(line, "ps")` would return non-zero (not equal). `strncmp(line, "ps", 2)` only compares the first 2 characters, ignoring the newline.

### Q: How do you compile and run this project?
**A:** Compile: `gcc -o pm_sim pm_sim.c -lpthread` — the `-lpthread` flag links the POSIX threads library. Run: `./pm_sim thread1.txt thread2.txt` — pass one or more script files as arguments. Each file becomes a separate worker thread.

### Q: What's the purpose of the monitor thread?
**A:** The monitor thread acts as a real-time observer. It waits (sleeps) on the condition variable and wakes up whenever any thread makes a change to the process table. It then prints the current state to stdout. This demonstrates the observer/publish-subscribe pattern and shows condition variables being used for notifications.

### Q: What does `simulation_done` do and why is it needed?
**A:** `simulation_done` is a boolean flag checked by the monitor thread. After all worker threads finish (`pthread_join` returns), `main()` sets this flag to `true` and broadcasts. The monitor sees the flag, breaks its loop, and exits. Without it, the monitor would wait on `cond_wait` forever since no more broadcasts would come — the program would hang.

### Q: What is `usleep` and why multiply by 1000?
**A:** `usleep()` suspends the thread for a given number of **microseconds** (μs). Our script files specify delays in **milliseconds** (ms). Since 1 ms = 1000 μs, we multiply by 1000. This simulates real-world timing where processes take time to execute.

### Q: Can two threads create different processes at the same time?
**A:** Not truly "at the same time." The mutex serializes all table modifications. One thread locks the mutex, creates its process, and unlocks. Only then can the other thread lock and create its process. They appear concurrent from the script's perspective, but the actual table modifications happen one at a time.

### Q: What is `fprintf(stderr, ...)` vs `fprintf(stdout, ...)`?
**A:** `stderr` is the **standard error** stream, used for error messages. `stdout` is the **standard output** stream, used for normal output. They're separate so you can redirect output to a file while still seeing errors on screen: `./pm_sim script.txt > output.txt` captures stdout but not stderr.

---

## 10. Glossary

| Term | Definition |
|------|-----------|
| **PCB** | Process Control Block — data structure storing all info about a process |
| **PID** | Process Identifier — unique integer assigned to each process |
| **PPID** | Parent Process ID — PID of the process that created this one |
| **Mutex** | Mutual Exclusion lock — ensures only one thread accesses a resource at a time |
| **Condition Variable** | Synchronization primitive that lets threads wait for a condition to become true |
| **Race Condition** | Bug where outcome depends on unpredictable thread scheduling |
| **Deadlock** | Situation where threads wait for each other forever |
| **Zombie Process** | Process that has exited but hasn't been reaped by its parent |
| **Reaping** | Parent collecting a zombie's exit status via `wait()`, freeing its PCB slot |
| **Critical Section** | Code that accesses shared resources and must be mutex-protected |
| **Broadcast** | Waking ALL threads waiting on a condition variable |
| **VLA** | Variable-Length Array — array whose size is determined at runtime (C99) |
| **Compound Literal** | Creating a struct value inline: `(Type){field1, field2, ...}` |
| **POSIX** | Portable Operating System Interface — standard for Unix-like OS APIs |
| **pthread** | POSIX Thread — the threading API used on Unix/Linux/macOS |
| **Atomic Operation** | Operation that completes entirely without interruption |
| **`fflush`** | Forces buffered data to be written to the file immediately |
| **`sscanf`** | Parses a string according to a format, extracting values |
| **`snprintf`** | Safely formats a string into a buffer with size limit |

---

> **Final tip for the viva:** Don't just memorize — understand the *why*. If they ask about any function, explain it as: "it locks the mutex to protect shared data, does its work, logs a snapshot, then unlocks." Every function follows this pattern. The core insight is: **the process table is shared state, and the mutex + condition variable are how we keep it safe in a multithreaded environment.**
