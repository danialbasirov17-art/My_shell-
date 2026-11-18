#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  STATUS_FAIL = 1,
  STATUS_CANNOT_EXEC = 126,   
  STATUS_NOT_FOUND = 127,
  EXIT_MASK = 255,
  SAVED_FD_MIN = 10,          
  STD_FDS = 3                 
};

static int run_tree(Node *n, int last);


static int apply_redirs(const Redir *r) {
  for (; r; r = r->next) {
    int target = r->kind == R_IN ? STDIN_FILENO : STDOUT_FILENO;
    int flags = r->kind == R_IN ? O_RDONLY
                                : O_WRONLY | O_CREAT | (r->kind == R_APPEND ? O_APPEND : O_TRUNC);
    int fd = open(r->path, flags | O_CLOEXEC, 0666);   
    if (fd < 0) {
      fprintf(stderr, "mysh: %s: %s\n", r->path, strerror(errno));
      return -1;
    }
    if (fd == target) {                  
      if (fcntl(fd, F_SETFD, 0) < 0) {
        fprintf(stderr, "mysh: %s: %s\n", r->path, strerror(errno));
        close(fd);
        return -1;
      }
      continue;
    }
    if (dup2(fd, target) < 0) {
      fprintf(stderr, "mysh: dup2: %s\n", strerror(errno));
      close(fd);
      return -1;
    }
    close(fd);
  }
  return 0;
}

typedef struct { int fd[STD_FDS]; } SavedFds;

static void save_fds(SavedFds *s) {
  for (int i = 0; i < STD_FDS; i++) 
      s->fd[i] = fcntl(i, F_DUPFD_CLOEXEC, SAVED_FD_MIN);
}

static void restore_fds(SavedFds *s) {
  for (int i = 0; i < STD_FDS; i++) {
    if (s->fd[i] < 0) continue;        
    if (dup2(s->fd[i], i) < 0) 
        fprintf(stderr, "mysh: dup2: %s\n", strerror(errno));
    close(s->fd[i]);
  }
}


static int run_builtin_inproc(Node *n, int last) {
  SavedFds saved;
  int rc;
  bool redirected = n->redirs != NULL;

  fflush(stdout);                        
  if (redirected) {
    save_fds(&saved);
    if (apply_redirs(n->redirs) < 0) {
      restore_fds(&saved);
      return STATUS_FAIL;
    }
  }
  rc = run_builtin(n->argv, last);
  if (fflush(stdout) != 0) {              
    if (rc == 0) {
      fprintf(stderr, "mysh: %s: write error: %s\n", n->argv[0], strerror(errno));
      rc = STATUS_FAIL;
    }
    clearerr(stdout);
  }
  if (redirected) restore_fds(&saved);
  return rc;
}

[[noreturn]] static void child_exit(int rc) {
  fflush(stdout);
  if (shell_exit_requested()) rc = shell_exit_code();   /* exit внутри подоболочки */
  _exit(rc & EXIT_MASK);
}

[[noreturn]] static void exec_external(char **argv) {
  execvp(argv[0], argv);
  int e = errno;
  struct stat sb;
  const char *msg = strerror(e);
  if (e == ENOENT && !strchr(argv[0], '/')) 
      msg = "command not found";
  else if (e == EACCES && stat(argv[0], &sb) == 0 && S_ISDIR(sb.st_mode)) 
      msg = "Is a directory";
  fprintf(stderr, "mysh: %s: %s\n", argv[0], msg);
  _exit(e == ENOENT ? STATUS_NOT_FOUND : STATUS_CANNOT_EXEC);
}

[[noreturn]] static void exec_stage(Node *n, int last) {
  switch (n->kind) {
    case N_SIMPLE:
      if (apply_redirs(n->redirs) < 0) 
          _exit(STATUS_FAIL);
      if (is_builtin(n->argv[0])) 
          child_exit(run_builtin(n->argv, last));
      exec_external(n->argv);
    case N_SUBSHELL:
      if (apply_redirs(n->redirs) < 0) _exit(STATUS_FAIL);
      child_exit(run_tree(n->left, last));
    default:
      child_exit(run_tree(n, last));
  }
}

static void move_fd(int from, int to) {
  if (from == to) return;
  dup2(from, to);
  close(from);
}

static int launch_job(Node **stages, size_t cnt, const char *cmd, bool bg, int last) {
  Job *job = jobs_new(cmd, bg);
  if (!job) return STATUS_FAIL;
  bool async_plain = bg && !jobs_enabled();
  int prev = -1;
  bool failed = false;

  fflush(NULL);                           
  for (size_t i = 0; i < cnt; i++) {
    int fds[2] = {-1, -1};
    if (i + 1 < cnt && pipe(fds) < 0) {
      fprintf(stderr, "mysh: pipe: %s\n", strerror(errno));
      failed = true;
      break;
    }
    pid_t pid = fork();
    if (pid < 0) {
      fprintf(stderr, "mysh: fork: %s\n", strerror(errno));
      if (fds[0] >= 0) close(fds[0]);
      if (fds[1] >= 0) close(fds[1]);
      failed = true;
      break;
    }
    if (pid == 0) {
      jobs_child_start(jobs_pgid(job), !bg);
      if (async_plain) {
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        if (prev < 0) {
          int nul = open("/dev/null", O_RDONLY);
          if (nul >= 0) move_fd(nul, STDIN_FILENO);
        }
      }
      
      if (fds[0] >= 0) close(fds[0]);
      if (prev >= 0) move_fd(prev, STDIN_FILENO);
      if (fds[1] >= 0) move_fd(fds[1], STDOUT_FILENO);
      exec_stage(stages[i], last);
    }
    if (jobs_add_proc(job, pid) < 0) {
      fprintf(stderr, "mysh: out of memory\n");
      failed = true;
    }
    if (prev >= 0) close(prev);
    if (fds[1] >= 0) close(fds[1]);
    prev = fds[0];
    if (failed) break;
  }
  if (prev >= 0) close(prev);

  if (jobs_pgid(job) == 0) {         
    jobs_discard(job);
    return STATUS_FAIL;
  }
  int rc = bg ? jobs_run_bg(job) : jobs_run_fg(job);
  return failed ? STATUS_FAIL : rc;
}

static size_t count_stages(const Node *n) {
  return n->kind == N_PIPE ? count_stages(n->left) + count_stages(n->right) : 1;
}

static void flatten(Node *n, Node **v, size_t *k) {
  if (n->kind == N_PIPE) {
    flatten(n->left, v, k);
    flatten(n->right, v, k);
  } else {
    v[(*k)++] = n;
  }
}

static int run_as_job(Node *n, bool bg, int last) {
  char *cmd = ast_to_string(n);
  size_t cnt = n->kind == N_PIPE ? count_stages(n) : 1, k = 0;
  Node **stages = malloc(cnt * sizeof(*stages));
  if (!stages) {
    free(cmd);
    fprintf(stderr, "mysh: out of memory\n");
    return STATUS_FAIL;
  }
  if (n->kind == N_PIPE) 
      flatten(n, stages, &k);
  else stages[0] = n;
  int rc = launch_job(stages, cnt, cmd, bg, last);
  free(stages);
  free(cmd);
  return rc;
}


static int run_tree(Node *n, int last) {
  if (!n) return last;
  switch (n->kind) {
    case N_SIMPLE:
      if (is_builtin(n->argv[0])) return run_builtin_inproc(n, last);
      return run_as_job(n, false, last);
    case N_SUBSHELL:
    case N_PIPE:
      return run_as_job(n, false, last);
    case N_BG:
      run_as_job(n->left, true, last);
      return 0;
    case N_AND: {
      int x = run_tree(n->left, last);
      if (shell_exit_requested() || x != 0) return x;
      return run_tree(n->right, x);
    }
    case N_OR: {
      int x = run_tree(n->left, last);
      if (shell_exit_requested() || x == 0) return x;
      return run_tree(n->right, x);
    }
    case N_LIST: {
      int x = run_tree(n->left, last);
      if (shell_exit_requested()) return x;
      return run_tree(n->right, x);
    }
  }
  return STATUS_FAIL;
}

int execute(Node *n, int last_status) {
  return run_tree(n, last_status);
}
