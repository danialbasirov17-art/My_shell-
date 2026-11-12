#ifndef MYSH_SHELL_H
#define MYSH_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
  T_WORD, T_PIPE, T_AMP, T_SEMI, T_NL, T_AND, T_OR,
  T_LP, T_RP, T_IN, T_OUT, T_APPEND, T_END
} TokenKind;

typedef struct { TokenKind kind; char *text; } Token;
typedef struct { Token *v; size_t n; } Tokens;

typedef enum {
  LEX_OK = 0,
  LEX_ERROR = -1,
  LEX_UNCLOSED = 1,   
  LEX_CONTINUE = 2    
} LexStatus;

int lex_line(const char *s, Tokens *out, char **error);
void free_tokens(Tokens *t);
const char *token_name(TokenKind k);    
const char *token_symbol(TokenKind k);  

typedef enum { R_IN, R_OUT, R_APPEND } RedirKind;
typedef struct Redir { RedirKind kind; char *path; struct Redir *next; } Redir;


typedef enum { N_SIMPLE, N_SUBSHELL, N_PIPE, N_AND, N_OR, N_LIST, N_BG } NodeKind;
typedef struct Node Node;
struct Node {
  NodeKind kind;
  Node *left, *right;
  char **argv;
  size_t argc;
  Redir *redirs;
};

typedef enum {
  PARSE_OK = 0,
  PARSE_ERROR = -1,
  PARSE_INCOMPLETE = 1  
} ParseStatus;

int parse_tokens(const Tokens *t, Node **out, char **error);
void free_node(Node *n);

void print_ast(const Node *n, int depth);
char *ast_to_string(const Node *n); 

int execute(Node *n, int last_status);



bool is_builtin(const char *name);
int run_builtin(char **argv, int last_status);
bool shell_exit_requested(void);
int shell_exit_code(void);
void builtin_line_begin(void);          
bool builtin_eof_blocked(void);         

typedef struct Job Job;
typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobState;

void jobs_init(bool interactive);
bool jobs_enabled(void);            
void jobs_shutdown(void);
void jobs_notify(bool print);     

Job *jobs_new(const char *cmd, bool background);
int jobs_add_proc(Job *j, pid_t pid);
pid_t jobs_pgid(const Job *j);         
void jobs_discard(Job *j);
int jobs_run_fg(Job *j);
int jobs_run_bg(Job *j);
void jobs_child_start(pid_t pgid, bool foreground);

Job *jobs_find_spec(const char *spec);  
Job *jobs_current(void);
int jobs_id(const Job *j);
JobState jobs_state(const Job *j);
void jobs_print(Job *only);
int jobs_continue_fg(Job *j);
int jobs_continue_bg(Job *j);
int jobs_signal(Job *j, int sig);
int jobs_wait_all(void);
int jobs_wait_job(Job *j);
int jobs_wait_pid(pid_t pid, int *status);
bool jobs_has_stopped(void);

#endif
