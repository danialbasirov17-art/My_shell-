#include "shell.h"
#include <stdlib.h>
#include <string.h>

typedef struct { char *v; size_t n, cap; } Buf;

static int buf_put(Buf *b, char c) {
  if (b->n + 1 >= b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 32;
    char *p = realloc(b->v, cap);
    if (!p) return -1;
    b->v = p;
    b->cap = cap;
  }
  b->v[b->n++] = c;
  return 0;
}

static int push(Tokens *t, TokenKind k, char *text) {
  Token *p = realloc(t->v, (t->n + 1) * sizeof(*p));
  if (!p) return -1;
  t->v = p;
  t->v[t->n].kind = k;
  t->v[t->n].text = text;
  t->n++;
  return 0;
}

void free_tokens(Tokens *t) {
  for (size_t i = 0; i < t->n; i++) free(t->v[i].text);
  free(t->v);
  t->v = NULL;
  t->n = 0;
}

const char *token_name(TokenKind k) {
  switch (k) {
    case T_WORD: return "WORD";
    case T_PIPE: return "PIPE";
    case T_AMP: return "AMP";
    case T_SEMI: return "SEMI";
    case T_NL: return "NEWLINE";
    case T_AND: return "AND";
    case T_OR: return "OR";
    case T_LP: return "LPAREN";
    case T_RP: return "RPAREN";
    case T_IN: return "LESS";
    case T_OUT: return "GREAT";
    case T_APPEND: return "DGREAT";
    case T_END: return "END";
  }
  return "?";
}

const char *token_symbol(TokenKind k) {
  switch (k) {
    case T_PIPE: return "|";
    case T_AMP: return "&";
    case T_SEMI: return ";";
    case T_AND: return "&&";
    case T_OR: return "||";
    case T_LP: return "(";
    case T_RP: return ")";
    case T_IN: return "<";
    case T_OUT: return ">";
    case T_APPEND: return ">>";
    case T_NL:
    case T_END: return "newline";
    case T_WORD: return "word";
  }
  return "?";
}

static bool is_meta(char c) {
  return c == ' ' || c == '\t' || c == '\n' || strchr("|&;()<>", c) != NULL;
}

static bool all_digits(const Buf *b) {
  if (b->n == 0) return false;
  for (size_t i = 0; i < b->n; i++)
    if (b->v[i] < '0' || b->v[i] > '9') return false;
  return true;
}

static char *dup_msg(const char *m) {
  char *s = strdup(m);
  return s;
}

int lex_line(const char *s, Tokens *out, char **error) {
  memset(out, 0, sizeof(*out));
  *error = NULL;
  size_t i = 0;
  Buf b = {0};
  int rc = LEX_ERROR;
  const char *msg = "out of memory";

  while (s[i]) {
    char c = s[i];
    if (c == ' ' || c == '\t') { i++; continue; }
    if (c == '\n') {
      if (push(out, T_NL, NULL)) goto fail;
      i++;
      continue;
    }
    if (c == '#') {                     
      while (s[i] && s[i] != '\n') i++;
      continue;
    }

    TokenKind k = T_END;
    size_t width = 1;
    if (c == '&' && s[i + 1] == '&') k = T_AND, width = 2;
    else if (c == '|' && s[i + 1] == '|') k = T_OR, width = 2;
    else if (c == '>' && s[i + 1] == '>') k = T_APPEND, width = 2;
    else if (c == '|') k = T_PIPE;
    else if (c == '&') k = T_AMP;
    else if (c == ';') k = T_SEMI;
    else if (c == '(') k = T_LP;
    else if (c == ')') k = T_RP;
    else if (c == '<') k = T_IN;
    else if (c == '>') k = T_OUT;
    if (k != T_END) {
      if (push(out, k, NULL)) goto fail;
      i += width;
      continue;
    }

    b.n = 0;
    bool started = false, quoted = false;
    while (s[i] && !is_meta(s[i])) {
      c = s[i];
      if (c == '\'') {                   
        started = quoted = true;
        i++;
        while (s[i] && s[i] != '\'')
          if (buf_put(&b, s[i++])) goto fail;
        if (!s[i]) { rc = LEX_UNCLOSED; msg = "unexpected EOF while looking for matching `''"; goto fail; }
        i++;
      } else if (c == '"') {              
        started = quoted = true;
        i++;
        while (s[i] && s[i] != '"') {
          if (s[i] == '\\' && (s[i + 1] == '"' || s[i + 1] == '\\')) {
            if (buf_put(&b, s[i + 1])) goto fail;
            i += 2;
          } else if (s[i] == '\\' && s[i + 1] == '\n') {
            i += 2;
          } else if (buf_put(&b, s[i++])) goto fail;
        }
        if (!s[i]) { rc = LEX_UNCLOSED; msg = "unexpected EOF while looking for matching `\"'"; goto fail; }
        i++;
      } else if (c == '\\') {
        if (s[i + 1] == '\n') {
          if (!s[i + 2]) { rc = LEX_CONTINUE; msg = "line continuation"; goto fail; }
          i += 2;                         
        } else if (!s[i + 1]) {           
          if (buf_put(&b, '\\')) goto fail;
          started = true;
          i++;
        } else {
          if (buf_put(&b, s[i + 1])) goto fail;
          started = quoted = true;
          i += 2;
        }
      } else {
        if (buf_put(&b, c)) goto fail;
        started = true;
        i++;
      }
    }
    if (!started) continue;               

    if (!quoted && (s[i] == '<' || s[i] == '>') && all_digits(&b)) {
      rc = LEX_ERROR;
      msg = "redirection with a file descriptor number is not supported";
      goto fail;
    }
    if (buf_put(&b, '\0')) goto fail;
    char *text = b.v;                     
    b.v = NULL;
    b.n = b.cap = 0;
    if (push(out, T_WORD, text)) { free(text); goto fail; }
  }
  if (push(out, T_END, NULL)) goto fail;
  free(b.v);
  return LEX_OK;

fail:
  free(b.v);
  free_tokens(out);
  *error = dup_msg(msg);
  return rc;
}
