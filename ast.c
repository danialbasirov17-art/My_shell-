#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// делает отступ
static void indent(int depth) {
  for (int i = 0; i < depth; i++) 
    fputs("  ", stdout);
}

// заключает текст в двойные кавычки
static void print_word(const char *s) {
  putchar('"');
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') 
      printf("\\%c", c);
    else if (c == '\n') 
      fputs("\\n", stdout);
    else if (c == '\t') 
      fputs("\\t", stdout);
    else if (c < 0x20 || c == 0x7f) 
      printf("\\x%02x", c);
    else putchar(c);
  }
  putchar('"');
}

// переводит R_IN, R_OUT, R_APPEND в >, <, >> 
static const char *redir_sym(RedirKind k) {
  return k == R_IN ? "<" : k == R_OUT ? ">" : ">>";
}

// печатает каждое перенаправление отдельной строкой
static void print_redirs(const Redir *r, int depth) {
  for (; r; r = r->next) {
    indent(depth);
    printf("redirect %s ", redir_sym(r->kind));
    print_word(r->path);
    putchar('\n');
  }
}

// разворачивает цепочку N_PIPE и печатает стадии конвейера подряд
static void print_stages(const Node *n, int depth) {
  if (n->kind == N_PIPE) {
    print_stages(n->left, depth);
    print_stages(n->right, depth);
  } else {
    print_ast(n, depth);
  }
}

// рекурсивное печатание узла и его дочерних элементов
void print_ast(const Node *n, int depth) {
  if (!n) return;
  indent(depth);
  switch (n->kind) {
    case N_SIMPLE:
      fputs("simple", stdout);
      for (size_t i = 0; i < n->argc; i++) {
        putchar(' ');
        print_word(n->argv[i]);
      }
      putchar('\n');
      print_redirs(n->redirs, depth + 1);
      break;
    case N_SUBSHELL:
      puts("subshell ( )");
      print_ast(n->left, depth + 1);
      print_redirs(n->redirs, depth + 1);
      break;
    case N_PIPE:
      puts("pipeline |");
      print_stages(n, depth + 1);
      break;
    case N_AND:
      puts("and &&");
      print_ast(n->left, depth + 1);
      print_ast(n->right, depth + 1);
      break;
    case N_OR:
      puts("or ||");
      print_ast(n->left, depth + 1);
      print_ast(n->right, depth + 1);
      break;
    case N_LIST:
      puts("sequence ;");
      print_ast(n->left, depth + 1);
      print_ast(n->right, depth + 1);
      break;
    case N_BG:
      puts("background &");
      print_ast(n->left, depth + 1);
      break;
  }
}


typedef struct { char *v; size_t n, cap; bool oom; } Str;

// добавление в буфер
static void put(Str *s, const char *t) {
  size_t len = strlen(t);
  if (s->oom) return;
  if (s->n + len + 1 > s->cap) {
    size_t cap = s->cap ? s->cap : 64;
    while (cap < s->n + len + 1) cap *= 2;
    char *p = realloc(s->v, cap);
    if (!p) { s->oom = true; return; }
    s->v = p;
    s->cap = cap;
  }
  memcpy(s->v + s->n, t, len + 1);
  s->n += len;
}

// выбор записи элемента
static void put_word(Str *s, const char *w) {
  static const char safe[] = "_-./=:,+@%^";
  bool plain = *w != '\0';
  for (const char *c = w; *c && plain; c++)
    if (!(((*c | 0x20) >= 'a' && (*c | 0x20) <= 'z') || (*c >= '0' && *c <= '9') ||
          strchr(safe, *c)))
      plain = false;
  if (plain) { 
    put(s, w); 
    return;
  }
  put(s, "'");
  for (const char *c = w; *c; c++) {
    if (*c == '\'') 
      put(s, "'\\''");
    else { 
      char one[2] = {*c, 0};
      put(s, one); 
    }
  }
  put(s, "'");
}

// добавление перенаправления 
static void put_redirs(Str *s, const Redir *r) {
  for (; r; r = r->next) {
    put(s, " ");
    put(s, redir_sym(r->kind));
    put(s, " ");
    put_word(s, r->path);
  }
}

// рекурсивный обход узлов
static void unparse(Str *s, const Node *n) {
  if (!n) return;
  switch (n->kind) {
    case N_SIMPLE:
      for (size_t i = 0; i < n->argc; i++) {
        if (i) put(s, " ");
        put_word(s, n->argv[i]);
      }
      put_redirs(s, n->redirs);
      break;
    case N_SUBSHELL:
      put(s, "( ");
      unparse(s, n->left);
      put(s, " )");
      put_redirs(s, n->redirs);
      break;
    case N_PIPE: 
      unparse(s, n->left); 
      put(s, " | "); 
      unparse(s, n->right); 
      break;
    case N_AND:  
      unparse(s, n->left);
      put(s, " && "); 
      unparse(s, n->right); 
      break;
    case N_OR:   
      unparse(s, n->left); 
      put(s, " || "); 
      unparse(s, n->right); 
      break;
    case N_LIST:
      unparse(s, n->left);
      put(s, n->left->kind == N_BG ? " " : "; ");  
      unparse(s, n->right);
      break;
    case N_BG: 
      unparse(s, n->left); 
      put(s, " &"); 
      break;
  }
}

// создание начального буфера, запуск обхода, возврат готовой строки
char *ast_to_string(const Node *n) {
  Str s = {0};
  unparse(&s, n);
  if (s.oom) { 
    free(s.v); 
    return NULL; 
  }
  return s.v ? s.v : strdup("");
}
