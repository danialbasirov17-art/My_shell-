#include "shell.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int push(Tokens *t, TokenKind k, const char *s, size_t n) {
    Token *p = realloc(t->v, (t->n+1) * sizeof(*p));
    if (!p) 
      return -1;
    t->v = p; 
    t->v[t->n].kind = k; 
    t->v[t->n].text = NULL;
    if (k == T_WORD) { 
        t->v[t->n].text = malloc(n+1); 
        if (!t->v[t->n].text) return -1; 
        memcpy(t->v[t->n].text,s,n); 
        t->v[t->n].text[n]=0; 
  }
  t->n++; 
  return 0;
}

void free_tokens(Tokens *t) { 
    for (size_t i=0; i<t->n; i++){
        free(t->v[i].text); 
        free(t->v); 
        t->v=NULL;
        t->n=0; }}

int lex_line(const char *s, Tokens *out, char **error) {
    memset(out,0,sizeof(*out)); *error=NULL; size_t i=0;
    while (s[i]) {
      if (s[i]==' ' || s[i]=='\t' || s[i]=='\r' || s[i]=='\n') { 
        i++; 
        continue; 
      }
      if (s[i]=='#') 
        break;
      TokenKind k=T_END; 
      size_t width=1;
      if (s[i]=='&' && s[i+1]=='&') 
        k=T_AND, width=2; 
      else if (s[i]=='|' && s[i+1]=='|') 
        k=T_OR, width=2;
      else if (s[i]=='>'&&s[i+1]=='>')
        k=T_APPEND, width=2;
      else if (s[i]=='|')
        k=T_PIPE; 
      else if (s[i]=='&') 
        k=T_AMP; 
      else if (s[i]==';')
        k=T_SEMI;
      else if (s[i]=='(') 
        k=T_LP; 
      else if (s[i]==')') 
        k=T_RP; 
      else if (s[i]=='<') 
        k=T_IN; 
      else if (s[i]=='>') 
        k=T_OUT;
      if (k!=T_END) { 
        if(push(out,k,NULL,0)) 
          goto oom; 
        i+=width; 
        continue; 
      }

      char *buf=NULL; 
      size_t n=0, cap=0; 
      bool started=false; 
      char quote=0;
      while(s[i]) {
        char c=s[i];
        if(quote=='\'') { 
          i++; 
          if(c=='\'') 
            quote=0; 
          else { 
            if(n+1>=cap){
              cap=cap?cap*2:32;
              buf=realloc(buf,cap);
            } 
            buf[n++]=c; 
          } 
          started=true; 
          continue; 
        }
        if(quote=='"') { 
          i++; 
          if(c=='"'){
            quote=0;
            started=true;
            continue;
          } 
          if(c=='\\' && (s[i]=='"' || s[i]=='\\' || s[i]=='\n')) { 
            if(s[i]=='\n'){
              i++;
              started=true;
              continue;
            } 
            c=s[i++]; 
          } 
          if(n+1>=cap){
            cap=cap?cap*2:32;
            buf=realloc(buf,cap);
          } 
          buf[n++]=c;
          started=true;
          continue; 
        }
        if(c=='\'' || c=='"'){
          quote=c;
          started=true;
          i++;
          continue;
        }
        if(c=='\\'){
          i++;
          if(!s[i]){
            free(buf);
            *error=strdup("trailing backslash");
            goto fail;
          } 
          if(s[i]=='\n'){
            i++;
            continue;
          } 
          c=s[i++];
        }
        else if(isspace((unsigned char)c) || strchr("|&;()<>",c)) 
          break;
        else {
          i++;
        }
        if (n+1>=cap){
          cap=cap?cap*2:32;
          buf=realloc(buf,cap);
        } 
        buf[n++]=c;
        started=true;
    }
    if(quote){
      free(buf);
      *error=strdup("unclosed quote");
      goto fail;
    }
    if(started){
      if(!buf){
        buf=malloc(1);
      } 
      buf[n]=0; 
      if(push(out,T_WORD, buf, n)){
        free(buf);
        goto oom;
      } 
      free(buf);
    }
  }
  if(push(out,T_END,NULL,0)) 
    goto oom;
  return 0;
  oom: *error=strdup("out of memory");
  fail: free_tokens(out); 
  return -1;
}
