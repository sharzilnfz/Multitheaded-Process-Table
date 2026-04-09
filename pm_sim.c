#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>

#define MAX_PROCESSES 64

typedef enum {
    RUNNING,
    BLOCKED,
    ZOMBIE,
    TERMINATED
} ProcessState;

const char *state_to_str(ProcessState s) {
    switch (s) {
        case RUNNING: return "RUNNING";
        case BLOCKED: return "BLOCKED";
        case ZOMBIE: return "ZOMBIE";
        case TERMINATED: return "TERMINATED";
        default: return "UNKNOWN";
    }
}

typedef struct {
    int pid, ppid;
    ProcessState state;
    int exit_status;
    bool active;

    int children[MAX_PROCESSES];
    int child_count;

} PCB;

PCB process_table[MAX_PROCESSES];
int next_pid = 1;

pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  table_cond  = PTHREAD_COND_INITIALIZER;

bool simulation_done = false;
FILE *snap_file = NULL;

/* ---------- Helpers ---------- */

int find_free_slot() {
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

void write_snapshot(const char *label) {
    fprintf(snap_file, "%s\n", label);
    print_table(snap_file);
    fflush(snap_file);
    pthread_cond_broadcast(&table_cond);
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

    PCB *parent = &process_table[parent_idx];
    parent->children[parent->child_count++] = pid;

    char label[128];
    snprintf(label, sizeof(label),
             "Thread %d calls pm_fork %d", tid, parent_pid);
    write_snapshot(label);

    pthread_mutex_unlock(&table_mutex);
    return pid;
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

        /* FIX 1: wake any parent blocked in pm_wait */
        pthread_cond_broadcast(&table_cond);
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

        /* FIX 1: wake any parent blocked in pm_wait */
        pthread_cond_broadcast(&table_cond);
    }

    pthread_mutex_unlock(&table_mutex);
}

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
            int status = process_table[zombie_idx].exit_status;

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

        if (!has_children) {
            pthread_mutex_unlock(&table_mutex);
            return -1;
        }

        int p_idx = find_by_pid(parent_pid);
        if (p_idx != -1) process_table[p_idx].state = BLOCKED;

        pthread_cond_wait(&table_cond, &table_mutex);

        if (p_idx != -1) process_table[p_idx].state = RUNNING;
    }
}

void pm_ps(int tid) {
    pthread_mutex_lock(&table_mutex);
    printf("Thread %d calls pm_ps\n", tid);
    print_table(stdout);
    pthread_mutex_unlock(&table_mutex);
}

/* ---------- Threads ---------- */

void *monitor_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&table_mutex);

    while (true) {
        pthread_cond_wait(&table_cond, &table_mutex);
        printf("[Monitor] Update:\n");
        print_table(stdout);
        /* FIX 2: check exit condition AFTER printing, not before */
        if (simulation_done) break;
    }

    pthread_mutex_unlock(&table_mutex);
    return NULL;
}

void run_script(const char *file, int tid) {
    FILE *f = fopen(file, "r");
    if (!f) return;

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

        /* FIX 3: use strncmp so "ps" matches regardless of line endings */
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

    for (int i = 0; i < MAX_PROCESSES; i++)
        process_table[i].active = false;

    process_table[0] = (PCB){1, 0, RUNNING, 0, true, {0}, 0};
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

    pthread_mutex_lock(&table_mutex);
    simulation_done = true;
    pthread_cond_broadcast(&table_cond);
    pthread_mutex_unlock(&table_mutex);

    pthread_join(monitor, NULL);

    fclose(snap_file);
    return 0;
}