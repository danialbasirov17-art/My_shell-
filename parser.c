#include "shell.h"
#include <stdlib.h>
#include <string.h>

enum { MAX_DEPTH = 256 };

typedef struct {
  const Tokens *t;
  size_t p;
  char **err;
  bool incomplete;
  int depth;
} Parser;

static Node *list(Parser *p);

static TokenKind peek(const Parser *p) { 
    return p->t->v[p->p].kind; 
}

static void fail(Parser *p, const char *msg) {
  if (!*p->err && !p->incomplete) 
      *p->err = strdup(msg);
}

static void fail_token(Parser *p) {
  if (*p->err || p->incomplete) return;
  const Token *t = &p->t->v[p->p];
  const char *what = t->kind == T_WORD ? t->text : token_symbol(t->kind);
  const char *fmt = "syntax error near unexpected token `";
  size_t n = strlen(fmt) + strlen(what) + 2;
  char *m = malloc(n);
  if (!m) return;
  strcpy(m, fmt);
  strcat(m, what);
  strcat(m, "'");
  *p->err = m;
}

static void skip_nl(Parser *p) {
  while (peek(p) == T_NL) p->p++;
}

static Node *node(NodeKind k, Node *l, Node *r) {
  Node *n = calloc(1, sizeof(*n));
  if (n) {
    n->kind = k;
    n->left = l;
    n->right = r;
  }
  return n;
}

static Node *make(Parser *p, NodeKind k, Node *l, Node *r) {
  Node *n = node(k, l, r);
  if (!n) {
    free_node(l);
    free_node(r);
    fail(p, "out of memory");
  }
  return n;
}

void free_node(Node *n) {
  if (!n) return;
  free_node(n->left);
  free_node(n->right);
  for (size_t i = 0; i < n->argc; i++) 
      free(n->argv[i]);
  free(n->argv);
  for (Redir *r = n->redirs; r;) {
    Redir *q = r->next;
    free(r->path);
    free(r);
    r = q;
  }
  free(n);
}

static int addword(Node *n, const char *s) {
  char **v = realloc(n->argv, (n->argc + 2) * sizeof(*v));
  if (!v) return -1;
  n->argv = v;
  n->argv[n->argc] = strdup(s);
  if (!n->argv[n->argc]) return -1;
  n->argc++;
  n->argv[n->argc] = NULL;
  return 0;
}

static bool is_redir_op(TokenKind k) { return k == T_IN || k == T_OUT || k == T_APPEND; }

static int redir(Node *n, Parser *p) {
  TokenKind k = peek(p);
  p->p++;
  if (peek(p) != T_WORD) {
    fail_token(p);
    return -1;
  }
  Redir *r = calloc(1, sizeof(*r));
  if (!r) { 
      fail(p, "out of memory");
      return -1; 
  }
  r->kind = k == T_IN ? R_IN : k == T_OUT ? R_OUT : R_APPEND;
  r->path = strdup(p->t->v[p->p++].text);
  if (!r->path) { 
      free(r); 
      fail(p, "out of memory"); 
      return -1; 
  }
  Redir **q = &n->redirs;
  while (*q) q = &(*q)->next;
  *q = r;
  return 0;
}

static Node *subshell(Parser *p) {
  p->p++;                                  
  if (++p->depth > MAX_DEPTH) { 
      fail(p, "too many nested subshells"); 
      return NULL; 
  }
  skip_nl(p);
  Node *body = list(p);
  p->depth--;
  if (!body) return NULL;
  if (peek(p) != T_RP) {
    free_node(body);
    if (peek(p) == T_END) p->incomplete = true; else fail_token(p);
    return NULL;
  }
  p->p++;
  Node *n = make(p, N_SUBSHELL, body, NULL);
  if (!n) return NULL;
  while (is_redir_op(peek(p)))
    if (redir(n, p) < 0) { free_node(n); return NULL; }
  return n;
}

static Node *command(Parser *p) {
  if (peek(p) == T_LP) return subshell(p);

  Node *n = node(N_SIMPLE, NULL, NULL);
  if (!n) { 
      fail(p, "out of memory"); 
      return NULL; }
  for (;;) {
    TokenKind k = peek(p);
    if (k == T_WORD) {
      if (addword(n, p->t->v[p->p].text)) { 
          free_node(n); 
          fail(p, "out of memory"); 
          return NULL; 
      }
      p->p++;
    } else if (is_redir_op(k)) {
      if (redir(n, p) < 0) { 
          free_node(n); 
          return NULL; 
      }
    } else break;
  }
  if (n->argc == 0) {
    bool had_redir = n->redirs != NULL;
    free_node(n);
    if (peek(p) == T_END && !had_redir) 
        p->incomplete = true;
    else if (peek(p) == T_END) 
        fail(p, "syntax error: expected command");
    else fail_token(p);
    return NULL;
  }
  return n;
}

static Node *pipeline(Parser *p) {
  Node *n = command(p);
  if (!n) return NULL;
  while (peek(p) == T_PIPE) {
    p->p++;
    skip_nl(p);
    Node *r = command(p);
    if (!r) { free_node(n); return NULL; }
    n = make(p, N_PIPE, n, r);
    if (!n) return NULL;
  }
  return n;
}

static Node *andor(Parser *p) {
  Node *n = pipeline(p);
  if (!n) return NULL;
  while (peek(p) == T_AND || peek(p) == T_OR) {
    NodeKind k = peek(p) == T_AND ? N_AND : N_OR;
    p->p++;
    skip_nl(p);
    Node *r = pipeline(p);
    if (!r) { 
        free_node(n); 
        return NULL; 
    }
    n = make(p, k, n, r);
    if (!n) return NULL;
  }
  return n;
}

static Node *list(Parser *p) {
  Node *n = andor(p);
  if (!n) return NULL;
  for (;;) {
    TokenKind k = peek(p);
    if (k != T_SEMI && k != T_AMP && k != T_NL) break;
    p->p++;
    if (k == T_AMP) {      
      n = make(p, N_BG, n, NULL);
      if (!n) return NULL;
    }
    skip_nl(p);
    if (peek(p) == T_END || peek(p) == T_RP) break;
    Node *r = andor(p);
    if (!r) { 
        free_node(n); 
        return NULL; 
    }
    n = make(p, N_LIST, n, r);
    if (!n) return NULL;
  }
  return n;
}

int parse_tokens(const Tokens *t, Node **out, char **error) {
  Parser p = {t, 0, error, false, 0};
  *error = NULL;
  *out = NULL;
  skip_nl(&p);
  if (peek(&p) == T_END) return PARSE_OK;
  Node *n = list(&p);
  if (n && peek(&p) != T_END) {
    free_node(n);
    n = NULL;
    fail_token(&p);
  }
  if (!n) {
    if (p.incomplete) 
        return PARSE_INCOMPLETE;
    if (!*error) 
        *error = strdup("syntax error");
    return PARSE_ERROR;
  }
  *out = n;
  return PARSE_OK;
}
