#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
extern void job_add(pid_t);
static int run(Node*n,bool child);
static int apply(Redir*r){
    for(;r;r=r->next){
        int fd, target=r->kind==R_IN?STDIN_FILENO:STDOUT_FILENO;
        int flags=r->kind==R_IN?O_RDONLY:O_WRONLY|O_CREAT|(r->kind==R_APPEND?O_APPEND:O_TRUNC);
        fd=open(r->path,flags,0666);
        if(fd<0){
            fprintf(stderr,"mysh: %s: %s\n",r->path,strerror(errno));
            return -1;
        }
        if(dup2(fd, target)<0){
            fprintf(stderr,"mysh: dup2: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
    }
    return 0;

}
static void flatten(Node*n,Node**v,size_t*k){
    if(n && n->kind==N_PIPE){
        flatten(n->left,v,k);
        flatten(n->right,v,k);
    }
    else 
        v[(*k)++]=n;
    }

static int pipeline(Node*n){
    Node*cmds[256];
    size_t c=0;
    flatten(n,cmds,&c);
    if(c>256){
        fprintf(stderr,"mysh: pipeline too long\n");
        return 2;
    }
    int prev=-1, st=0;
    pid_t pids[256];
    for(size_t i=0; i<c; i++){
        int fds[2]={-1,-1};
        if(i+1<c && pipe(fds)<0){
            perror("mysh: pipe");
            return 1;
        }
        pid_t p=fork();
        if(p<0){
            perror("mysh: fork");
            return 1;
        }
        if(!p){
            if(prev>=0)
                dup2(prev,STDIN_FILENO);
            if(fds[1]>=0)
                dup2(fds[1],STDOUT_FILENO);
            if(prev>=0)
                close(prev);
            if(fds[0]>=0)
                close(fds[0]);
            if(fds[1]>=0)
                close(fds[1]);
            int rc=run(cmds[i],true);
            _exit(rc&255);
        }
        pids[i]=p;
        if(prev>=0)
            close(prev);
        if(fds[1]>=0)
            close(fds[1]);
        prev=fds[0];
    }
    if(prev>=0)
        close(prev);
    for(size_t i=0; i<c; i++){
        pid_t r;
        do {
            r=waitpid(pids[i],&st,0);
        }
        while (r<0 && errno==EINTR);
        if(i+1==c && r>0)
            return WIFEXITED(st)?WEXITSTATUS(st):128+WTERMSIG(st);
    }
    return 1;
}

static int simple(Node*n,bool child){
    if(child){
        if(apply(n->redirs)<0)
            _exit(1);
        bool handled=false, quit=false;
        int ec=0;
        int rc=run_builtin(n->argv,&handled,&quit,&ec);
        if(handled)
            _exit(rc&255);
        execvp(n->argv[0],n->argv);
        int e=errno;
        fprintf(stderr,"mysh: %s: %s\n",n->argv[0],strerror(e));
        _exit(e==ENOENT?127:126);
    }
    bool is_builtin=n->argv && (!strcmp(n->argv[0],"cd") || !strcmp(n->argv[0],"pwd") || !strcmp(n->argv[0],"echo") || !strcmp(n->argv[0],"help") || !strcmp(n->argv[0],"exit") || !strcmp(n->argv[0],"jobs"));
    if(is_builtin){
        int in=dup(STDIN_FILENO), out=dup(STDOUT_FILENO);
        if(in<0 || out<0){
            perror("mysh: dup");
            if(in>=0)
                close(in);
            if(out>=0)
                close(out);
            return 1;
        }
        int rc=apply(n->redirs)<0?1:0;
        if(!rc){
            bool handled=false,quit=false;
            int ec=0;
            rc=run_builtin(n->argv,&handled,&quit,&ec);
        }
        if(dup2(in,STDIN_FILENO)<0 || dup2(out,STDOUT_FILENO)<0)
            perror("mysh: dup2");
        close(in);
        close(out);
        return rc;
    }
    pid_t p=fork();
    if(p<0){
        perror("mysh: fork");
        return 1;
    }
    if(!p){
        if(apply(n->redirs)<0)
            _exit(1);
        execvp(n->argv[0],n->argv);
        int e=errno;
        fprintf(stderr,"mysh: %s: %s\n",n->argv[0],strerror(e));
        _exit(e==ENOENT?127:126);
    }
    int st;
    pid_t r;
    do {
        r=waitpid(p,&st,0);
    }
    while(r<0&&errno==EINTR);
    return r<0?1:(WIFEXITED(st)?WEXITSTATUS(st):128+WTERMSIG(st));
}

static int run(Node*n,bool child){
    if(!n)
        return 0;
    switch(n->kind){
        case N_SIMPLE: return simple(n,child);
        case N_SUBSHELL:{
            pid_t p=fork();
            if(!p){
                if(apply(n->redirs)<0)
                    _exit(1);
                _exit(run(n->left,true)&255);
            }
            if(p<0)
                return 1;
            int s;
            while(waitpid(p,&s,0)<0 && errno==EINTR){

            }
            return WIFEXITED(s)?WEXITSTATUS(s):128+WTERMSIG(s);
        }
        case N_PIPE: return pipeline(n);
        case N_AND:{
            int x=run(n->left,child);
            return x==0?run(n->right,child):x;
        }
        case N_OR:{
            int x=run(n->left,child);
            return x!=0?run(n->right,child):x;
        }
        case N_LIST:(void)run(n->left,child);
        return run(n->right,child);
        case N_BG:{
            pid_t p=fork();
            if(p<0)
                return 1;
            if(!p)
                _exit(run(n->left,true)&255);
            job_add(p);
            printf("[1] %ld\n",(long)p);
            return 0;
        }
    }
    return 2;
}
int execute(Node*n,bool child){
    return run(n,child);
}
