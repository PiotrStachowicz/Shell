#ifdef READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static void sigint_handler(int sig) {
  /* No-op handler, we just need break read() call with EINTR. */
  (void)sig;
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp) {
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++) {
    /* TODO: Handle tokens and open files as requested. */
#ifdef STUDENT
    token_t current_token = token[i];
    int fd;

    /* Input/Output mode */
    if (mode != NULL) {
      if (mode == T_INPUT) {
        MaybeClose(inputp);

        fd =
          Open(current_token, O_RDONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
        *inputp = fd;
      } else {
        MaybeClose(outputp);

        fd =
          Open(current_token, O_WRONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
        *outputp = fd;
      }

      mode = NULL;
      token[i] = T_NULL;
    }
    /* Other token */
    else {
      if (current_token == T_INPUT) {
        mode = T_INPUT;
        token[i] = T_NULL;
      } else if (current_token == T_OUTPUT) {
        mode = T_OUTPUT;
        token[i] = T_NULL;
      } else {
        n += 1;
      }
    }
#endif /* !STUDENT */
  }

  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if (!bg) {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */
#ifdef STUDENT
  pid_t pid = Fork();

  /* Father */
  if (pid > 0) {
    /* Close unused fd's */
    MaybeClose(&input);
    MaybeClose(&output);

    /* Create job with process, set correct pgid for child */
    setpgid(pid, pid);
    int j = addjob(pid, bg);
    addproc(j, pid, token);

    /* Monitor foreground job */
    if (!bg) {
      monitorjob(&mask);
    } else {
      msg("[%d] running '%s'\n", j, jobcmd(j));
    }
  }
  /* Child */
  else {
    /* Better safe than sorry (racing conditions?!) */
    /* Set my pgid to my pid and give my group terminal */
    setpgid(pid, pid);
    if (!bg)
      setfgpgrp(getpgrp());

    /* Change input/output of process */
    if (input != -1) {
      Dup2(input, STDIN_FILENO);
      MaybeClose(&input);
    }
    if (output != -1) {
      Dup2(output, STDOUT_FILENO);
      MaybeClose(&output);
    }

    /* Set default signal behaviour as stated in GNU C library */
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    Sigprocmask(SIG_SETMASK, &mask, NULL);

    /* Run background builtin command */
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;

    /* Run external command */
    external_command(token);

    /* Code never reaches here */
  }
#endif /* !STUDENT */

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens, bool bg) {
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

  /* TODO: Start a subprocess and make sure it's moved to a process group. */
  pid_t pid = Fork();
#ifdef STUDENT
  /* Father */
  if (pid > 0) {
    /* Either I am first stage (pgid == 0), then set pgid to my pid
       or I am some latter stage, then I should have pgid from argument */
    pgid == 0 ? setpgid(pid, pid) : setpgid(pid, pgid);
  } else {
    pgid == 0 ? setpgid(pid, pid) : setpgid(pid, pgid);

    /* Change input/output of process */
    if (input != -1) {
      Dup2(input, STDIN_FILENO);
      MaybeClose(&input);
    }
    if (output != -1) {
      Dup2(output, STDOUT_FILENO);
      MaybeClose(&output);
    }

    /* Set default signal behaviour as stated in GNU C library */
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    Sigprocmask(SIG_SETMASK, mask, NULL);

    /* Run background builtin command */
    builtin_command(token);

    /* Run external command */
    external_command(token);

    /* Code never reaches here */
  }
#endif /* !STUDENT */

  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */
#ifdef STUDENT
  int itoken = 0;

  while (token[itoken] != T_NULL) {

    /* End of stage */
    if (token[itoken] == T_PIPE) {
      pid = do_stage(pgid, &mask, input, output, token, itoken, bg);

      /* We do not need input/output fd now */
      /* If we write to output and read from next_input,
         then input now should be next_input */
      MaybeClose(&input);
      input = next_input;
      MaybeClose(&output);

      /* Create pipe connecting output -> next_input */
      mkpipe(&next_input, &output);

      /* First stage */
      if (pgid == 0) {
        pgid = pid;
        job = addjob(pgid, bg);
      }

      addproc(job, pid, token);

      /* Shrink input tokens */
      token += itoken + 1;
      itoken = 0;
    } else {
      itoken++;
    }
  }
  /* Last stage */
  /* Close output so that we write to terminal */
  MaybeClose(&output);

  /* Give pipeline process the terminal */
  if (!bg)
    setfgpgrp(pgid);

  pid = do_stage(pgid, &mask, input, output, token, itoken, bg);

  addproc(job, pid, token);

  /* Like in do_job, monitor the job untill it's finished */
  if (!bg) {
    monitorjob(&mask);
  }

  /* "Maybe" close all previously fds */
  MaybeClose(&input);
  MaybeClose(&next_input);
  MaybeClose(&output);
#endif /* !STUDENT */

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

#ifndef READLINE
static char *readline(const char *prompt) {
  static char line[MAXLINE]; /* `readline` is clearly not reentrant! */

  write(STDOUT_FILENO, prompt, strlen(prompt));

  line[0] = '\0';

  ssize_t nread = read(STDIN_FILENO, line, MAXLINE);
  if (nread < 0) {
    if (errno != EINTR)
      unix_error("Read error");
    msg("\n");
  } else if (nread == 0) {
    return NULL; /* EOF */
  } else {
    if (line[nread - 1] == '\n')
      line[nread - 1] = '\0';
  }

  return strdup(line);
}
#endif

int main(int argc, char *argv[]) {
  /* `stdin` should be attached to terminal running in canonical mode */
  if (!isatty(STDIN_FILENO))
    app_error("ERROR: Shell can run only in interactive mode!");

#ifdef READLINE
  rl_initialize();
#endif

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  if (getsid(0) != getpgid(0))
    Setpgid(0, 0);

  initjobs();

  struct sigaction act = {
    .sa_handler = sigint_handler,
    .sa_flags = 0, /* without SA_RESTART read() will return EINTR */
  };
  Sigaction(SIGINT, &act, NULL);

  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  while (true) {
    char *line = readline("# ");

    if (line == NULL)
      break;

    if (strlen(line)) {
#ifdef READLINE
      add_history(line);
#endif
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
