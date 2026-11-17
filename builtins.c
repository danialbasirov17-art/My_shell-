#include "shell.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum {
  STATUS_FAIL = 1,
  STATUS_USAGE = 2,         
  STATUS_NOT_CHILD = 127,   
  EXIT_MASK = 255,          
  KILL_LIST_COLUMNS = 5
};

static bool request_exit;
static int requested_code;
static bool exit_warned_prev, exit_warned_now;

bool shell_exit_requested(void) { return request_exit; }
int shell_exit_code(void) { return requested_code; }

static const struct { const char *name; int num; } signals[] = {
  {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
  {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"BUS", SIGBUS}, {"FPE", SIGFPE},
  {"KILL", SIGKILL}, {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2},
  {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"CHLD", SIGCHLD},
  {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
  {"TTOU", SIGTTOU}, {"URG", SIGURG}, {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ},
  {"VTALRM", SIGVTALRM}, {"PROF", SIGPROF}, {"WINCH", SIGWINCH}, {"SYS", SIGSYS},
};
#define NSIGNALS (sizeof(signals) / sizeof(signals[0]))

enum { SIGNUM_MAX = 64 };

static int parse_signal(const char *s) {
  if (!*s) return -1;
  if (isdigit((unsigned char)*s)) {
    char *end;
    long n = strtol(s, &end, 10);
    return (*end || n < 0 || n > SIGNUM_MAX) ? -1 : (int)n;
  }
  if (!strncasecmp(s, "SIG", 3)) s += 3;
  for (size_t i = 0; i < NSIGNALS; i++)
    if (!strcasecmp(s, signals[i].name)) return signals[i].num;
  return -1;
}

static const char *signal_name(int num) {
  for (size_t i = 0; i < NSIGNALS; i++)
    if (signals[i].num == num) return signals[i].name;
  return NULL;
}

static bool parse_long(const char *s, long *out) {
  if (!*s) return false;
  char *end;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (*end || errno) return false;
  *out = v;
  return true;
}

static bool exit_blocked(void) {
  if (!jobs_enabled() || !jobs_has_stopped() || exit_warned_prev) return false;
  fprintf(stderr, "mysh: There are stopped jobs.\n");
  exit_warned_now = true;
  return true;
}

void builtin_line_begin(void) {
  exit_warned_prev = exit_warned_now;
  exit_warned_now = false;
}

bool builtin_eof_blocked(void) { return exit_blocked(); }


static int bi_cd(char **a) {
  if (a[1] && a[2]) {
    fprintf(stderr, "mysh: cd: too many arguments\n");
    return STATUS_FAIL;
  }
  const char *d = a[1] ? a[1] : getenv("HOME");
  if (!d) {
    fprintf(stderr, "mysh: cd: HOME not set\n");
    return STATUS_FAIL;
  }
  if (chdir(d) < 0) {
    fprintf(stderr, "mysh: cd: %s: %s\n", d, strerror(errno));
    return STATUS_FAIL;
  }
  char b[PATH_MAX];
  if (getcwd(b, sizeof b)) setenv("PWD", b, 1);
  return 0;
}

static int bi_pwd(void) {
  char b[PATH_MAX];
  if (!getcwd(b, sizeof b)) {
    fprintf(stderr, "mysh: pwd: %s\n", strerror(errno));
    return STATUS_FAIL;
  }
  puts(b);
  return 0;
}

static int bi_echo(char **a) {
  int i = 1;
  bool newline = true;
  if (a[i] && !strcmp(a[i], "-n")) {
    newline = false;
    i++;
  }
  for (bool first = true; a[i]; i++, first = false) {
    if (!first) putchar(' ');
    fputs(a[i], stdout);
  }
  if (newline) putchar('\n');
  return 0;
}

static int bi_help(void) {
  puts("mysh built-in commands:\n"
       "  cd [dir]                change the current directory\n"
       "  pwd                     print the current directory\n"
       "  echo [-n] [args]        print arguments\n"
       "  help                    show this help\n"
       "  exit [code]             leave the shell\n"
       "  jobs [%n ...]           list jobs\n"
       "  fg [%n]                 bring a job to the foreground\n"
       "  bg [%n ...]             resume a stopped job in the background\n"
       "  kill [-s SIG|-SIG] target...   send a signal to a pid or a job (%n, %+, %-)\n"
       "  kill -l                 list signal names\n"
       "  wait [pid|%n ...]       wait for background jobs");
  return 0;
}

static int bi_exit(char **a, int last) {
  int code = last;
  if (a[1]) {
    long v;
    if (!parse_long(a[1], &v)) {
      fprintf(stderr, "mysh: exit: %s: numeric argument required\n", a[1]);
      code = STATUS_USAGE;                
    } else if (a[2]) {
      fprintf(stderr, "mysh: exit: too many arguments\n");
      return STATUS_FAIL;
    } else {
      code = (int)(v & EXIT_MASK);
    }
  }
  if (exit_blocked()) return STATUS_FAIL;
  request_exit = true;
  requested_code = code & EXIT_MASK;
  return requested_code;
}

static int bi_jobs(char **a) {
  if (!a[1]) {
    jobs_print(NULL);
    return 0;
  }
  int rc = 0;
  for (int i = 1; a[i]; i++) {
    Job *j = jobs_find_spec(a[i]);
    if (!j) {
      fprintf(stderr, "mysh: jobs: %s: no such job\n", a[i]);
      rc = STATUS_FAIL;
    } else {
      jobs_print(j);
    }
  }
  return rc;
}

static Job *resolve_job(const char *name, const char *spec) {
  if (!spec) return jobs_current();
  if (spec[0] == '%') return jobs_find_spec(spec);
  char buf[32];
  snprintf(buf, sizeof buf, "%%%s", spec);
  return jobs_find_spec(buf);
  (void)name;
}

static int bi_fg(char **a) {
  if (!jobs_enabled()) {
    fprintf(stderr, "mysh: fg: no job control\n");
    return STATUS_FAIL;
  }
  if (a[1] && a[2]) {
    fprintf(stderr, "mysh: fg: too many arguments\n");
    return STATUS_FAIL;
  }
  Job *j = resolve_job("fg", a[1]);
  if (!j) {
    fprintf(stderr, "mysh: fg: %s: no such job\n", a[1] ? a[1] : "current");
    return STATUS_FAIL;
  }
  if (jobs_state(j) == JOB_DONE) {
    fprintf(stderr, "mysh: fg: job has terminated\n");
    return STATUS_FAIL;
  }
  return jobs_continue_fg(j);
}

static int bi_bg(char **a) {
  if (!jobs_enabled()) {
    fprintf(stderr, "mysh: bg: no job control\n");
    return STATUS_FAIL;
  }
  int rc = 0;
  for (int i = 1; i == 1 || a[i]; i++) {  
    Job *j = resolve_job("bg", a[i]);
    if (!j) {
      fprintf(stderr, "mysh: bg: %s: no such job\n", a[i] ? a[i] : "current");
      rc = STATUS_FAIL;
    } else if (jobs_state(j) == JOB_DONE) {
      fprintf(stderr, "mysh: bg: job has terminated\n");
      rc = STATUS_FAIL;
    } else if (jobs_state(j) == JOB_RUNNING) {
      fprintf(stderr, "mysh: bg: job %d already in background\n", jobs_id(j));
    } else {
      jobs_continue_bg(j);
    }
    if (!a[i]) break;
  }
  return rc;
}

static int kill_list(char **a) {
  if (a[2]) {                             
    int rc = 0;
    for (int i = 2; a[i]; i++) {
      long n;
      if (parse_long(a[i], &n) && signal_name((int)(n & 127)) && n >= 0) {
        puts(signal_name((int)(n & 127)));
      } else if (parse_signal(a[i]) > 0) {
        printf("%d\n", parse_signal(a[i]));
      } else {
        fprintf(stderr, "mysh: kill: %s: invalid signal specification\n", a[i]);
        rc = STATUS_FAIL;
      }
    }
    return rc;
  }
  int col = 0;
  for (int n = 1; n <= SIGNUM_MAX; n++) {
    const char *nm = signal_name(n);
    if (!nm) continue;
    printf("%2d) SIG%-8s%s", n, nm, ++col % KILL_LIST_COLUMNS ? "" : "\n");
  }
  if (col % KILL_LIST_COLUMNS) putchar('\n');
  return 0;
}

static int bi_kill(char **a) {
  static const char usage[] =
      "mysh: kill: usage: kill [-s sigspec | -sigspec] pid | %jobspec ... or kill -l [sigspec]\n";
  int i = 1, sig = SIGTERM;
  if (!a[1]) { fputs(usage, stderr); return STATUS_USAGE; }
  if (!strcmp(a[1], "-l") || !strcmp(a[1], "-L")) return kill_list(a);
  if (!strcmp(a[1], "-s") || !strcmp(a[1], "-n")) {
    if (!a[2]) {
      fprintf(stderr, "mysh: kill: %s: option requires an argument\n", a[1]);
      return STATUS_USAGE;
    }
    sig = parse_signal(a[2]);
    if (sig < 0) {
      fprintf(stderr, "mysh: kill: %s: invalid signal specification\n", a[2]);
      return STATUS_FAIL;
    }
    i = 3;
  } else if (!strcmp(a[1], "--")) {
    i = 2;
  } else if (a[1][0] == '-' && a[1][1]) {
    sig = parse_signal(a[1] + 1);
    if (sig < 0) {
      fprintf(stderr, "mysh: kill: %s: invalid signal specification\n", a[1] + 1);
      return STATUS_FAIL;
    }
    i = 2;
  }
  if (!a[i]) { fputs(usage, stderr); return STATUS_USAGE; }

  int rc = 0;
  for (; a[i]; i++) {
    if (a[i][0] == '%') {
      Job *j = jobs_find_spec(a[i]);
      if (!j) {
        fprintf(stderr, "mysh: kill: %s: no such job\n", a[i]);
        rc = STATUS_FAIL;
      } else if (jobs_signal(j, sig) < 0) {
        fprintf(stderr, "mysh: kill: %s: %s\n", a[i], strerror(errno));
        rc = STATUS_FAIL;
      }
      continue;
    }
    long pid;
    if (!parse_long(a[i], &pid)) {
      fprintf(stderr, "mysh: kill: %s: arguments must be process or job IDs\n", a[i]);
      rc = STATUS_FAIL;
    } else if (kill((pid_t)pid, sig) < 0) {
      fprintf(stderr, "mysh: kill: (%ld) - %s\n", pid, strerror(errno));
      rc = STATUS_FAIL;
    }
  }
  return rc;
}

static int bi_wait(char **a) {
  if (!a[1]) return jobs_wait_all();
  int rc = 0;
  for (int i = 1; a[i]; i++) {
    if (a[i][0] == '%') {
      Job *j = jobs_find_spec(a[i]);
      if (!j) {
        fprintf(stderr, "mysh: wait: %s: no such job\n", a[i]);
        rc = STATUS_NOT_CHILD;
      } else {
        rc = jobs_wait_job(j);
      }
      continue;
    }
    long pid;
    if (!parse_long(a[i], &pid) || pid <= 0) {
      fprintf(stderr, "mysh: wait: `%s': not a pid or valid job spec\n", a[i]);
      rc = STATUS_FAIL;
      continue;
    }
    int st = 0;
    int r = jobs_wait_pid((pid_t)pid, &st);
    if (r == -1) {
      fprintf(stderr, "mysh: wait: pid %ld is not a child of this shell\n", pid);
      rc = STATUS_NOT_CHILD;
    } else if (r == -2) {
      rc = 128 + SIGINT;
    } else {
      rc = st;
    }
  }
  return rc;
}


static const char *const names[] = {"cd", "pwd", "echo", "help", "exit",
                                    "jobs", "fg", "bg", "kill", "wait"};

bool is_builtin(const char *name) {
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (!strcmp(name, names[i])) return true;
  return false;
}

int run_builtin(char **a, int last) {
  const char *n = a[0];
  if (!strcmp(n, "cd")) return bi_cd(a);
  if (!strcmp(n, "pwd")) return bi_pwd();
  if (!strcmp(n, "echo")) return bi_echo(a);
  if (!strcmp(n, "help")) return bi_help();
  if (!strcmp(n, "exit")) return bi_exit(a, last);
  if (!strcmp(n, "jobs")) return bi_jobs(a);
  if (!strcmp(n, "fg")) return bi_fg(a);
  if (!strcmp(n, "bg")) return bi_bg(a);
  if (!strcmp(n, "kill")) return bi_kill(a);
  if (!strcmp(n, "wait")) return bi_wait(a);
  return STATUS_FAIL;
}
