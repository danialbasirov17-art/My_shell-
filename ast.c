#include "shell.h"
#include <stdio.h>

static const char *name(NodeKind k) {
  switch (k) {
    case N_SIMPLE: return "simple";
    case N_SUBSHELL: return "subshell";
    case N_PIPE: return "pipeline";
    case N_AND: return "and";
    case N_OR: return "or";
    case N_LIST: return "sequence";
    case N_BG: return "background";
  }
  return "unknown";
}

void print_ast(const Node *n, int depth) {
  if (!n) return;
  for (int i = 0; i < depth; i++) 
    fputs("  ", stdout);
  printf("%s", name(n->kind));
  if (n->kind == N_SIMPLE)
    for (size_t i = 0; i < n->argc; i++)
      printf(" [%s]", n->argv[i]);
  putchar('\n');
  print_ast(n->left, depth + 1);
  print_ast(n->right, depth + 1);
  for (const Redir *r = n->redirs; r; r = r->next) {
    for (int i = 0; i <= depth; i++) 
      fputs("  ", stdout);
    printf("redirect %s [%s]\n", r->kind == R_IN ? "<" : r->kind == R_OUT ? ">" : ">>", r->path);
  }
}
