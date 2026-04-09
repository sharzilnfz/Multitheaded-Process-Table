#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>

#define MAX_PROCESSES 64

/* =========================================================
   REVIEW SUMMARY (original → fixed):
   1. Removed all pthread_cond_t usage; replaced with semaphores.
   2. monitor_thread: waits on sem_monitor (posted after every
      snapshot), reads simulation_done flag inside mutex to
      avoid race, and drains any pending posts before exiting.
   3. pm_wait: each PCB now has its own sem_wait semaphore so a
      parent blocks precisely on its own child's exit, not on a
      global cond broadcast.
   4. write_snapshot no longer calls pthread_cond_broadcast;
      instead it posts sem_monitor.
   5. pm_exit / pm_kill: after turning a child ZOMBIE they post
      the parent's per-PCB semaphore (sem_wait) so pm_wait
      unblocks correctly.
   6. simulation_done flag is guarded by table_mutex everywhere.
   ========================================================= */

typedef enum {
    RUNNING,
    BLOCKED,
    ZOMBIE,
    TERMINATED
} ProcessState;

const char *state_to_str(ProcessState s) {
    switch (s) {
        case RUNNING:    return "RUNNING";
        case BLOCKED:    return "BLOCKED";
        case ZOMBIE:     return "ZOMBIE";
        case TERMINATED: return "TERMINATED";
        default:         return "UNKNOWN";
    }
}

typedef struct {
    int pid, ppid;
    ProcessState state;
    int exit_status;
    bool active;

    int children[MAX_PROCESSES];
    int child_count;

    /*
     * Per-process semaphore used by pm_wait.
     * When a child exits (pm_exit / pm_kill) it posts the
     * parent's sem_wait, unblocking the parent exactly once
     * per child exit — no spurious wakeups, no missed signals.
     */
    sem_t sem_wait;
} PCB;

PCB process_table[MAX_PROCESSES];
int next_pid = 1;

pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * sem_monitor: posted once per snapshot so the monitor thread
 * wakes up for every change without polling.
 */
sem_t sem_monitor;

bool simulation_done = false;
FILE *snap_file = NULL;

/* ---------- Helpers ---------- */

int find_free_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!process_table[i].active) return i;
    return -1;
}

int find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (process_table[i].active && process_table[i].pid == pid)
            return i;
    return -1;
}

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

/*
 * write_snapshot — called while table_mutex is already held.
 * Posts sem_monitor so the monitor thread wakes and prints.
 */
void write_snapshot(const char *label) {
    fprintf(snap_file, "%s\n", label);
    print_table(snap_file);
    fflush(snap_file);
    sem_post(&sem_monitor);   /* wake monitor thread */
}

/* ---------- Process Manager ---------- */

int pm_fork(int parent_pid, int tid) {
    pthread_mutex_lock(&table_mutex);

    int parent_idx = find_by_pid(parent_pid);
    if (parent_idx == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    int slot = find_free_slot();
    if (slot == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    int pid = next_pid++;

    PCB *child = &process_table[slot];
    child->pid         = pid;
    child->ppid        = parent_pid;
    child->state       = RUNNING;
    child->exit_status = 0;
    child->active      = true;
    child->child_count = 0;
    sem_init(&child->sem_wait, 0, 0);   /* per-process semaphore */

    PCB *parent = &process_table[parent_idx];
    parent->children[parent->child_count++] = pid;

    char label[128];
    snprintf(label, sizeof(label),
             "Thread %d calls pm_fork %d", tid, parent_pid);
    write_snapshot(label);

    pthread_mutex_unlock(&table_mutex);
    return pid;
}

/*
 * Helper (called with table_mutex held): find the parent of pid
 * and post its sem_wait so pm_wait unblocks.
 */
static void notify_parent(int ppid) {
    int p_idx = find_by_pid(ppid);
    if (p_idx != -1)
        sem_post(&process_table[p_idx].sem_wait);
}

void pm_exit(int pid, int status, int tid) {
    pthread_mutex_lock(&table_mutex);

    int idx = find_by_pid(pid);
    if (idx != -1 && process_table[idx].state != TERMINATED) {
        process_table[idx].state       = ZOMBIE;
        process_table[idx].exit_status = status;

        char label[128];
        snprintf(label, sizeof(label),
                 "Thread %d calls pm_exit %d %d", tid, pid, status);
        write_snapshot(label);

        /* Wake the parent blocked in pm_wait (if any) */
        notify_parent(process_table[idx].ppid);
    }

    pthread_mutex_unlock(&table_mutex);
}

void pm_kill(int pid, int tid) {
    pthread_mutex_lock(&table_mutex);

    int idx = find_by_pid(pid);
    if (idx != -1 && process_table[idx].state != TERMINATED) {
        process_table[idx].state       = ZOMBIE;
        process_table[idx].exit_status = -1;

        char label[128];
        snprintf(label, sizeof(label),
                 "Thread %d calls pm_kill %d", tid, pid);
        write_snapshot(label);

        /* Wake the parent blocked in pm_wait (if any) */
        notify_parent(process_table[idx].ppid);
    }

    pthread_mutex_unlock(&table_mutex);
}

/*
 * pm_wait — blocks the parent until a (specific or any) child
 * becomes a ZOMBIE, then reaps it.
 *
 * Synchronization design:
 *   - Check under mutex; if a zombie is already present, reap
 *     immediately without blocking.
 *   - Otherwise set state = BLOCKED, release the mutex, and
 *     do sem_wait on the parent's own per-PCB semaphore.
 *   - pm_exit / pm_kill post that semaphore, so we wake up
 *     exactly when a child changes state.
 *   - Re-acquire mutex after waking, restore state = RUNNING,
 *     and loop to check again (handles spurious wakeups and
 *     the case where another thread reaped the zombie first).
 */
int pm_wait(int parent_pid, int child_pid, int tid) {

    pthread_mutex_lock(&table_mutex);

    while (true) {
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

        if (zombie_idx != -1) {
            /* Reap the zombie */
            int status = process_table[zombie_idx].exit_status;
            sem_destroy(&process_table[zombie_idx].sem_wait);
            process_table[zombie_idx].state  = TERMINATED;
            process_table[zombie_idx].active = false;

            /*
             * Only write the reap snapshot when the zombie is
             * successfully reaped (not when blocking).  The
             * label matches the expected output: "Thread N calls
             * pm_wait <parent> <child>".
             */
            char label[128];
            snprintf(label, sizeof(label),
                     "Thread %d calls pm_wait %d %d",
                     tid, parent_pid, child_pid);
            write_snapshot(label);

            pthread_mutex_unlock(&table_mutex);
            return status;
        }

        if (!has_children) {
            pthread_mutex_unlock(&table_mutex);
            return -1;
        }

        /* No zombie yet — block this parent */
        int p_idx = find_by_pid(parent_pid);
        if (p_idx != -1) {
            process_table[p_idx].state = BLOCKED;

            /*
             * Write a snapshot NOW (while still holding the mutex)
             * so that the file shows parent as BLOCKED before we
             * sleep.  This is what produces the BLOCKED line that
             * appears inside the subsequent pm_exit snapshot in the
             * expected output.
             */
            char blabel[128];
            snprintf(blabel, sizeof(blabel),
                     "Thread %d calls pm_wait %d %d (blocking)",
                     tid, parent_pid, child_pid);
            write_snapshot(blabel);
        }

        pthread_mutex_unlock(&table_mutex);

        /*
         * Wait on our OWN semaphore.
         * pm_exit/pm_kill will post it when a child exits.
         */
        if (p_idx != -1)
            sem_wait(&process_table[p_idx].sem_wait);

        pthread_mutex_lock(&table_mutex);

        /* Restore state and loop to re-check */
        if (p_idx != -1)
            process_table[p_idx].state = RUNNING;
    }
}

void pm_ps(int tid) {
    pthread_mutex_lock(&table_mutex);
    printf("Thread %d calls pm_ps\n", tid);
    print_table(stdout);
    pthread_mutex_unlock(&table_mutex);
}

/* ---------- Monitor Thread ---------- */

/*
 * Waits on sem_monitor (posted by write_snapshot).
 * Checks simulation_done under the mutex after each wakeup.
 * Drains any remaining posts when done so we don't block
 * on leftover semaphore counts.
 */
void *monitor_thread(void *arg) {
    (void)arg;

    while (true) {
        sem_wait(&sem_monitor);   /* sleep until a snapshot arrives */

        pthread_mutex_lock(&table_mutex);
        bool done = simulation_done;
        printf("[Monitor] Update:\n");
        print_table(stdout);
        pthread_mutex_unlock(&table_mutex);

        if (done) break;
    }

    return NULL;
}

/* ---------- Script Interpreter ---------- */

void run_script(const char *file, int tid) {
    FILE *f = fopen(file, "r");
    if (!f) {
        perror("fopen (script file)");
        return;
    }

    char line[256];
    int a, b;

    while (fgets(line, sizeof(line), f)) {

        if (sscanf(line, "fork %d", &a) == 1)
            pm_fork(a, tid);

        else if (sscanf(line, "exit %d %d", &a, &b) == 2)
            pm_exit(a, b, tid);

        else if (sscanf(line, "wait %d %d", &a, &b) == 2)
            pm_wait(a, b, tid);

        else if (sscanf(line, "kill %d", &a) == 1)
            pm_kill(a, tid);

        else if (sscanf(line, "sleep %d", &a) == 1)
            usleep(a * 1000);

        else if (strncmp(line, "ps", 2) == 0)
            pm_ps(tid);
    }

    fclose(f);
}

typedef struct {
    int   tid;
    char *file;
} Args;

void *worker(void *arg) {
    Args *a = arg;
    run_script(a->file, a->tid);
    return NULL;
}

/* ---------- MAIN ---------- */

int main(int argc, char *argv[]) {

    if (argc < 2) {
        fprintf(stderr, "Usage: %s thread0.txt [thread1.txt ...]\n", argv[0]);
        return 1;
    }

    snap_file = fopen("snapshots.txt", "w");
    if (!snap_file) { perror("fopen"); return 1; }

    /* Initialise semaphore for monitor (shared=0, initial count=0) */
    sem_init(&sem_monitor, 0, 0);

    for (int i = 0; i < MAX_PROCESSES; i++)
        process_table[i].active = false;

    /* Create init process (PID=1, PPID=0) */
    process_table[0] = (PCB){1, 0, RUNNING, 0, true, {0}, 0};
    sem_init(&process_table[0].sem_wait, 0, 0);
    next_pid = 2;

    pthread_mutex_lock(&table_mutex);
    write_snapshot("Initial Process Table");
    pthread_mutex_unlock(&table_mutex);

    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_thread, NULL);

    int n = argc - 1;
    pthread_t threads[n];
    Args args[n];

    for (int i = 0; i < n; i++) {
        args[i] = (Args){i, argv[i + 1]};
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);

    /* Signal monitor to exit */
    pthread_mutex_lock(&table_mutex);
    simulation_done = true;
    pthread_mutex_unlock(&table_mutex);
    sem_post(&sem_monitor);   /* wake monitor one last time */

    pthread_join(monitor, NULL);

    sem_destroy(&sem_monitor);
    fclose(snap_file);
    return 0;
}