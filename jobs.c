#include "shell.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#define MAX_JOBS 128
typedef struct {
    pid_t pid;
    int status; 
    int used;
} Job;
static Job table[MAX_JOBS];
void jobs_init(bool interactive){(void)interactive;}
static void add(pid_t p){
    for(int i=0; i<MAX_JOBS; i++)
    if(!table[i].used){
        table[i]=(Job){p,0,1};
        break;
    }
}

void jobs_reap(void){
    int st;
    pid_t p;
    while((p=waitpid(-1, &st,WNOHANG))>0)
    for(int i=0; i<MAX_JOBS; i++)
    if(table[i].used && table[i].pid==p)
    table[i].status=st;
}

void jobs_print(void){
    jobs_reap();
    int n=0;
    for(int i=0; i<MAX_JOBS; i++)
    if(table[i].used){
        if(WIFEXITED(table[i].status) || WIFSIGNALED(table[i].status)){
            table[i].used=0;
            continue;
        }
        printf("[%d] Running pid %ld &\n", ++n, (long)table[i].pid);
    }
}

void jobs_shutdown(void){jobs_reap();}
void job_add(pid_t p){add(p);}
