#include "erl_nif.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define ERL_TRUE enif_make_atom(env, "true")
#define ERL_FALSE enif_make_atom(env, "false")
#define ERL_OK(__TERM__)                                                       \
  enif_make_tuple2(env, enif_make_atom(env, "ok"), __TERM__)
#define ERL_ERROR(__TERM__)                                                    \
  enif_make_tuple2(env, enif_make_atom(env, "error"), __TERM__)

static const int PIPE_READ = 0;
static const int PIPE_WRITE = 1;
static const int MAX_ARGUMENTS = 20;
static const int MAX_ARGUMENT_LEN = 1024;

enum exec_status {
  SUCCESS,
  PIPE_CREATE_ERROR,
  PIPE_FLAG_ERROR,
  FORK_ERROR,
  PIPE_DUP_ERROR
};

typedef struct ExecResults {
  enum exec_status status;
  int err;
  pid_t pid;
  int pipe_in;
  int pipe_out;
} ExecResult;

static int set_flag(int fd) {
  return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK | O_CLOEXEC);
}

static void close_all(int pipes[3][2]) {
  for (int i = 0; i < 3; i++) {
    if (pipes[i][PIPE_READ])
      close(pipes[i][PIPE_READ]);
    if (pipes[i][PIPE_WRITE])
      close(pipes[i][PIPE_WRITE]);
  }
}

static ExecResult start_proccess(char *args[]) {
  ExecResult result;
  pid_t pid;
  int pipes[3][2] = {{0, 0}, {0, 0}, {0, 0}};

#define RETURN_ERROR(_status)                                                  \
  do {                                                                         \
    result.status = _status;                                                   \
    result.err = errno;                                                        \
    close_all(pipes);                                                          \
    return result;                                                             \
  } while (0);

  if (pipe(pipes[STDIN_FILENO]) == -1 || pipe(pipes[STDOUT_FILENO]) == -1 ||
      pipe(pipes[STDERR_FILENO]) == -1) {
    RETURN_ERROR(PIPE_CREATE_ERROR)
  }

  if (set_flag(pipes[STDIN_FILENO][PIPE_READ]) < 0 ||
      set_flag(pipes[STDIN_FILENO][PIPE_WRITE]) < 0 ||
      set_flag(pipes[STDOUT_FILENO][PIPE_READ]) < 0 ||
      set_flag(pipes[STDOUT_FILENO][PIPE_WRITE]) < 0 ||
      set_flag(pipes[STDERR_FILENO][PIPE_READ]) < 0 ||
      set_flag(pipes[STDERR_FILENO][PIPE_WRITE]) < 0) {
    RETURN_ERROR(PIPE_FLAG_ERROR)
  }

  switch (pid = fork()) {
  case -1:
    RETURN_ERROR(FORK_ERROR)

  case 0:
    close(STDIN_FILENO);
    close(STDOUT_FILENO);

    if (dup2(pipes[STDIN_FILENO][PIPE_READ], STDIN_FILENO) < 0)
      RETURN_ERROR(PIPE_DUP_ERROR)
    if (dup2(pipes[STDOUT_FILENO][PIPE_WRITE], STDOUT_FILENO) < 0)
      RETURN_ERROR(PIPE_DUP_ERROR)

    close_all(pipes);

    execvp(args[0], args);
    perror("execvp(): failed");

  default:
    close(pipes[STDIN_FILENO][PIPE_READ]);
    close(pipes[STDOUT_FILENO][PIPE_WRITE]);
    result.pid = pid;
    result.pipe_in = pipes[STDIN_FILENO][PIPE_WRITE];
    result.pipe_out = pipes[STDOUT_FILENO][PIPE_READ];
    result.status = SUCCESS;
    return result;
  }
}

static ERL_NIF_TERM exec_proc(ErlNifEnv *env, int argc,
                              const ERL_NIF_TERM argv[]) {
  char _temp[MAX_ARGUMENTS][MAX_ARGUMENT_LEN];
  char *exec_args[MAX_ARGUMENTS + 1];
  char *arg = NULL;

  unsigned int args_len;
  if (enif_get_list_length(env, argv[0], &args_len) != true)
    return enif_make_badarg(env);

  if (args_len > MAX_ARGUMENTS)
    return enif_make_badarg(env);

  ERL_NIF_TERM head, tail, list = argv[0];
  for (int i = 0; i < args_len; i++) {
    if (enif_get_list_cell(env, list, &head, &tail) != true)
      return enif_make_badarg(env);

    if (enif_get_string(env, head, _temp[i], sizeof(_temp[i]), ERL_NIF_LATIN1) <
        1)
      return enif_make_badarg(env);

    exec_args[i] = _temp[i];
    list = tail;
  }
  exec_args[args_len] = NULL;

  ExecResult result = start_proccess(exec_args);

  switch (result.status) {
  case SUCCESS:
    return enif_make_tuple4(env, enif_make_int(env, 0),
                            enif_make_int(env, result.pid),
                            enif_make_int(env, result.pipe_in),
                            enif_make_int(env, result.pipe_out));
  default:
    return enif_make_tuple4(env, enif_make_int(env, -1), enif_make_int(env, 0),
                            enif_make_int(env, 0), enif_make_int(env, 0));
  }
}

static ERL_NIF_TERM write_proc(ErlNifEnv *env, int argc,
                               const ERL_NIF_TERM argv[]) {
  int pipe_in;
  enif_get_int(env, argv[0], &pipe_in);

  if (argc != 2)
    enif_make_badarg(env);

  ErlNifBinary bin;
  bool is_success = enif_inspect_binary(env, argv[1], &bin);
  int result = write(pipe_in, bin.data, bin.size);

  if (result >= 0) {
    return ERL_OK(enif_make_int(env, result));
  } else {
    perror("write()");
    return ERL_ERROR(enif_make_int(env, errno));
  }
}

static ERL_NIF_TERM close_pipe(ErlNifEnv *env, int argc,
                               const ERL_NIF_TERM argv[]) {
  int pipe;
  enif_get_int(env, argv[0], &pipe);

  int result = close(pipe);

  if (result == 0) {
    return enif_make_atom(env, "ok");
  } else {
    perror("close()");
    return ERL_ERROR(enif_make_int(env, errno));
  }
}

static ERL_NIF_TERM read_proc(ErlNifEnv *env, int argc,
                              const ERL_NIF_TERM argv[]) {
  int pipe_out;
  enif_get_int(env, argv[0], &pipe_out);

  char buf[65535];
  int result = read(pipe_out, buf, sizeof(buf));

  if (result >= 0) {
    ErlNifBinary bin;
    enif_alloc_binary(result, &bin);
    memcpy(bin.data, buf, result);
    return ERL_OK(enif_make_binary(env, &bin));
  } else {
    perror("read()");
    return ERL_ERROR(enif_make_int(env, errno));
  }
}

static ERL_NIF_TERM is_alive(ErlNifEnv *env, int argc,
                             const ERL_NIF_TERM argv[]) {
  int pid;
  enif_get_int(env, argv[0], &pid);

  int result = kill(pid, 0);

  if (result == 0) {
    return ERL_TRUE;
  } else {
    return ERL_FALSE;
  }
}

static ERL_NIF_TERM terminate_proc(ErlNifEnv *env, int argc,
                                   const ERL_NIF_TERM argv[]) {
  int pid;
  enif_get_int(env, argv[0], &pid);
  return enif_make_int(env, kill(pid, SIGTERM));
}

static ERL_NIF_TERM kill_proc(ErlNifEnv *env, int argc,
                              const ERL_NIF_TERM argv[]) {
  int pid;
  enif_get_int(env, argv[0], &pid);
  return enif_make_int(env, kill(pid, SIGKILL));
}

static ERL_NIF_TERM wait_proc(ErlNifEnv *env, int argc,
                              const ERL_NIF_TERM argv[]) {
  int pid, status;
  enif_get_int(env, argv[0], &pid);

  int wpid = waitpid(pid, &status, WNOHANG);
  if (wpid != pid) {
    perror("waitpid()");
  }

  return enif_make_tuple2(env, enif_make_int(env, wpid),
                          enif_make_int(env, status));
}

static ErlNifFunc nif_funcs[] = {
    {"exec_proc", 1, exec_proc},           {"write_proc", 2, write_proc},
    {"read_proc", 1, read_proc},           {"close_pipe", 1, close_pipe},
    {"terminate_proc", 1, terminate_proc}, {"wait_proc", 1, wait_proc},
    {"kill_proc", 1, kill_proc},           {"is_alive", 1, is_alive},
};

ERL_NIF_INIT(Elixir.Exile, nif_funcs, NULL, NULL, NULL, NULL)
