#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
static bool request_exit;
static int requested_code;
bool shell_exit_requested(void){return request_exit;}
int shell_exit_code(void){return requested_code;}

int run_builtin(char **a,bool *handled,bool *quit,int *code){
    *handled=true;
    *quit=false;
    *code=0;
    if(!a || !a[0])
        return 0;
    if(!strcmp(a[0],"cd")){
        if(a[1]&&a[2]){
            fprintf(stderr,"mysh: cd: too many arguments\n");
            return 1;
        }
        const char*d=a[1]?a[1]:getenv("HOME");
        if(!d){
            fprintf(stderr,"mysh: cd: HOME not set\n");
            return 1;
        }
        if(chdir(d)<0){
            fprintf(stderr,"mysh: cd: %s: %s\n",d,strerror(errno));
            return 1;
        }
        return 0;
    }
    if(!strcmp(a[0],"pwd")){
        char b[PATH_MAX];
        if(!getcwd(b,sizeof b)){
            fprintf(stderr,"mysh: pwd: %s\n",strerror(errno));
            return 1;
        }
        puts(b);
        return 0;
    }
    if(!strcmp(a[0],"echo")){
        int i=1;
        bool nl=true;
        if(a[i] && !strcmp(a[i],"-n")){
            nl=false;i++;
        }
        for(; a[i]; i++){
            if(i>(nl?1:2))
                putchar(' ');
            fputs(a[i],stdout);
        }
        if(nl)
            putchar('\n');
        return ferror(stdout)?1:0;
    }
    if(!strcmp(a[0],"help")){
        puts("cd [dir]  pwd  echo [-n] [args]  help  exit [code]  jobs  fg  bg  kill  wait");
        return 0;
    }
    if(!strcmp(a[0],"exit")){
        *quit=true;
        if(a[1])
            *code=atoi(a[1])&255;
        request_exit=true;
        requested_code=*code;
        return *code;
    }
    if(!strcmp(a[0],"jobs")){
        jobs_print();
        return 0;
    }
    *handled=false;
    return 0;
}
