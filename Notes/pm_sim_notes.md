# CSE 321 — Multithreaded Process Manager Simulator
## Complete Viva Notes: From Zero to Master

---

## Table of Contents

1. [The Big Picture — What Are We Building?](#1-the-big-picture)
2. [Operating System Concepts You Must Know](#2-os-concepts)
3. [Concurrency Primitives — Mutex & Semaphore](#3-concurrency-primitives)
4. [Header Files — Every Include Explained](#4-header-files)
5. [Global Constants and Data](#5-global-constants-and-data)
6. [The PCB Struct — Heart of the System](#6-the-pcb-struct)
7. [Process States and Transitions](#7-process-states-and-transitions)
8. [Helper Functions](#8-helper-functions)
9. [write_snapshot — The Logging Engine](#9-write_snapshot)
10. [pm_fork — Creating a Process](#10-pm_fork)
11. [pm_exit — Terminating a Process](#11-pm_exit)
12. [pm_kill — Force-Killing a Process](#12-pm_kill)
13. [pm_wait — Waiting for a Child](#13-pm_wait)
14. [pm_ps — Printing the Process Table](#14-pm_ps)
15. [monitor_thread — The Observer](#15-monitor_thread)
16. [run_script — The Script Interpreter](#16-run_script)
17. [worker — The Thread Entry Point](#17-worker)
18. [main — Putting It All Together](#18-main)
19. [Why No pthread_cond_t?](#19-why-no-pthread_cond_t)
20. [Concurrency Scenarios — Walk-Throughs](#20-concurrency-scenarios)
21. [Likely Viva Questions and Model Answers](#21-viva-questions)
22. [Common Bugs and Why We Avoided Them](#22-common-bugs)

---

## 1. The Big Picture

### What is this project?

In a real operating system (like Linux), when you run a program, the OS creates a **process** — a running instance of that program. The OS keeps track of every running process in a data structure called the **process table**. Each entry in the process table is a **Process Control Block (PCB)**.

This project **simulates** that entire mechanism in C. We are not creating real OS processes. We are:

- Maintaining a fake process table in memory (an array of PCBs)
- Using **POSIX threads (pthreads)** to simulate multiple things happening at the same time
- Using **mutexes and semaphores** to make sure threads don't corrupt the shared table
- Writing **snapshot logs** every time the table changes

Think of it this way:

```
Real OS          ←→      This Project
-----------               -----------------
Actual processes          PCB entries in an array
Kernel scheduler          Our mutex + semaphores
fork() system call        pm_fork()
exit() system call        pm_exit()
wait() system call        pm_wait()
kill() system call        pm_kill()
ps command                pm_ps()
```

### How does the program run?

```
./pm_sim thread0.txt thread1.txt thread2.txt
```

- The program reads N script files from command-line arguments.
- It creates N **worker threads**, one per file.
- Each worker reads its file line by line and calls the appropriate function.
- A separate **monitor thread** watches for any change to the process table and prints it.
- All of this happens **concurrently** — threads are truly running at the same time.

---

## 2. OS Concepts You Must Know

### What is a Process?

A process is a program in execution. It has:
- Its own memory space
- A unique ID (PID — Process IDentifier)
- A state (running, waiting, etc.)
- A parent (the process that created it)

### What is a PCB?

The Process Control Block is the data structure the OS uses to store all information about one process. Think of it as the "file" the OS keeps on every process.

### What is fork()?

In real Unix, `fork()` creates an exact copy of the calling process. The copy is called the **child**. The original is the **parent**. In our simulation, `pm_fork(parent_pid)` creates a new PCB entry whose parent is `parent_pid`.

### What is a Zombie Process?

When a process calls `exit()`, it dies — but its PCB is **not immediately deleted**. It stays in the table as a **zombie** until its parent calls `wait()` to collect the exit status. Only then is it fully removed (reaped).

Why? Because the parent might want to know *how* its child died (exit code 0 = success, non-zero = error). The zombie holds onto that exit code until the parent reads it.

### What is wait()?

`wait()` is called by a parent to:
1. Block until a child exits (if no child has exited yet)
2. Collect the child's exit status
3. Remove the zombie from the process table (reap it)

### What is the init process?

In Linux, PID 1 is the `init` process — it's the first process started by the kernel and is the ancestor of all other processes. In our simulation, we manually create PID 1 with PPID 0 (it has no parent) as the starting point.

---

## 3. Concurrency Primitives — Mutex & Semaphore

This is the most important section for your viva. Make sure you understand this deeply.

### Why do we need synchronization?

We have multiple threads all reading and writing the same `process_table` array at the same time. Without synchronization, this causes **race conditions** — two threads might try to assign the same PID, or one thread might read a half-written PCB entry.

### Mutex (Mutual Exclusion Lock)

A mutex is a lock. Only one thread can hold it at a time.

```c
pthread_mutex_lock(&table_mutex);
// --- CRITICAL SECTION ---
// Only ONE thread is here at a time.
// All others are blocked, waiting for the lock.
pthread_mutex_unlock(&table_mutex);
```

**Key rule:** Any time you access `process_table`, `next_pid`, or `simulation_done`, you must hold `table_mutex`.

Think of a mutex like a **bathroom key** — only the person holding the key can enter. Everyone else waits outside.

### Semaphore

A semaphore is a counter with two operations:
- `sem_post(s)` — increments the counter (signals that something is available)
- `sem_wait(s)` — decrements the counter, or **blocks if counter is 0** (waits until something is available)

A semaphore initialized to 0 is perfect for **signaling**: one thread sleeps with `sem_wait`, another wakes it up with `sem_post`.

```
Initial value: 0

Thread A calls sem_wait()  → counter is 0, so Thread A BLOCKS
Thread B calls sem_post()  → counter becomes 1, Thread A WAKES UP
Thread A resumes           → counter goes back to 0
```

**Semaphore vs Mutex:**

| Feature | Mutex | Semaphore |
|---|---|---|
| Values | Locked / Unlocked (binary) | Integer counter (0 to N) |
| Owner | Only the locker can unlock | Anyone can post |
| Use case | Protect a critical section | Signal between threads |
| Our usage | Protect process_table | Wake up blocked threads |

### We use TWO semaphores:

**`sem_monitor` (global)**
- Initialized to 0
- Posted every time `write_snapshot` is called (i.e., every table change)
- The monitor thread does `sem_wait(&sem_monitor)` — it sleeps until there's a snapshot to print

**`sem_wait` (per-PCB)**
- Each PCB has its own semaphore, initialized to 0
- When a parent calls `pm_wait` and no zombie child is found yet, the parent does `sem_wait` on **its own PCB's semaphore**
- When a child exits (`pm_exit` or `pm_kill`), it does `sem_post` on the **parent's semaphore** to wake it up

This is the key insight: each process has its own private "doorbell" semaphore. Only its children ring that doorbell.

---

## 4. Header Files

```c
#include <stdio.h>
```
Standard I/O: `printf`, `fprintf`, `fopen`, `fclose`, `fgets`, `fflush`.

```c
#include <stdlib.h>
```
Standard library: memory allocation, `exit()`. Not heavily used here but standard to include.

```c
#include <string.h>
```
String functions: `strncmp` (used to detect the `ps` command), `snprintf`.

```c
#include <unistd.h>
```
POSIX API: gives us `usleep()` — sleep for microseconds (used for the `sleep` script command).

```c
#include <pthread.h>
```
The POSIX threads library. Gives us:
- `pthread_t` — thread handle type
- `pthread_create` — create a thread
- `pthread_join` — wait for a thread to finish
- `pthread_mutex_t` — mutex type
- `pthread_mutex_lock` / `pthread_mutex_unlock`

```c
#include <semaphore.h>
```
POSIX semaphores. Gives us:
- `sem_t` — semaphore type
- `sem_init` — initialize a semaphore
- `sem_wait` — decrement (block if 0)
- `sem_post` — increment (wake a waiter)
- `sem_destroy` — clean up when done

```c
#include <stdbool.h>
```
Gives us the `bool` type and `true` / `false` constants. Without this, we'd have to use `int` with 0/1.

---

## 5. Global Constants and Data

```c
#define MAX_PROCESSES 64
```
The process table can hold at most 64 entries. This is a **compile-time constant** — it's replaced by the number 64 everywhere in code before compilation.

```c
PCB process_table[MAX_PROCESSES];
```
The **shared process table**. An array of 64 PCB structs sitting in global memory. Every thread reads and writes this. This is why we need a mutex.

```c
int next_pid = 1;
```
The next PID to assign. Every time a new process is created, we take this value and increment it. Protected by the mutex so two threads don't get the same PID.

```c
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
```
The one global mutex protecting all access to `process_table` and `next_pid`. `PTHREAD_MUTEX_INITIALIZER` is a macro that statically initializes the mutex — no need to call `pthread_mutex_init()` for global mutexes.

```c
sem_t sem_monitor;
```
The semaphore for the monitor thread. Declared globally so all functions can post to it via `write_snapshot`.

```c
bool simulation_done = false;
```
A flag set by `main` when all worker threads have finished. The monitor thread checks this to know when to exit. Protected by `table_mutex`.

```c
FILE *snap_file = NULL;
```
File pointer for `snapshots.txt`. Opened in `main`, written by `write_snapshot`.

---

## 6. The PCB Struct

```c
typedef struct {
    int pid;           // This process's unique ID
    int ppid;          // Parent's PID
    ProcessState state; // Current state (RUNNING, BLOCKED, ZOMBIE, TERMINATED)
    int exit_status;   // Exit code (only meaningful when ZOMBIE)
    bool active;       // Is this slot in use?

    int children[MAX_PROCESSES]; // Array of child PIDs
    int child_count;             // How many children

    sem_t sem_wait;    // Per-process semaphore for blocking in pm_wait
} PCB;
```

### Why `active` instead of just checking state?

When a PCB is `TERMINATED`, we set `active = false` to mark the slot as free for reuse. The `active` flag is the "is this slot occupied?" check. State alone wouldn't be enough because a slot could have garbage values from before it was ever used.

### Why store `children[]`?

We need to know which processes are children of a given parent — especially for `pm_wait(-1)` which waits for *any* child. The array stores PIDs of children.

### Why `sem_wait` inside PCB?

Each process gets its own semaphore specifically so `pm_wait` can block on it. This is a per-process resource:
- Initialized to 0 in `pm_fork` when the process is created
- Waited on in `pm_wait` when the parent has no zombie yet
- Posted in `pm_exit`/`pm_kill` when a child exits
- Destroyed in `pm_wait` when the zombie is reaped

---

## 7. Process States and Transitions

```c
typedef enum {
    RUNNING,     // Process exists and is active
    BLOCKED,     // Process is waiting for a child
    ZOMBIE,      // Process has exited, not yet reaped
    TERMINATED   // Process has been reaped (slot is freed)
} ProcessState;
```

### State Transition Diagram

```
                   pm_fork()
                       │
                       ▼
                   RUNNING  ◄────────────────────────────┐
                  /        \                             │
      pm_wait()  /          \ pm_exit() / pm_kill()     │
    (no zombie) /            \                           │
               ▼              ▼                          │
           BLOCKED          ZOMBIE                       │
               │               │                         │
               │ child exits   │ parent calls pm_wait()  │
               └───────────────┘                         │
                       │                                 │
                       ▼                                 │
                  TERMINATED ─── slot freed, can be ─────┘
                               reused by new pm_fork()
```

### Key Rule: TERMINATED processes are invisible

`print_table` skips any PCB where `state == TERMINATED` or `active == false`. Once reaped, a process completely disappears from all output.

---

## 8. Helper Functions

### `find_free_slot()`

```c
int find_free_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!process_table[i].active) return i;
    return -1;
}
```

Scans the process table array from index 0 to 63 looking for any slot where `active == false`. Returns the index (not the PID!) of the first free slot, or -1 if the table is full.

Note: this returns an **array index**, not a PID. The PID is separately assigned from `next_pid`.

**Must be called while holding `table_mutex`** — otherwise two threads might find the same free slot simultaneously.

### `find_by_pid()`

```c
int find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (process_table[i].active && process_table[i].pid == pid)
            return i;
    return -1;
}
```

Scans the table looking for a PCB with a matching `pid` that is also `active`. Returns the array index or -1.

Why check `active`? Because a slot might have a leftover `pid` value from a previously terminated process. We only want currently active ones.

**Must be called while holding `table_mutex`.**

### `print_table()`

```c
void print_table(FILE *out) {
    fprintf(out, "PID\tPPID\tSTATE\tEXIT_STATUS\n");
    fprintf(out, "---------------------------------\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *p = &process_table[i];
        if (!p->active || p->state == TERMINATED) continue;

        fprintf(out, "%d\t%d\t%s\t",
                p->pid, p->ppid, state_to_str(p->state));

        if (p->state == ZOMBIE)
            fprintf(out, "%d\n", p->exit_status);
        else
            fprintf(out, "-\n");
    }
    fprintf(out, "\n");
}
```

Prints the process table to any file (`stdout` for console, `snap_file` for snapshots.txt).

Line by line:
- Print the header row
- Print the separator line
- For each slot: skip if not active or if TERMINATED
- Print PID, PPID, state as a string
- For EXIT_STATUS: only print the number if the process is a ZOMBIE, otherwise print a hyphen `-`
- Print a trailing newline for visual separation

**Must be called while holding `table_mutex`.**

### `state_to_str()`

```c
const char *state_to_str(ProcessState s) {
    switch (s) {
        case RUNNING:    return "RUNNING";
        ...
    }
}
```

Converts the enum value to a human-readable string for printing. Straightforward.

---

## 9. write_snapshot

```c
void write_snapshot(const char *label) {
    fprintf(snap_file, "%s\n", label);
    print_table(snap_file);
    fflush(snap_file);
    sem_post(&sem_monitor);
}
```

This function is the central logging mechanism. It is **always called while `table_mutex` is already held** — that's a critical design rule.

Line by line:
- `fprintf(snap_file, "%s\n", label)` — write the event label (e.g., "Thread 0 calls pm_fork 1") to snapshots.txt
- `print_table(snap_file)` — write the current state of the entire process table below the label
- `fflush(snap_file)` — force the OS to flush the buffer to disk immediately (so the file is always up to date even if the program crashes)
- `sem_post(&sem_monitor)` — increment the monitor semaphore by 1, waking up the monitor thread

**Why call this inside the mutex?**

Because we're reading `process_table` to print it. If we called this after releasing the mutex, another thread could modify the table between the unlock and the print, giving us an inconsistent snapshot.

---

## 10. pm_fork

```c
int pm_fork(int parent_pid, int tid) {
    pthread_mutex_lock(&table_mutex);
```
First thing: acquire the lock. No other thread can touch the table while we work.

```c
    int parent_idx = find_by_pid(parent_pid);
    if (parent_idx == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }
```
Find the parent process. If it doesn't exist (invalid PID), release the lock and return error. We **must** unlock before returning — otherwise we leave the mutex locked forever (deadlock).

```c
    int slot = find_free_slot();
    if (slot == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }
```
Find a free slot in the table. If the table is full (64 processes), fail safely.

```c
    int pid = next_pid++;
```
Assign the next available PID. `next_pid++` reads the current value (assigns to `pid`) then increments `next_pid`. Because we hold the mutex, no other thread can do this simultaneously — so PIDs are unique.

```c
    PCB *child = &process_table[slot];
    child->pid         = pid;
    child->ppid        = parent_pid;
    child->state       = RUNNING;
    child->exit_status = 0;
    child->active      = true;
    child->child_count = 0;
    sem_init(&child->sem_wait, 0, 0);
```
Fill in the new PCB:
- `pid` = newly assigned PID
- `ppid` = the parent's PID
- `state` = RUNNING (all newly created processes start running)
- `exit_status` = 0 (not meaningful yet, just initialized)
- `active` = true (slot is now occupied)
- `child_count` = 0 (no children yet)
- `sem_init(&child->sem_wait, 0, 0)` = initialize the per-process semaphore to 0 (the `0, 0` means: not shared across processes, initial value 0)

```c
    PCB *parent = &process_table[parent_idx];
    parent->children[parent->child_count++] = pid;
```
Register the new child with the parent. Add the new PID to the parent's `children[]` array and increment `child_count`.

```c
    char label[128];
    snprintf(label, sizeof(label),
             "Thread %d calls pm_fork %d", tid, parent_pid);
    write_snapshot(label);
```
Build the snapshot label string and log the current state of the table.

```c
    pthread_mutex_unlock(&table_mutex);
    return pid;
}
```
Release the lock and return the new child's PID.

---

## 11. pm_exit

```c
void pm_exit(int pid, int status, int tid) {
    pthread_mutex_lock(&table_mutex);

    int idx = find_by_pid(pid);
    if (idx != -1 && process_table[idx].state != TERMINATED) {
```
Find the process. The guard `state != TERMINATED` prevents double-exiting a process that was already reaped.

```c
        process_table[idx].state       = ZOMBIE;
        process_table[idx].exit_status = status;
```
Change state to ZOMBIE and save the exit code. The process is "dead" but its PCB remains so the parent can read the exit code.

```c
        char label[128];
        snprintf(label, sizeof(label),
                 "Thread %d calls pm_exit %d %d", tid, pid, status);
        write_snapshot(label);
```
Log the snapshot. At this moment, if the parent is BLOCKED (waiting), that will be visible in the snapshot because we haven't woken it up yet.

```c
        notify_parent(process_table[idx].ppid);
    }

    pthread_mutex_unlock(&table_mutex);
}
```
Call `notify_parent` to wake up the parent (if it's blocked), then release the lock.

### notify_parent (helper)

```c
static void notify_parent(int ppid) {
    int p_idx = find_by_pid(ppid);
    if (p_idx != -1)
        sem_post(&process_table[p_idx].sem_wait);
}
```

Find the parent by PID and `sem_post` on its personal semaphore. If the parent was blocked in `sem_wait(&process_table[p_idx].sem_wait)`, it will now wake up.

The `static` keyword means this function is only visible within this file — good practice for internal helpers.

---

## 12. pm_kill

```c
void pm_kill(int pid, int tid) {
    pthread_mutex_lock(&table_mutex);

    int idx = find_by_pid(pid);
    if (idx != -1 && process_table[idx].state != TERMINATED) {
        process_table[idx].state       = ZOMBIE;
        process_table[idx].exit_status = -1;    // -1 = killed, not natural exit
        ...
        notify_parent(process_table[idx].ppid);
    }

    pthread_mutex_unlock(&table_mutex);
}
```

Almost identical to `pm_exit`, except:
- We don't receive a status from the process — it's being killed externally
- We use `-1` as the exit status to indicate it was killed (not a natural exit)

From the parent's perspective, a killed child behaves exactly like one that exited — it becomes a ZOMBIE and the parent can reap it with `pm_wait`.

---

## 13. pm_wait

This is the most complex function. Read carefully.

```c
int pm_wait(int parent_pid, int child_pid, int tid) {
    pthread_mutex_lock(&table_mutex);

    while (true) {
```
We use a `while(true)` loop because after waking up from a `sem_wait`, we need to **check again** — the zombie we woke up for might have already been reaped by another thread.

```c
        bool has_children = false;
        int  zombie_idx   = -1;

        for (int i = 0; i < MAX_PROCESSES; i++) {
            PCB *p = &process_table[i];
            if (!p->active || p->ppid != parent_pid) continue;

            has_children = true;

            if ((child_pid == -1 || p->pid == child_pid) &&
                p->state == ZOMBIE) {
                zombie_idx = i;
                break;
            }
        }
```
Scan the table for:
1. Any active process whose `ppid == parent_pid` — if found, `has_children = true`
2. Among those children, find one that is a ZOMBIE AND matches the requested `child_pid` (or any child if `child_pid == -1`)

```c
        if (zombie_idx != -1) {
            int status = process_table[zombie_idx].exit_status;
            sem_destroy(&process_table[zombie_idx].sem_wait);
            process_table[zombie_idx].state  = TERMINATED;
            process_table[zombie_idx].active = false;

            char label[128];
            snprintf(label, sizeof(label),
                     "Thread %d calls pm_wait %d %d",
                     tid, parent_pid, child_pid);
            write_snapshot(label);

            pthread_mutex_unlock(&table_mutex);
            return status;
        }
```
**Zombie found!** Reap it:
- Save the exit status (we'll return it)
- `sem_destroy` — clean up the zombie's semaphore (it no longer needs it)
- Mark as TERMINATED and `active = false` (frees the slot)
- Write snapshot showing the reaped state
- Unlock and return the exit status

```c
        if (!has_children) {
            pthread_mutex_unlock(&table_mutex);
            return -1;
        }
```
If there are no children at all, there's nothing to wait for. Return -1 immediately.

```c
        int p_idx = find_by_pid(parent_pid);
        if (p_idx != -1) {
            process_table[p_idx].state = BLOCKED;

            char blabel[128];
            snprintf(blabel, sizeof(blabel),
                     "Thread %d calls pm_wait %d %d (blocking)",
                     tid, parent_pid, child_pid);
            write_snapshot(blabel);
        }
```
No zombie found yet. Mark the parent as BLOCKED and **write a snapshot right now** (while still holding the mutex). This is critical — it means the snapshot file records the parent as BLOCKED before we actually sleep. When `pm_exit` runs next, its snapshot will also show the parent as BLOCKED.

```c
        pthread_mutex_unlock(&table_mutex);

        if (p_idx != -1)
            sem_wait(&process_table[p_idx].sem_wait);

        pthread_mutex_lock(&table_mutex);

        if (p_idx != -1)
            process_table[p_idx].state = RUNNING;
    }
}
```
**The critical sequence:**
1. Release the mutex — we MUST do this before `sem_wait`, otherwise no other thread can call `pm_exit` to wake us up (deadlock!)
2. `sem_wait` on our own semaphore — we sleep here until someone does `sem_post`
3. When we wake up, re-acquire the mutex
4. Set state back to RUNNING
5. Loop back to the top of `while(true)` to re-check for zombies

**Why release the mutex before sem_wait?** If we kept the mutex locked and called `sem_wait`, we'd be sleeping while holding the lock. Nobody else could enter any function that needs the lock — including `pm_exit`. That's a **deadlock**: the parent is waiting for the child to exit, but the child can't exit because the parent holds the lock.

---

## 14. pm_ps

```c
void pm_ps(int tid) {
    pthread_mutex_lock(&table_mutex);
    printf("Thread %d calls pm_ps\n", tid);
    print_table(stdout);
    pthread_mutex_unlock(&table_mutex);
}
```

Simple: lock, print the table to stdout (console), unlock. No snapshot written here — `ps` just shows the current state to the console, it doesn't log to the file.

---

## 15. monitor_thread

```c
void *monitor_thread(void *arg) {
    (void)arg;
```
`(void)arg` suppresses the compiler warning for unused parameter. Monitor doesn't need any arguments.

```c
    while (true) {
        sem_wait(&sem_monitor);
```
Sleep here. This thread does nothing until `write_snapshot` calls `sem_post(&sem_monitor)`. Every time a snapshot is written anywhere in the code, the monitor wakes up.

```c
        pthread_mutex_lock(&table_mutex);
        bool done = simulation_done;
        printf("[Monitor] Update:\n");
        print_table(stdout);
        pthread_mutex_unlock(&table_mutex);
```
Wake up, acquire the lock to safely read the table:
- Read `simulation_done` flag (must read under mutex to avoid race)
- Print "[Monitor] Update:" to console
- Print the current table state
- Release the lock

```c
        if (done) break;
    }

    return NULL;
}
```
If `simulation_done` was true when we woke up, break out of the loop and exit. The thread returns `NULL` (standard for pthreads).

**Why check `done` AFTER printing?**

If we checked before printing, we'd exit without printing the final state. By printing first, then checking, we ensure the very last snapshot is always shown.

**Why is `simulation_done` read under the mutex?**

Because `main` writes to it: `simulation_done = true`. If we read it without the mutex, there's a race condition — the CPU might see a stale cached value of `false` even after `main` set it to `true`. The mutex enforces memory visibility.

---

## 16. run_script

```c
void run_script(const char *file, int tid) {
    FILE *f = fopen(file, "r");
    if (!f) {
        perror("fopen (script file)");
        return;
    }
```
Open the script file for reading. `perror` prints the system error message if it fails (e.g., "No such file or directory").

```c
    char line[256];
    int a, b;

    while (fgets(line, sizeof(line), f)) {
```
Read the file line by line. `fgets` reads one line at a time (up to 255 characters + null terminator). Returns NULL at end of file, ending the loop.

```c
        if (sscanf(line, "fork %d", &a) == 1)
            pm_fork(a, tid);
```
`sscanf` tries to parse the line as `"fork %d"` (the word "fork" followed by an integer). If it successfully parses 1 integer, it calls `pm_fork`.

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
`sleep N` in the script means sleep N **milliseconds**. `usleep` takes **microseconds**. So we multiply by 1000: `N ms × 1000 = N×1000 µs`.

```c
        else if (strncmp(line, "ps", 2) == 0)
            pm_ps(tid);
    }

    fclose(f);
}
```
`strncmp(line, "ps", 2)` compares only the first 2 characters of `line` with `"ps"`. This handles the fact that `fgets` includes the newline character `\n` at the end of the line — a direct `strcmp(line, "ps")` would fail because the line is actually `"ps\n"`.

---

## 17. worker

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

`pthread_create` can only pass a single `void *` to the thread function. To pass multiple arguments (thread ID + filename), we pack them into an `Args` struct, pass a pointer to it, and cast it back inside the thread.

The worker simply calls `run_script` with its file and thread ID, then exits.

---

## 18. main

```c
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s thread0.txt [thread1.txt ...]\n", argv[0]);
        return 1;
    }
```
`argc` = number of arguments. `argv[0]` = program name. `argv[1]` onwards = script filenames. We need at least 1 script file.

```c
    snap_file = fopen("snapshots.txt", "w");
    if (!snap_file) { perror("fopen"); return 1; }
```
Open `snapshots.txt` for writing. The `"w"` mode creates it if it doesn't exist, or truncates it if it does.

```c
    sem_init(&sem_monitor, 0, 0);
```
Initialize the monitor semaphore: `0` = not shared between processes (just threads), `0` = initial value.

```c
    for (int i = 0; i < MAX_PROCESSES; i++)
        process_table[i].active = false;
```
Mark all 64 slots as empty. Without this, they contain garbage values from uninitialized memory.

```c
    process_table[0] = (PCB){1, 0, RUNNING, 0, true, {0}, 0};
    sem_init(&process_table[0].sem_wait, 0, 0);
    next_pid = 2;
```
Create the init process (PID 1, PPID 0) manually. The `(PCB){...}` syntax is a **compound literal** — it initializes all fields at once. Then initialize its semaphore and set `next_pid` to 2 (since 1 is taken).

```c
    pthread_mutex_lock(&table_mutex);
    write_snapshot("Initial Process Table");
    pthread_mutex_unlock(&table_mutex);
```
Write the very first snapshot (before any threads run). We lock/unlock even though no threads exist yet — good habit, and `write_snapshot` expects the lock to be held.

```c
    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_thread, NULL);
```
Create the monitor thread. Arguments: `NULL` attributes (default), `monitor_thread` function, `NULL` argument.

```c
    int n = argc - 1;
    pthread_t threads[n];
    Args args[n];

    for (int i = 0; i < n; i++) {
        args[i] = (Args){i, argv[i + 1]};
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }
```
Create N worker threads, one per script file. Thread 0 gets `argv[1]`, Thread 1 gets `argv[2]`, etc. We pass `&args[i]` — a pointer to each thread's own `Args` struct.

```c
    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);
```
**Wait** for all worker threads to finish. `pthread_join` blocks until the specified thread exits. We don't move on until all workers are done.

```c
    pthread_mutex_lock(&table_mutex);
    simulation_done = true;
    pthread_mutex_unlock(&table_mutex);
    sem_post(&sem_monitor);
```
Set the done flag under the mutex (for memory visibility), then post the monitor semaphore one final time to wake it up so it can see `done == true` and exit.

```c
    pthread_join(monitor, NULL);

    sem_destroy(&sem_monitor);
    fclose(snap_file);
    return 0;
}
```
Wait for the monitor thread to exit cleanly. Then destroy the semaphore (release OS resources) and close the file.

---

## 19. Why No pthread_cond_t?

The project explicitly forbids `pthread_cond_t`. Here's the comparison so you can explain it clearly:

### What pthread_cond_t does

A condition variable is used together with a mutex. The pattern is:

```c
pthread_mutex_lock(&m);
while (!condition)
    pthread_cond_wait(&cv, &m);   // atomically releases m and sleeps
// condition is now true
pthread_mutex_unlock(&m);
```

`pthread_cond_wait` atomically releases the mutex and puts the thread to sleep. When another thread calls `pthread_cond_signal` or `pthread_cond_broadcast`, the sleeping thread wakes up, re-acquires the mutex, and continues.

### The problem with cond_wait: spurious wakeups and broadcast noise

`pthread_cond_broadcast` wakes up **every** thread waiting on that condition. In a system with many parents waiting for children, all of them would wake up when *any* child exits — only one would find its zombie, and the rest would go back to sleep. This is wasteful.

### How semaphores solve this better

With per-PCB semaphores, each parent waits on **its own** semaphore. Only its own children post to that semaphore. So when child PID 5 exits, only the parent of PID 5 wakes up. Nobody else is disturbed.

This is a **cleaner, more targeted** signaling mechanism.

### Replacement mapping

| Original (cond_t) | Replacement (semaphore) |
|---|---|
| `pthread_cond_wait(&table_cond, &table_mutex)` in `pm_wait` | `sem_wait(&process_table[p_idx].sem_wait)` after releasing mutex |
| `pthread_cond_broadcast(&table_cond)` in `pm_exit`/`pm_kill` | `sem_post(&process_table[parent_idx].sem_wait)` |
| `pthread_cond_wait(&table_cond, &table_mutex)` in monitor | `sem_wait(&sem_monitor)` |
| `pthread_cond_broadcast(&table_cond)` in `write_snapshot` | `sem_post(&sem_monitor)` |

---

## 20. Concurrency Scenarios — Walk-Throughs

### Scenario 1: Normal fork and exit

**Script thread0.txt:**
```
fork 1
```
**Script thread1.txt:**
```
exit 2 10
```

1. Thread 0 calls `pm_fork(1, 0)` — acquires mutex, creates PID 2, writes snapshot, releases mutex
2. Thread 1 calls `pm_exit(2, 10, 1)` — acquires mutex, turns PID 2 to ZOMBIE, writes snapshot, releases mutex
3. Both snapshots are in the file; monitor prints after each one

### Scenario 2: Parent waits before child exits

**thread0.txt:** `wait 1 -1`  
**thread1.txt:** `sleep 100` then `exit 2 10`

1. Thread 0 calls `pm_wait(1, -1, 0)` — acquires mutex, scans for zombies, finds none
2. Sets PID 1 to BLOCKED, writes "(blocking)" snapshot, **releases mutex**, calls `sem_wait` on PID 1's semaphore — Thread 0 is now sleeping
3. Thread 1 (after 100ms) calls `pm_exit(2, 10, 1)` — acquires mutex (now free!), turns PID 2 to ZOMBIE
4. Writes snapshot (shows PID 1 as BLOCKED, PID 2 as ZOMBIE)
5. Calls `notify_parent(1)` → `sem_post(&process_table[0].sem_wait)` — wakes Thread 0
6. Thread 1 releases mutex
7. Thread 0 wakes from `sem_wait`, re-acquires mutex, loops back, finds PID 2 as ZOMBIE, reaps it
8. Writes final "pm_wait 1 -1" snapshot, releases mutex, returns 10

This produces the exact output shown in your screenshot.

### Scenario 3: Multiple threads forking simultaneously

**thread0.txt:** `fork 1`  
**thread1.txt:** `fork 1`

Both threads try to call `pm_fork(1, ...)` at the same time.

- Thread 0 acquires the mutex first → gets PID 2, releases mutex
- Thread 1 acquires the mutex next → gets PID 3, releases mutex

Because of the mutex, PIDs are always unique. Without the mutex, both could read `next_pid = 2` and both would try to create PID 2 — a race condition.

---

## 21. Viva Questions

**Q: What is a PCB and what does it contain in your implementation?**

A PCB (Process Control Block) is a struct that represents one simulated process. It contains the PID, parent PID (PPID), current state, exit status, a list of children PIDs, an `active` flag marking whether the slot is in use, and a per-process semaphore used for blocking in `pm_wait`.

---

**Q: What is a zombie process? How is it handled?**

A zombie is a process that has exited but hasn't been reaped by its parent yet. It stays in the process table in ZOMBIE state, holding its exit code, until the parent calls `pm_wait`. When the parent reaps it, the zombie transitions to TERMINATED and its slot is freed.

---

**Q: What is the purpose of the mutex in this program?**

The mutex (`table_mutex`) protects the shared process table from concurrent access. Since multiple threads can call any function simultaneously, without the mutex, two threads could read/write the same PCB entry at the same time, causing corruption. The mutex ensures only one thread is inside the critical section at any time.

---

**Q: Why do you use semaphores and not condition variables?**

Semaphores give us targeted signaling. Each process has its own semaphore, so when a child exits, only its parent wakes up. With condition variables and `pthread_cond_broadcast`, all waiting threads would wake up on any child exit, causing unnecessary context switches. Semaphores also don't require holding the mutex during the wait, which is the more natural pattern here.

---

**Q: Why must you release the mutex before calling `sem_wait` in `pm_wait`?**

If we kept the mutex locked while calling `sem_wait`, we'd be sleeping while holding the lock. No other thread could acquire the mutex, which means `pm_exit` could never run to post the semaphore. The parent would wait forever for the child to exit, and the child would wait forever for the mutex — this is a deadlock.

---

**Q: What happens if a parent calls `pm_wait` but the child already exited?**

`pm_wait` first scans for an existing zombie. If it finds one immediately, it reaps it and returns without ever calling `sem_wait`. No blocking occurs. This is the "fast path."

---

**Q: What does `sem_init(&sem, 0, 0)` mean?**

The three arguments are: pointer to semaphore, `pshared` flag (0 = shared between threads only, not across processes), and initial value (0 = the semaphore starts locked, any `sem_wait` will block until a `sem_post` is called).

---

**Q: What is `next_pid` and why is it protected by the mutex?**

`next_pid` is a global counter that tracks the next available PID. It must be protected by the mutex because without it, two threads calling `pm_fork` simultaneously could both read the same value of `next_pid`, assign the same PID to two different processes, and then both increment it — resulting in a duplicate PID. The mutex ensures only one thread increments `next_pid` at a time.

---

**Q: What does the monitor thread do and how does it know when to exit?**

The monitor thread waits on `sem_monitor`. Every time `write_snapshot` is called, it posts `sem_monitor`, waking the monitor. The monitor then locks the mutex, reads `simulation_done`, prints the table, and unlocks. If `simulation_done` is true, it exits. Main sets `simulation_done = true` after all worker threads finish, then posts `sem_monitor` one final time to wake the monitor for its last print-and-exit.

---

**Q: What is `fflush` and why is it called in `write_snapshot`?**

`fflush` forces the OS to write any buffered data to the actual file on disk. Without it, writes might stay in an internal buffer in RAM and not appear in the file until later. In a concurrent simulation that could crash or terminate unexpectedly, `fflush` ensures all snapshots are always persisted.

---

**Q: Why do you use `strncmp` for the "ps" command instead of `strcmp`?**

`fgets` includes the newline character `\n` at the end of each line. So the line read from the file is `"ps\n"`, not `"ps"`. `strcmp(line, "ps")` would fail. `strncmp(line, "ps", 2)` compares only the first 2 characters, ignoring the newline.

---

**Q: What is `PTHREAD_MUTEX_INITIALIZER`?**

It's a macro that statically initializes a mutex with default attributes at compile time. It's equivalent to calling `pthread_mutex_init(&m, NULL)` but works for global/static mutexes without needing explicit initialization code in `main`.

---

**Q: What is `pthread_join` and why is it called?**

`pthread_join(t, NULL)` blocks the calling thread until thread `t` finishes. Without it, `main` might reach `return 0` before worker threads finish, killing the entire process and all threads. We join all workers before setting `simulation_done`, ensuring all work is complete before we signal the monitor to exit.

---

**Q: What happens if `pm_fork` is called with an invalid parent PID?**

`find_by_pid(parent_pid)` returns -1. The code checks for this, releases the mutex immediately, and returns -1 (error). The table is not modified.

---

**Q: Can two threads fork children under the same parent simultaneously? Is this safe?**

Yes, it's safe because of the mutex. Thread A acquires the lock, completes the entire fork (including updating `parent->child_count`), and releases the lock. Then Thread B does the same. The parent's `children[]` array is updated atomically — no lost updates.

---

**Q: Why write the "(blocking)" snapshot before releasing the mutex?**

Because we want the snapshot file to show the parent as BLOCKED before we sleep. If we released the mutex first, `pm_exit` might run in the tiny gap between "mutex released" and "snapshot written," producing a snapshot that shows the zombie but the parent still as RUNNING — which would be incorrect and not match the expected output.

---

## 22. Common Bugs and Why We Avoided Them

### Bug 1: Deadlock in pm_wait

**Wrong approach:** Lock the mutex, then call `sem_wait` without releasing it.

```c
// WRONG
pthread_mutex_lock(&table_mutex);
process_table[p_idx].state = BLOCKED;
sem_wait(&process_table[p_idx].sem_wait);  // sleeping while holding lock!
```

**Why it deadlocks:** `pm_exit` needs `table_mutex` to turn the child into a ZOMBIE. But `pm_wait` is holding it and sleeping. Neither can proceed.

**Fix:** Unlock before `sem_wait`, relock after.

---

### Bug 2: Monitor exits before printing the last snapshot

**Wrong approach:** Check `simulation_done` first, then break, then print.

```c
// WRONG
sem_wait(&sem_monitor);
if (simulation_done) break;    // exit without printing!
print_table(stdout);
```

**Fix:** Print first, check `done` after:

```c
sem_wait(&sem_monitor);
// ...
print_table(stdout);
if (done) break;
```

---

### Bug 3: Race condition on simulation_done

**Wrong approach:** Read `simulation_done` without the mutex.

```c
// WRONG (in monitor thread)
bool done = simulation_done;   // not under mutex!
```

**Why it's a bug:** The CPU might cache the old value. Without the mutex enforcing memory synchronization, the monitor might never see `done = true`.

**Fix:** Always read/write `simulation_done` under `table_mutex`.

---

### Bug 4: strcmp vs strncmp for "ps"

**Wrong approach:**
```c
else if (strcmp(line, "ps") == 0)
```

`fgets` returns `"ps\n"`, so `strcmp` with `"ps"` fails. The `ps` command never executes.

**Fix:** `strncmp(line, "ps", 2)` — compare only 2 characters.

---

### Bug 5: Forgetting to sem_destroy

If you don't call `sem_destroy` on reaped PCBs, you leak OS semaphore resources. Over time in a long simulation, this could exhaust system limits. We call `sem_destroy(&process_table[zombie_idx].sem_wait)` inside `pm_wait` when reaping.

---

### Bug 6: Not unlocking before early return

```c
// WRONG
int pm_fork(int parent_pid, int tid) {
    pthread_mutex_lock(&table_mutex);
    int parent_idx = find_by_pid(parent_pid);
    if (parent_idx == -1)
        return -1;   // mutex still locked! Nobody else can run.
```

**Fix:** Always unlock before returning:
```c
    if (parent_idx == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }
```

---

### Bug 7: Using the same semaphore for all waiters (global sem instead of per-PCB)

If a global semaphore was used for all `pm_wait` callers, calling `sem_post` once would wake up only one random waiter — not necessarily the right parent. With per-PCB semaphores, `notify_parent` posts exactly the right parent's semaphore.

---

*End of Notes*

---

> **Compile command:** `gcc -o pm_sim pm_sim.c -lpthread`
>
> **Run example:** `./pm_sim thread0.txt thread1.txt`
>
> **Output files:** `snapshots.txt` (all table snapshots), stdout (monitor prints + ps output)