# 🚀 Multithreaded Process Table Simulator: The Ultimate Guide

This guide is designed to take you from knowing nothing about this project to being a complete master. Whether you're preparing for a Viva (oral exam) or just want to understand how operating systems manage processes, this document covers it all.

---

## 📖 Table of Contents
1. [What is this Project?](#-what-is-this-project)
2. [Core OS Concepts (The "Why")](#-core-os-concepts-the-why)
3. [Architecture: How it all fits together](#-architecture-how-it-all-fits-together)
4. [Line-by-Line Code Breakdown](#-line-by-line-code-breakdown)
5. [Concurrency & Synchronization (The Hard Part)](#-concurrency--synchronization-the-hard-part)
6. [The Viva Ace: Common Questions & Answers](#-the-viva-ace-common-questions--answers)
7. [How to Run and Test](#-how-to-run-and-test)

---

## 🎯 What is this Project?
This is a **Multithreaded Process Manager Simulator**. 
- It simulates how an Operating System (OS) keeps track of processes using a **Process Control Block (PCB)** and a **Process Table**.
- It uses **Threads** to simulate concurrent actions (like two people trying to create processes at the exact same time).
- It handles complex synchronization using **Mutexes** and **Condition Variables**.

---

## 🧠 Core OS Concepts (The "Why")

### 1. The Process Control Block (PCB)
In a real OS (like Linux), every process has a "passport" or ID card. This is the PCB. It contains the PID, the Parent's PID, and the Current State.

### 2. Process States
*   **RUNNING**: The process is doing work.
*   **BLOCKED**: The process is waiting for something (like a parent waiting for a child to finish).
*   **ZOMBIE**: The process has finished (`exit`), but its parent hasn't acknowledged it yet. It stays in the table so the parent can read its exit status.
*   **TERMINATED**: The process is completely gone and its slot in the table can be reused.

### 3. "Reaping" a Process
When a process finishes, it doesn't just vanish. It becomes a **Zombie**. The parent must call `wait()` to "reap" it. This removes the zombie from the table and allows the OS to reuse that memory.

---

## 🏗 Architecture: How it all fits together

The program runs three types of threads simultaneously:
1.  **Main Thread**: The "Manager." It sets up the initial process (Init), creates other threads, and waits for them to finish.
2.  **Worker Threads**: These read your `.txt` script files. Each file represents a series of actions (fork, exit, wait) happening in parallel.
3.  **Monitor Thread**: This is a background "Observer." Every time the process table changes, this thread wakes up and prints the new state to your screen.

---

## 🔍 Line-by-Line Code Breakdown

### 1. Data Structures (The Foundation)
```c
typedef struct {
    int           pid;
    int           ppid;
    ProcessState  state;
    int           exit_status;
    bool          active;
} PCB;
```
*   `pid`: Unique ID for the process.
*   `ppid`: Parent ID. (Who created this process?)
*   `state`: Is it Running, Blocked, or a Zombie?
*   `active`: A boolean flag to tell if this slot in our array is currently used.

### 2. Synchronization Primitives
```c
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  table_cond  = PTHREAD_COND_INITIALIZER;
```
*   **Mutex**: A "Lock." Since multiple threads might try to `fork` at the same time, we must lock the table so only one thread can modify it at a time. This prevents "Race Conditions."
*   **Condition Variable**: A "Signal." It allows threads to "sleep" and wait for a specific event (like a child exiting) without wasting CPU power.

### 3. Core Functions

#### `pm_fork(parent_pid)`
1.  Locks the mutex.
2.  Finds an empty slot in the `process_table`.
3.  Assigns a new PID.
4.  Sets the state to `RUNNING`.
5.  Records a "Snapshot" in `snapshots.txt`.
6.  Unlocks the mutex.

#### `pm_exit(pid, status)`
1.  Finds the process in the table.
2.  Changes its state to `ZOMBIE`.
3.  Sets the `exit_status`.
4.  **Crucial**: Calls `pthread_cond_broadcast`. This "wakes up" any parent thread that was waiting for a child to exit.

#### `pm_wait(parent_pid, child_pid)`
1.  This is a `while(true)` loop.
2.  It looks for a child that is in the `ZOMBIE` state.
3.  **If found**: It reaps it (sets `active = false`), returns the status, and exits.
4.  **If not found but children exist**: It calls `pthread_cond_wait`. This puts the thread to sleep and **automatically releases the lock**. When a child exits, this thread wakes up, re-acquires the lock, and checks again.

---

## 🤝 Concurrency & Synchronization (The Hard Part)

**Question: Why do we need `pthread_cond_wait` inside `pm_wait`?**
*   **Scenario**: Parent calls `wait`, but the child is still running.
*   **Bad way**: The parent keeps checking in a loop (`while(zombie == NULL);`). This is called "Busy Waiting" and it kills the CPU.
*   **Good way (Our way)**: The parent says "I'm going to sleep. Wake me up when any process exits." `pthread_cond_wait` does exactly this.

---

## 🎓 The Viva Ace: Common Questions & Answers

**Q1: What is a Race Condition, and how did you prevent it?**
> A race condition happens when two threads try to modify the same data (the process table) at once. I prevented it by using a **Mutex**. Only the thread holding the lock can modify the table.

**Q2: What is the difference between a Zombie and a Terminated process?**
> A Zombie process has finished execution but still has an entry in the process table so its parent can read its exit status. A Terminated process has been "reaped" by its parent (via `wait`), and its entry has been removed from the table.

**Q3: Why did you use a Condition Variable instead of just a Mutex?**
> A Mutex only provides "mutual exclusion" (locking). A Condition Variable provides "signaling." It allows a thread to wait for a specific *condition* (like a state change) to become true without wasting CPU cycles.

**Q4: What happens if a parent calls `wait` but has no children?**
> My implementation checks for this! If `has_children` is false, `pm_wait` returns `-1` immediately instead of sleeping forever.

**Q5: What is the purpose of the Monitor Thread?**
> It acts as a real-time logger. It sleeps until it receives a signal that the table has changed, then it prints the current state of all processes to the console. This provides visibility into the system's "internal organs" while it runs.

---

## 🛠 How to Run and Test

### Compilation
```bash
gcc -Wall -o pm_sim pm_sim.c -lpthread
```

### Execution
```bash
./pm_sim thread1.txt thread2.txt
```

### Verification
1.  **Check `stdout`**: You should see the Monitor thread printing the table as it changes.
2.  **Check `snapshots.txt`**: This file contains a chronological history of every single action and the resulting state of the table.

---

*This guide was created to ensure you understand not just WHAT the code does, but WHY it does it. Good luck with your Viva!*
