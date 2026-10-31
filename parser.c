#include "shell.h"
#include <stdlib.h>
#include <string.h>

typedef struct { 
    const Tokens *t; 
    size_t p; 
    char **err; 
} Parser;

static Node *node(NodeKind k, Node*l, Node*r){
    Node*n=calloc(1,sizeof(*n));
    if(n){
        n->kind=k;
        n->left=l;
        n->right=r;
    }
    return n;
}

static TokenKind peek(Parser*p){
    return p->t->v[p->p].kind;
}

void free_node(Node*n){
    if(!n)
        return;
    free_node(n->left);
    free_node(n->right);
    for(size_t i=0; i<n->argc; i++)
        free(n->argv[i]);
    free(n->argv);
    Redir*r=n->redirs;
    while(r){
        Redir*q=r->next;
        free(r->path);
        free(r);
        r=q;
    }
    free(n);
}

static Node *list(Parser*p);
static int addword(Node*n, const char*s){
    char**v=realloc(n->argv, (n->argc+2)*sizeof(*v));
    if(!v)
        return -1;
    n->argv=v;
    n->argv[n->argc]=strdup(s);
    if(!n->argv[n->argc])
        return -1;
    n->argc++;
    n->argv[n->argc]=NULL;
    return 0;
}

static int redir(Node*n,Parser*p){
    TokenKind k=peek(p);
    if(k!=T_IN && k!=T_OUT && k!=T_APPEND)
        return 0;
    p->p++;
    if(peek(p)!=T_WORD){
        *p->err=strdup("expected filename after redirection");
        return -1;
    }
    Redir*r=calloc(1,sizeof(*r));
    if(!r)
        return-1;
    r->kind=k==T_IN?R_IN:k==T_OUT?R_OUT:R_APPEND;
    r->path=strdup(p->t->v[p->p++].text);
    Redir**q=&n->redirs;
    while(*q)
        q=&(*q)->next;
    *q=r;
    return 1;
}
static Node *command(Parser*p){
    Node*n=NULL;
    if(peek(p)==T_LP){
        p->p++;
        n=list(p);
        if(!n || peek(p)!=T_RP){
            free_node(n);
            *p->err=strdup("expected closing parenthesis");
            return NULL;
        }
        p->p++;
        Node*w=node(N_SUBSHELL,n,NULL);
        if(!w){
            free_node(n);
            return NULL;
        }
        n=w;
        while(peek(p)==T_IN || peek(p)==T_OUT || peek(p)==T_APPEND)
        if(redir(n,p)<0){
            free_node(n);
            return NULL;
        }
            return n;
        }  
    n=node(N_SIMPLE,NULL,NULL);
    if(!n)
        return NULL;
    bool word=false;
    for(;;){
        if(peek(p)==T_WORD){
            if(addword(n,p->t->v[p->p++].text)){
                free_node(n);
                return NULL;
            }
            word=true;
        }
        else if(peek(p)==T_IN || peek(p)==T_OUT || peek(p)==T_APPEND){
            if(redir(n,p)<0){
                free_node(n);
                return NULL;
            }
        }
        else break;
    }
    if(!word){
        free_node(n);
        *p->err=strdup("expected command");
        return NULL;
    }
    return n;
}

static Node *pipeline(Parser*p){
    Node*n=command(p);
    if(!n)
        return NULL;
    while(peek(p)==T_PIPE){
        p->p++;
        Node*r=command(p);
        if(!r){
            free_node(n);
            return NULL;
        }
        Node*x=node(N_PIPE,n,r);
        if(!x){
            free_node(n);
            free_node(r);
            return NULL;
        }
        n=x;
    }
    return n;
}

static Node *andor(Parser*p){
    Node*n=pipeline(p);
    if(!n)
        return NULL;
    while(peek(p)==T_AND || peek(p)==T_OR){
        TokenKind k=peek(p);
        p->p++;
        Node*r=pipeline(p);
        if(!r){
            free_node(n);
            return NULL;
        }
        Node*x=node(k==T_AND?N_AND:N_OR,n,r);
        if(!x){
            free_node(n);
            free_node(r);
            return NULL;
        }
        n=x;
    }
    return n;
}

static Node *list(Parser*p){
    Node*n=andor(p);
    if(!n)
        return NULL;
    while(peek(p)==T_SEMI || peek(p)==T_AMP){
        TokenKind k=peek(p);
        p->p++;
        if(peek(p)==T_END || peek(p)==T_RP)
            break;
        Node*r=andor(p);
        if(!r){
            free_node(n);
            return NULL;
        }
        Node*x=node(k==T_AMP?N_BG:N_LIST,n,r);
        if(!x){
            free_node(n);
            free_node(r);
            return NULL;
        }
        n=x;
    }
    return n;
}

Node *parse_tokens(const Tokens*t,char**error){
    Parser p={t,0,error};
    *error=NULL;
    if(peek(&p)==T_END)
        return NULL;
    Node*n=list(&p);
    if(n && peek(&p)!=T_END){
        free_node(n);
        *error=strdup("unexpected operator");
        return NULL;
    }
    return n;
}
