#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int options(int argc, char **argv, const char **command, bool *tokens, bool *ast) {
  *command = NULL;
  *tokens = false;
  *ast = false;
  if (argc == 1) 
    return 0;
  if (argc == 3 && !strcmp(argv[1], "-c")) { 
    *command = argv[2]; return 0; }
  if (argc == 2 && !strcmp(argv[1], "--dump-tokens")) { 
    *tokens = true; 
    return 0; 
  }
  if (argc == 2 && !strcmp(argv[1], "--dump-ast")) {
    *ast = true;
    return 0; 
  }
  fprintf(stderr, "mysh: usage: mysh [-c command|--dump-tokens|--dump-ast]\n");
  return 2;
}

static int process_line(const char *line, bool dump_tokens, bool dump_ast, bool interactive, bool *quit) {
  (void)interactive;
  Tokens tokens;
  char *error = NULL;
  if (lex_line(line, &tokens, &error) < 0) {
    fprintf(stderr, "mysh: lexer: %s\n", error ? error : "error");
    free(error);
    return 2;
  }
  if (dump_tokens) {
    for (size_t i = 0; i < tokens.n; i++)
      printf("%d%s%s\n", tokens.v[i].kind,
             tokens.v[i].kind == T_WORD ? " " : "",
             tokens.v[i].text ? tokens.v[i].text : "");
    free_tokens(&tokens);
    *quit = true;
    return 0;
  }
  Node *tree = parse_tokens(&tokens, &error);
  free_tokens(&tokens);
  if (error) {
    fprintf(stderr, "mysh: syntax: %s\n", error);
    free(error);
    return 2;
  }
  if (dump_ast) {
    if (tree) print_ast(tree, 0); else puts("(empty)");
    free_node(tree);
    *quit = true;
    return 0;
  }
  int status = tree ? execute(tree, false) : 0;
  free_node(tree);
  if (shell_exit_requested()) *quit = true;
  return shell_exit_requested() ? shell_exit_code() : status;
}

int main(int argc, char **argv) {
  const char *command;
  bool dump_tokens, dump_ast;
  int option_status = options(argc, argv, &command, &dump_tokens, &dump_ast);
  if (option_status) return option_status;
  bool interactive = isatty(STDIN_FILENO) && command == NULL;
  bool one_command = command != NULL;
  jobs_init(interactive);
  char *line = NULL;
  size_t capacity = 0;
  int status = 0;
  bool quit = false;
  do {
    const char *input;
    if (command) {
      input = command;
      command = NULL;
    } else {
      if (interactive) { fputs("mysh$ ", stdout); fflush(stdout); }
      if (getline(&line, &capacity, stdin) < 0) break;
      input = line;
    }
    status = process_line(input, dump_tokens, dump_ast, interactive, &quit);
    jobs_reap();
    if (one_command) quit = true;
    if (!interactive && status == 2) break;
  } while (!quit);
  free(line);
  jobs_shutdown();
  return status;
}
