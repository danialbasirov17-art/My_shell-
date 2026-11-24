#include "shell.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { STATUS_SYNTAX = 2 };

typedef enum { DUMP_NONE, DUMP_TOKENS, DUMP_AST } DumpMode;
typedef enum { IN_DONE, IN_MORE, IN_MORE_BACKSLASH, IN_ERROR } InputState;

static const char PROMPT[] = "mysh$ "; 
static const char PROMPT2[] = "> ";
static const char CONTINUATION[] = "\\\n";

// печатает допустимый формат запуска 
static int usage(void) {
  fprintf(stderr, "mysh: usage: mysh [--dump-tokens|--dump-ast] [-c command]\n");
  return STATUS_SYNTAX;
}

// разбирает аргументы командной строки, записывая выбранный режим дампа и указатель на строку после -с
static int options(int argc, char **argv, const char **command, DumpMode *dump) {
  *command = NULL;
  *dump = DUMP_NONE;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dump-tokens") && *dump == DUMP_NONE) *dump = DUMP_TOKENS;
    else if (!strcmp(argv[i], "--dump-ast") && *dump == DUMP_NONE) *dump = DUMP_AST;
    else if (!strcmp(argv[i], "-c") && i + 1 < argc && !*command) *command = argv[++i];
    else return usage();
  }
  return 0;
}

// печатает текст слова в двойных кавычках, экранируя " и \
static void print_word_escaped(const char *s) {
  putchar('"');
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') printf("\\%c", c);
    else if (c == '\n') fputs("\\n", stdout);
    else if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
    else putchar(c);
  }
  putchar('"');
}

// проходит по массиву токенов и печатает имя типа каждого токена
static void dump_tokens(const Tokens *t) {
  for (size_t i = 0; i < t->n; i++) {
    fputs(token_name(t->v[i].kind), stdout);
    if (t->v[i].kind == T_WORD) {
      putchar(' ');
      print_word_escaped(t->v[i].text);
    }
    putchar('\n');
  }
}

// печатает сообщение об ошибке, устанавливая STATUS_SYNTAX
static InputState syntax_error(const char *msg, int *status) {
  fprintf(stderr, "mysh: %s\n", msg);
  *status = STATUS_SYNTAX;
  return IN_ERROR;
}

// пробует разобрать накопленный текст
static InputState process(const char *text, DumpMode dump, bool at_eof, int *status) {
  Tokens tokens;
  char *err = NULL;
  int lr = lex_line(text, &tokens, &err);
  if (lr != LEX_OK) {
    InputState st = IN_MORE;
    if (lr == LEX_CONTINUE) st = IN_MORE_BACKSLASH;
    else if (lr == LEX_ERROR) st = IN_ERROR;
    if (st == IN_MORE_BACKSLASH || (st == IN_MORE && !at_eof)) {
      free(err);
      return st;
    }
    char msg[256];
    snprintf(msg, sizeof msg, "syntax error: %s", err ? err : "lexical error");
    free(err);
    return syntax_error(msg, status);
  }
  if (dump == DUMP_TOKENS) {
    dump_tokens(&tokens);
    free_tokens(&tokens);
    return IN_DONE;
  }

  Node *tree = NULL;
  int pr = parse_tokens(&tokens, &tree, &err);
  free_tokens(&tokens);
  if (pr == PARSE_INCOMPLETE) {
    if (!at_eof) return IN_MORE;
    return syntax_error("syntax error: unexpected end of file", status);
  }
  if (pr != PARSE_OK) {
    char msg[256];
    snprintf(msg, sizeof msg, "%s", err ? err : "syntax error");
    free(err);
    return syntax_error(msg, status);
  }
  if (dump == DUMP_AST) {
    if (tree) print_ast(tree, 0); else puts("(empty)");
  } else if (tree) {
    *status = execute(tree, *status);
  }
  free_node(tree);
  return IN_DONE;
}

// при конце ввода вызывает process() с признаком at_eof=true
static InputState process_at_eof(char *text, size_t len, DumpMode dump, int *status) {
  size_t cl = sizeof(CONTINUATION) - 1;
  for (;;) {
    InputState st = process(text, dump, true, status);
    if (st != IN_MORE_BACKSLASH) return st;
    len -= cl;
    text[len] = '\0';
  }
}

int main(int argc, char **argv) {
  const char *command;
  DumpMode dump;
  int rc = options(argc, argv, &command, &dump);
  if (rc) return rc;

  bool interactive = isatty(STDIN_FILENO) && !command;
  jobs_init(interactive);

  char *line = NULL, *pending = NULL;
  size_t line_cap = 0, pending_len = 0, pending_cap = 0;
  int status = 0;
  bool syntax_failed = false;

  if (command) {
    size_t len = strlen(command);
    char *copy = strdup(command);
    if (!copy) {
      fprintf(stderr, "mysh: out of memory\n");
      return 1;
    }
    syntax_failed = process_at_eof(copy, len, dump, &status) == IN_ERROR;
    free(copy);
  }

  while (!command && !shell_exit_requested() && !(syntax_failed && !interactive)) {
    jobs_notify(interactive);
    if (interactive) {
      fputs(pending_len ? PROMPT2 : PROMPT, stderr);
      fflush(stderr);
    }
    errno = 0;
    ssize_t n = getline(&line, &line_cap, stdin);
    if (n < 0) {
      if (interactive && errno == EINTR) {   
        clearerr(stdin);
        pending_len = 0;
        continue;
      }
      builtin_line_begin();
      if (pending_len) {
        syntax_failed = process_at_eof(pending, pending_len, dump, &status) == IN_ERROR;
        pending_len = 0;
      }
      if (interactive) {
        if (builtin_eof_blocked()) { 
          clearerr(stdin);
          continue;
        }
        fputs("exit\n", stderr);
      }
      break;
    }
    builtin_line_begin();
    if (pending_len + (size_t)n + 1 > pending_cap) {
      size_t cap = pending_cap ? pending_cap : 256;
      while (cap < pending_len + (size_t)n + 1) cap *= 2;
      char *p = realloc(pending, cap);
      if (!p) {
        fprintf(stderr, "mysh: out of memory\n");
        status = 1;
        break;
      }
      pending = p;
      pending_cap = cap;
    }
    memcpy(pending + pending_len, line, (size_t)n + 1);
    pending_len += (size_t)n;

    InputState st = process(pending, dump, false, &status);
    if (st == IN_MORE || st == IN_MORE_BACKSLASH) continue;
    pending_len = 0;
    if (st == IN_ERROR) syntax_failed = true;
  }

  free(line);
  free(pending);
  if (shell_exit_requested()) status = shell_exit_code();
  jobs_shutdown();
  return status;
}
