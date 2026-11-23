#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

enum {
  MAX_JOBS = 1024,
  TTY_FD_MIN = 10,       
  STATUS_WIDTH = 24,     
  EXIT_SIGNAL_BASE = 128 
};

typedef enum { PROC_RUNNING, PROC_STOPPED, PROC_DONE } ProcState;
typedef struct { pid_t pid; ProcState state; int wstatus; } Proc;

struct Job {
  int id;                
  pid_t pgid;
  Proc *procs;
  size_t nprocs;
  char *cmd;
  JobState state;
  bool background;
  bool notified;         
  bool has_tmodes;
  struct termios tmodes; 
};

static Job *table[MAX_JOBS];
static int cur_id, prev_id; 
static bool job_control;
static int tty_fd = -1;
static pid_t shell_pgid;
static struct termios shell_tmodes;
static volatile sig_atomic_t sigint_seen;

// отмечает, что shell получил SIGINT и пишет перевод строки через write()
static void on_sigint(int sig) {
  int saved = errno;
  (void)sig;
  sigint_seen = 1;
  if (write(STDOUT_FILENO, "\n", 1) < 0) { }
  errno = saved;
}

// устанавливает обработчик с sigaction()
static void set_handler(int sig, void (*h)(int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = h;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;                        
  sigaction(sig, &sa, NULL);
}

// возвращает дочерним процессам стандартное поведение для сигналов
static void default_signals(void) {
  static const int sigs[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU};
  for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) set_handler(sigs[i], SIG_DFL);
}

// передаёт терминал указанной группе процессов
static void give_terminal(pid_t pgid) {
  if (!job_control) return;
  while (tcsetpgrp(tty_fd, pgid) < 0 && errno == EINTR) { }
}

// включает управление заданиями, если shell в интерактивном режиме
void jobs_init(bool interactive) {
  if (!interactive) return;
  tty_fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, TTY_FD_MIN);
  if (tty_fd < 0) { perror("mysh: tty"); return; }

  set_handler(SIGTTIN, SIG_DFL);
  while (tcgetpgrp(tty_fd) != (shell_pgid = getpgrp())) kill(-shell_pgid, SIGTTIN);

  set_handler(SIGINT, on_sigint);
  set_handler(SIGQUIT, SIG_IGN);
  set_handler(SIGTSTP, SIG_IGN);
  set_handler(SIGTTIN, SIG_IGN);
  set_handler(SIGTTOU, SIG_IGN);          

  if (setpgid(0, 0) < 0 && getpgrp() != getpid()) { 
      perror("mysh: setpgid"); 
      goto fail;
  }
  shell_pgid = getpgrp();
  if (tcsetpgrp(tty_fd, shell_pgid) < 0) { 
      perror("mysh: tcsetpgrp");
      goto fail; 
  }
  if (tcgetattr(tty_fd, &shell_tmodes) < 0) { 
      perror("mysh: tcgetattr");
      goto fail;
  }
  job_control = true;
  return;
fail:
  close(tty_fd);
  tty_fd = -1;
}

// сообщает, удалось ли включить управление
bool jobs_enabled(void) { 
    return job_control;
}

// освобождает массив процессов, строку команды и саму структуру
static void job_free(Job *j) {
  free(j->procs);
  free(j->cmd);
  free(j);
}

// 
static Job *by_id(int id) { 
    return id >= 1 && id <= MAX_JOBS ? table[id - 1] : NULL;
}

// делает указанное задание текущим
static void set_current(const Job *j) {
  if (cur_id == j->id) return;
  prev_id = cur_id;
  cur_id = j->id;
}

//
static int newest_except(int except) {
  for (int id = MAX_JOBS; id >= 1; id--)
    if (table[id - 1] && id != except) return id;
  return 0;
}

// выбирает актуальные задания, если прежнее было удалено 
static void fix_current(void) {
  if (!by_id(cur_id)) cur_id = 0;
  if (!by_id(prev_id)) prev_id = 0;
  if (!cur_id) { cur_id = prev_id; prev_id = 0; }
  if (!cur_id) cur_id = newest_except(0);
  if (prev_id == cur_id) prev_id = 0;
  if (!prev_id) prev_id = newest_except(cur_id);
}

// убирает задания из таблицы
static void job_remove(Job *j) {
  table[j->id - 1] = NULL;
  job_free(j);
  fix_current();
}

// находит свободный слот и создаёт Job
Job *jobs_new(const char *cmd, bool background) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (table[i]) continue;           
    Job *j = calloc(1, sizeof(*j));
    if (!j) break;
    j->cmd = strdup(cmd ? cmd : "");
    if (!j->cmd) { 
        free(j);
        break;
    }
    j->id = i + 1;
    j->state = JOB_RUNNING;
    j->background = background;
    table[i] = j;
    return j;
  }
  fprintf(stderr, "mysh: cannot create job: %s\n", errno == ENOMEM ? "out of memory" : "too many jobs");
  return NULL;
}

// добавляет процесс
int jobs_add_proc(Job *j, pid_t pid) {
  Proc *p = realloc(j->procs, (j->nprocs + 1) * sizeof(*p));
  if (!p) return -1;
  j->procs = p;
  j->procs[j->nprocs++] = (Proc){pid, PROC_RUNNING, 0};
  if (j->nprocs == 1) 
      j->pgid = pid;
  if (job_control) 
      setpgid(pid, j->pgid); 
  return 0;
}

// возвращает pgid
pid_t jobs_pgid(const Job *j) { 
    return j->nprocs ? j->pgid : 0;
}

// удаляет задание
void jobs_discard(Job *j) { 
    job_remove(j); 
}

// возвращает id
int jobs_id(const Job *j) { 
    return j->id; 
}

// возвращает состояние задания
JobState jobs_state(const Job *j) {
    return j->state;
}

// возвращает id текущего задания 
Job *jobs_current(void) { 
    return by_id(cur_id);
}

// вызывается в потомке сразу после fork()
void jobs_child_start(pid_t pgid, bool foreground) {
  if (job_control) {
    if (pgid == 0) pgid = getpid();
    setpgid(0, pgid);
    if (foreground) tcsetpgrp(tty_fd, pgid);
  }
  default_signals();
  job_control = false;
  if (tty_fd >= 0) { 
      close(tty_fd);
      tty_fd = -1; 
  }
  for (int i = 0; i < MAX_JOBS; i++)
    if (table[i]) { 
        job_free(table[i]);
        table[i] = NULL;
    }
  cur_id = prev_id = 0;
}

// пересчитывает состояние job по его процессам
static void recompute(Job *j) {
  bool running = false, stopped = false;
  for (size_t i = 0; i < j->nprocs; i++) {
    if (j->procs[i].state == PROC_RUNNING) running = true;
    else if (j->procs[i].state == PROC_STOPPED) stopped = true;
  }
  j->state = running ? JOB_RUNNING : stopped ? JOB_STOPPED : JOB_DONE;
}

// возвращает код завершения процесса
static int proc_code(const Proc *p) {
  int w = p->wstatus;
  if (p->state == PROC_STOPPED) return EXIT_SIGNAL_BASE + WSTOPSIG(w);
  if (WIFEXITED(w)) return WEXITSTATUS(w);
  if (WIFSIGNALED(w)) return EXIT_SIGNAL_BASE + WTERMSIG(w);
  return 0;
}

// берёт статус последнего процесса задания
static int job_code(const Job *j) {
  return j->nprocs ? proc_code(&j->procs[j->nprocs - 1]) : 0;
}

// ищет pid в таблице заданий
static Job *find_proc(pid_t pid, Proc **out) {
  for (int i = 0; i < MAX_JOBS; i++) {
    Job *j = table[i];
    if (!j) continue;
    for (size_t k = 0; k < j->nprocs; k++)
      if (j->procs[k].pid == pid) { *out = &j->procs[k]; return j; }
  }
  return NULL;
}

// ждёт завершение или изменение состояния одного дочернего процесса
static pid_t reap_one(bool block) {
  int st, flags = job_control ? WUNTRACED | WCONTINUED : 0;
  pid_t p;
  if (!block) flags |= WNOHANG;
  do {
    p = waitpid(-1, &st, flags);
  } while (p < 0 && errno == EINTR && !block);
  if (p <= 0) return p;

  Proc *pr = NULL;
  Job *j = find_proc(p, &pr);
  if (!j) return p;                 
  JobState before = j->state;
  if (WIFSTOPPED(st)) { 
    pr->state = PROC_STOPPED;
    pr->wstatus = st;
  }
  else if (WIFCONTINUED(st)) pr->state = PROC_RUNNING;
  else { 
    pr->state = PROC_DONE;
    pr->wstatus = st;
  }
  recompute(j);
  if (j->state == JOB_STOPPED && before != JOB_STOPPED) {
    j->notified = false;
    set_current(j);
  }
  return p;
}

// собирает все уже готовые события
static void update_nohang(void) {
  while (reap_one(false) > 0) { }
}

// помечает все оставшиеся процессы как завершённые, когда дочерних процессов уже нет
static void forget_running(void) {
  for (int i = 0; i < MAX_JOBS; i++) {
    Job *j = table[i];
    if (!j) continue;
    for (size_t k = 0; k < j->nprocs; k++)
      if (j->procs[k].state == PROC_RUNNING) { j->procs[k].state = PROC_DONE; j->procs[k].wstatus = 0; }
    recompute(j);
  }
}

typedef bool (*Pending)(const void *);
static bool job_running(const void *a) { 
    return ((const Job *)a)->state == JOB_RUNNING;
}
static bool proc_running(const void *a) { 
    return ((const Proc *)a)->state == PROC_RUNNING;
}
static bool any_bg_running(const void *a) {
  (void)a;
  for (int i = 0; i < MAX_JOBS; i++)
    if (table[i] && table[i]->background && table[i]->state == JOB_RUNNING) return true;
  return false;
}

// 
static int wait_for(Pending pending, const void *arg, bool interruptible) {
  sigint_seen = 0;
  while (pending(arg)) {
    pid_t p = reap_one(true);
    if (p > 0) continue;
    if (errno == EINTR) {
      if (interruptible && sigint_seen) return -1;
      continue;
    }
    forget_running();
    break;
  }
  return 0;
}

// формирует текст состояния
static void state_text(const Job *j, char *buf, size_t n) {
  switch (j->state) {
    case JOB_RUNNING: snprintf(buf, n, "Running"); break;
    case JOB_STOPPED: snprintf(buf, n, "Stopped"); break;
    case JOB_DONE: {
      const Proc *last = &j->procs[j->nprocs ? j->nprocs - 1 : 0];
      if (j->nprocs && WIFSIGNALED(last->wstatus)) 
          snprintf(buf, n, "%s", strsignal(WTERMSIG(last->wstatus)));
      else if (job_code(j) != 0) 
          snprintf(buf, n, "Exit %d", job_code(j));
      else snprintf(buf, n, "Done");
      break;
    }
  }
}

// печатает номер, метку +/'-', состояние и сохраняет текст команды
static void print_job(FILE *f, const Job *j) {
  char st[64];
  char mark = j->id == cur_id ? '+' : j->id == prev_id ? '-' : ' ';
  state_text(j, st, sizeof st);
  fprintf(f, "[%d]%c  %-*s%s%s\n", j->id, mark, STATUS_WIDTH, st, j->cmd,
          j->state == JOB_RUNNING && j->background ? " &" : "");
}

// обновляет состояния, печатает все задания или выбранное, а завершённое после печать удаляет
void jobs_print(Job *only) {
  update_nohang();
  for (int i = 0; i < MAX_JOBS; i++) {
    Job *j = table[i];
    if (!j || (only && j != only)) continue;
    print_job(stdout, j);
    j->notified = true;
    if (j->state == JOB_DONE) job_remove(j);
  }
}

// печатает сообщения и удаляет завершённые задания
void jobs_notify(bool print) {
  update_nohang();
  for (int i = 0; i < MAX_JOBS; i++) {
    Job *j = table[i];
    if (!j) continue;
    if (j->state == JOB_DONE) {
      if (print && j->background && !j->notified) 
          print_job(stderr, j);
      job_remove(j);
    } else if (j->state == JOB_STOPPED && !j->notified) {
      if (print) print_job(stderr, j);
      j->notified = true;
    }
  }
}

// переводит остальные процессы в состояние PROC_RUNNING
static void mark_running(Job *j) {
  for (size_t i = 0; i < j->nprocs; i++)
    if (j->procs[i].state == PROC_STOPPED) j->procs[i].state = PROC_RUNNING;
  recompute(j);
  j->notified = false;
}

// ждёт, пока задание не перестанет быть работающим
static int finish_fg(Job *j) {
  wait_for(job_running, j, false);
  if (job_control) {
    give_terminal(shell_pgid);
    if (j->state == JOB_STOPPED && tcgetattr(tty_fd, &j->tmodes) == 0) j->has_tmodes = true;
    tcsetattr(tty_fd, TCSADRAIN, &shell_tmodes);
  }
  int code = job_code(j);
  if (j->state == JOB_STOPPED) {
    set_current(j);
    j->notified = true;
    if (job_control) {
      fputc('\n', stderr);
      print_job(stderr, j);
    }
    return code;
  }
  if (job_control && j->nprocs && WIFSIGNALED(j->procs[j->nprocs - 1].wstatus)) {
    int sig = WTERMSIG(j->procs[j->nprocs - 1].wstatus);
    if (sig == SIGINT) fputc('\n', stderr);
    else if (sig != SIGPIPE) fprintf(stderr, "%s\n", strsignal(sig));
  }
  job_remove(j);
  return code;
}

// помечает задание как преднеплановое, печатает ему терминал и ждёт через finish_fg()
int jobs_run_fg(Job *j) {
  j->background = false;
  give_terminal(j->pgid);
  return finish_fg(j);
}

// помечает job как фоновую, делает текущей, печатает её номер и pid
int jobs_run_bg(Job *j) {
  j->background = true;
  set_current(j);
  if (job_control) fprintf(stderr, "[%d] %ld\n", j->id, (long)j->procs[j->nprocs - 1].pid);
  return 0;
}

// отправляет сигнал всей группе для группы процессов
int jobs_signal(Job *j, int sig) {
  int rc = 0;
  if (job_control) {
    rc = kill(-j->pgid, sig);
  } else {
    for (size_t i = 0; i < j->nprocs; i++)
      if (j->procs[i].state != PROC_DONE && kill(j->procs[i].pid, sig) < 0) rc = -1;
  }
  if (rc == 0 && j->state == JOB_STOPPED && (sig == SIGTERM || sig == SIGHUP)) {
    if (job_control) kill(-j->pgid, SIGCONT);
    else for (size_t i = 0; i < j->nprocs; i++) kill(j->procs[i].pid, SIGCONT);
  }
  return rc;
}

// печатает команду, возвращает ей терминал, восстанавливает её настройки терминала
int jobs_continue_fg(Job *j) {
  j->background = false;
  printf("%s\n", j->cmd);
  fflush(stdout);
  give_terminal(j->pgid);
  if (j->state == JOB_STOPPED && j->has_tmodes)
      tcsetattr(tty_fd, TCSADRAIN, &j->tmodes);
  mark_running(j);
  kill(-j->pgid, SIGCONT);
  return finish_fg(j);
}

// печатает job фоновой и делает её текущей
int jobs_continue_bg(Job *j) {
  j->background = true;
  mark_running(j);
  set_current(j);
  printf("[%d]%c %s &\n", j->id, j->id == cur_id ? '+' : j->id == prev_id ? '-' : ' ', j->cmd);
  fflush(stdout);
  kill(-j->pgid, SIGCONT);
  return 0;
}

// принимает строки, начинающиеся с %
Job *jobs_find_spec(const char *spec) {
  if (!spec || spec[0] != '%') return NULL;
  const char *s = spec + 1;
  if (!*s || !strcmp(s, "%") || !strcmp(s, "+")) return by_id(cur_id);
  if (!strcmp(s, "-")) return by_id(prev_id);
  char *end;
  long id = strtol(s, &end, 10);
  if (*end || id < 1 || id > MAX_JOBS) return NULL;
  return by_id((int)id);
}

// проверяет, есть ли в таблице остановленные задания
bool jobs_has_stopped(void) {
  for (int i = 0; i < MAX_JOBS; i++)
    if (table[i] && table[i]->state == JOB_STOPPED) return true;
  return false;
}

// ждёт фоновые работающие задания
int jobs_wait_all(void) {
  if (wait_for(any_bg_running, NULL, true) < 0) return EXIT_SIGNAL_BASE + SIGINT;
  for (int i = 0; i < MAX_JOBS; i++)
    if (table[i] && table[i]->state == JOB_DONE) job_remove(table[i]);
  return 0;
}

// ждёт конкретный job
int jobs_wait_job(Job *j) {
  if (wait_for(job_running, j, true) < 0) return EXIT_SIGNAL_BASE + SIGINT;
  int code = job_code(j);
  if (j->state == JOB_DONE) job_remove(j);
  return code;
}

// ищет pid в таблице и ждёт именно этот процесс
int jobs_wait_pid(pid_t pid, int *status) {
  Proc *pr = NULL;
  Job *j = find_proc(pid, &pr);
  if (!j) return -1;
  if (wait_for(proc_running, pr, true) < 0) return -2;
  *status = proc_code(pr);
  if (j->state == JOB_DONE) job_remove(j);
  return 0;
}

// очищение структур shell при завершении
void jobs_shutdown(void) {
  update_nohang();
  for (int i = 0; i < MAX_JOBS; i++) {
    Job *j = table[i];
    if (!j) continue;
    if (job_control && j->state == JOB_STOPPED) {  
      kill(-j->pgid, SIGHUP);
      kill(-j->pgid, SIGCONT);
    }
  }
  update_nohang();
  for (int i = 0; i < MAX_JOBS; i++)
    if (table[i]) { 
        job_free(table[i]);
        table[i] = NULL;
    }
  if (tty_fd >= 0) { 
      close(tty_fd);
      tty_fd = -1; 
  }
}
