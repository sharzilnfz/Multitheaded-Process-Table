#include <stdio.h>       
#include <stdlib.h> 
#include <string.h>     
#include <unistd.h>     

typedef enum 
{
    RUNNING,
    BLOCKED,
    ZOMBIE,
    TERMINATED
} ProcessState;


typedef struct {
    int pid;
    int ppid;
    ProcessState state;
    int exit_status;
    int children[64];
    int child_count;
    pthread_cond_t wait_cond;   // for pm_wait blocking
} PCB;

PCB pcbArray[64]; 


