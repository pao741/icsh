#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <pwd.h>
#include <glob.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#define NR_JOBS 20
#define BUFSIZE 1024
#define PATH_BUFSIZE 1024
#define TOKEN_BUFSIZE 64
#define DELIM " \t\r\n\a"

#define BACKGROUND_EXECUTION 0
#define FOREGROUND_EXECUTION 1
#define PIPELINE_EXECUTION 2

#define COMMAND_EXTERNAL 0
#define COMMAND_EXIT 1
#define COMMAND_CD 2
#define COMMAND_JOBS 3
#define COMMAND_FG 4
#define COMMAND_BG 5
#define COMMAND_KILL 6
#define COMMAND_EXPORT 7
#define COMMAND_UNSET 8

#define STATUS_RUNNING 0
#define STATUS_DONE 1
#define STATUS_SUSPENDED 2
#define STATUS_CONTINUED 3
#define STATUS_TERMINATED 4

#define COLOR_NONE "\033[m"
#define COLOR_RED "\033[1;37;41m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_CYAN "\033[0;36m"

#define PROC_FILTER_ALL 0
#define PROC_FILTER_DONE 1
#define PROC_FILTER_REMAINING 2

const char *STATUS_STRING[] = {
    "running",
    "done",
    "suspended",
    "continued",
    "terminated"};

struct process
{
    char *command;
    int argc;
    char **argv;
    char *input_path;
    char *output_path;
    pid_t pid;
    int type;
    int status;
    struct process *next;
};

struct job
{
    int id;
    struct process *root;
    char *command;
    pid_t pgid;
    int mode;
};

struct job *jobs[NR_JOBS + 1];
char cur_dir[PATH_BUFSIZE];
int status;

void print_error()
{
    perror("Unable to allocate buffer");
    exit(1);
}
int get_next_job_id()
{
    for (int i = 1; i <= NR_JOBS; i++)
    {
        if (jobs[i] == NULL)
        {
            return i;
        }
    }
    // exceed max job
    return -1;
}
int get_proc_count(int id, int filter)
{
    if (id > NR_JOBS || jobs[id] == NULL)
    {
        return -1;
    }

    int count = 0;
    struct process *proc;
    for (proc = jobs[id]->root; proc != NULL; proc = proc->next)
    {
        if (filter == PROC_FILTER_ALL ||
            (filter == PROC_FILTER_DONE && proc->status == STATUS_DONE) ||
            (filter == PROC_FILTER_REMAINING && proc->status != STATUS_DONE))
        {
            count++;
        }
    }

    return count;
}

void print_sh()
{
    printf(COLOR_YELLOW "icsh> " COLOR_CYAN "%s |" COLOR_NONE "\n", cur_dir);
}

void sigintHandler()
{
    printf("\n");
}
int print_job_status(int id)
{
    if (id > NR_JOBS || jobs[id] == NULL)
    {
        return -1;
    }

    printf("[%d]", id);

    struct process *proc;
    for (proc = jobs[id]->root; proc != NULL; proc = proc->next)
    {
        printf("\t%d\t%s\t%s", proc->pid,
               STATUS_STRING[proc->status], proc->command);
        if (proc->next != NULL)
        {
            printf("|\n");
        }
        else
        {
            printf("\n");
        }
    }

    return 0;
}
int get_command_type(char *command)
{
    if (strcmp(command, "exit") == 0)
    {
        return COMMAND_EXIT;
    }
    else if (strcmp(command, "cd") == 0)
    {
        return COMMAND_CD;
    }
    else if (strcmp(command, "jobs") == 0)
    {
        return COMMAND_JOBS;
    }
    else if (strcmp(command, "fg") == 0)
    {
        return COMMAND_FG;
    }
    else if (strcmp(command, "bg") == 0)
    {
        return COMMAND_BG;
    }
    else if (strcmp(command, "kill") == 0)
    {
        return COMMAND_KILL;
    }
    else if (strcmp(command, "export") == 0)
    {
        return COMMAND_EXPORT;
    }
    else if (strcmp(command, "unset") == 0)
    {
        return COMMAND_UNSET;
    }
    else
    {
        return COMMAND_EXTERNAL;
    }
}

struct process *parse_command_segment(char *segment)
{
    int bufsize = TOKEN_BUFSIZE;
    int position = 0;
    char *command = strdup(segment);
    char *token;
    char **tokens = (char **)malloc(bufsize * sizeof(char *));

    if (!tokens)
    {
        print_error();
    }

    token = strtok(segment, DELIM);
    while (token != NULL)
    {
        tokens[position] = token;
        position++;

        if (position >= bufsize)
        {
            bufsize += 32;
            tokens = realloc(tokens, bufsize * sizeof(char *));
            if (!tokens)
            {
                print_error();
            }
        }

        token = strtok(NULL, DELIM);
    }

    int i = 0, argc = 0;
    char *input_path = NULL, *output_path = NULL;
    while (i < position)
    {
        if (tokens[i][0] == '<' || tokens[i][0] == '>')
        {
            break;
        }
        i++;
    }

    token = strtok(segment, DELIM);

    argc = i;

    for (; i < position; i++)
    {
        if (tokens[i][0] == '<')
        {
            if (strlen(tokens[i]) == 1)
            {

                input_path = (char *)malloc((strlen(tokens[i + 1]) + 1) * sizeof(char));
                strcpy(input_path, tokens[i + 1]);
                i++;
            }
            else
            {
                input_path = (char *)malloc(strlen(tokens[i]) * sizeof(char));
                strcpy(input_path, tokens[i] + 1);
            }
        }
        else if (tokens[i][0] == '>')
        {
            if (strlen(tokens[i]) == 1)
            {
                output_path = (char *)malloc((strlen(tokens[i + 1]) + 1) * sizeof(char));
                strcpy(output_path, tokens[i + 1]);
                i++;
            }
            else
            {
                output_path = (char *)malloc(strlen(tokens[i]) * sizeof(char));
                strcpy(output_path, tokens[i] + 1);
            }
        }

        break;
    }

    // writing this fuction make me cries
    for (i = argc; i <= position; i++)
    {
        tokens[i] = NULL;
    }

    struct process *new_proc = (struct process *)malloc(sizeof(struct process));
    new_proc->command = command;
    new_proc->argv = tokens;
    new_proc->argc = argc;
    new_proc->input_path = input_path;
    new_proc->output_path = output_path;
    new_proc->pid = -1;
    new_proc->type = get_command_type(tokens[0]);
    new_proc->next = NULL;
    return new_proc;
}

struct job *parse_command(char *line)
{
    char *command = strdup(line);

    struct process *root_proc = NULL, *proc = NULL;
    char *line_cursor = line, *c = line, *seg;
    int seg_len = 0, mode = FOREGROUND_EXECUTION;

    if (line[strlen(line) - 1] == '&')
    {
        mode = BACKGROUND_EXECUTION;
        line[strlen(line) - 1] = '\0';
    }
    while (1)
    {
        if (*c == '\0')
        {
            seg = (char *)malloc((seg_len + 1) * sizeof(char));
            strncpy(seg, line_cursor, seg_len);
            seg[seg_len] = '\0';

            struct process *new_proc = parse_command_segment(seg);
            if (!root_proc)
            {
                // if root process is not created
                root_proc = new_proc;
                proc = root_proc;
            }
            else
            {
                proc->next = new_proc;
                proc = new_proc;
            }
            break;
        }
        else
        {
            seg_len++;
            c++;
        }
    }

    struct job *new_job = (struct job *)malloc(sizeof(struct job));
    new_job->root = root_proc;
    new_job->command = command;
    new_job->pgid = -1;
    new_job->mode = mode;
    return new_job;
}

int release_job(int id)
{
    if (id > NR_JOBS || jobs[id] == NULL)
    {
        return -1;
    }

    struct job *job = jobs[id];
    struct process *proc, *tmp;
    for (proc = job->root; proc != NULL;)
    {
        tmp = proc->next;
        free(proc->command);
        free(proc->argv);
        free(proc->input_path);
        free(proc->output_path);
        free(proc);
        proc = tmp;
    }
    free(job->command);
    free(job);

    return 0;
}

int print_processes_of_job(int id)
{
    if (id > NR_JOBS || jobs[id] == NULL)
    {
        return -1;
    }

    printf("[%d]", id);

    struct process *proc;
    for (proc = jobs[id]->root; proc != NULL; proc = proc->next)
    {
        printf(" %d", proc->pid);
    }
    printf("\n");

    return 0;
}
int mysh_cd(int argc, char **argv)
{
    if (chdir(argv[1]) != 0)
    {
        print_error();
    }
    return 0;
}
int get_job_id_by_pid(int pid)
{
    int i;
    struct process *proc;

    for (i = 1; i <= NR_JOBS; i++)
    {
        if (jobs[i] != NULL)
        {
            for (proc = jobs[i]->root; proc != NULL; proc = proc->next)
            {
                if (proc->pid == pid)
                {
                    return i;
                }
            }
        }
    }

    return -1;
}
struct job *get_job_by_id(int id)
{
    if (id > NR_JOBS)
    {
        return NULL;
    }

    return jobs[id];
}
int remove_job(int id)
{
    if (id > NR_JOBS || jobs[id] == NULL)
    {
        return -1;
    }

    release_job(id);
    jobs[id] = NULL;

    return 0;
}
int set_job_status(int id, int status)
{
    if (id > NR_JOBS || jobs[id] == NULL)
    {
        return -1;
    }

    struct process *proc;

    for (proc = jobs[id]->root; proc != NULL; proc = proc->next)
    {
        if (proc->status != STATUS_DONE)
        {
            proc->status = status;
        }
    }

    return 0;
}

int set_process_status(int pid, int status)
{
    int i;
    struct process *proc;

    for (i = 1; i <= NR_JOBS; i++)
    {
        if (jobs[i] == NULL)
        {
            continue;
        }
        for (proc = jobs[i]->root; proc != NULL; proc = proc->next)
        {
            if (proc->pid == pid)
            {
                proc->status = status;
                return 0;
            }
        }
    }

    return -1;
}
int wait_for_pid(int pid)
{
    int c_status = 0;

    waitpid(pid, &c_status, WUNTRACED);
    if (WIFEXITED(status))
    {
        set_process_status(pid, STATUS_DONE);
    }
    else if (WIFSIGNALED(c_status))
    {
        set_process_status(pid, STATUS_TERMINATED);
    }
    else if (WSTOPSIG(c_status))
    {
        c_status = -1;
        set_process_status(pid, STATUS_SUSPENDED);
    }

    return c_status;
}
int wait_for_job(int id)
{
    if (id > NR_JOBS || jobs[id] == NULL)
    {
        return -1;
    }

    int proc_count = get_proc_count(id, PROC_FILTER_REMAINING);
    int wait_pid = -1, wait_count = 0;
    int c_status = 0;

    do
    {
        wait_pid = waitpid(-jobs[id]->pgid, &status, WUNTRACED);
        wait_count++;

        if (WIFEXITED(status))
        {
            set_process_status(wait_pid, STATUS_DONE);
        }
        else if (WIFSIGNALED(status))
        {
            set_process_status(wait_pid, STATUS_TERMINATED);
        }
        else if (WSTOPSIG(status))
        {
            status = -1;
            set_process_status(wait_pid, STATUS_SUSPENDED);
            if (wait_count == proc_count)
            {
                print_job_status(id);
            }
        }
    } while (wait_count < proc_count);

    return c_status;
}

int get_pgid_by_job_id(int id)
{
    struct job *job = get_job_by_id(id);

    if (job == NULL)
    {
        return -1;
    }

    return job->pgid;
}

int mysh_jobs(int argc, char **argv)
{
    int i;

    for (i = 0; i < NR_JOBS; i++)
    {
        if (jobs[i] != NULL)
        {
            print_job_status(i);
        }
    }

    return 0;
}

int mysh_fg(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: fg <pid>\n");
        return -1;
    }

    int c_status;
    pid_t pid;
    int job_id = -1;

    if (argv[1][0] == '%')
    {
        job_id = atoi(argv[1] + 1);
        pid = get_pgid_by_job_id(job_id);
        if (pid < 0)
        {
            printf("mysh: fg %s: no such job\n", argv[1]);
            return -1;
        }
    }
    else
    {
        pid = atoi(argv[1]);
    }

    if (kill(-pid, SIGCONT) < 0)
    {
        printf("mysh: fg %d: job not found\n", pid);
        return -1;
    }

    tcsetpgrp(0, pid);

    if (job_id > 0)
    {
        set_job_status(job_id, STATUS_CONTINUED);
        print_job_status(job_id);
        if (wait_for_job(job_id) >= 0)
        {
            remove_job(job_id);
        }
    }
    else
    {
        wait_for_pid(pid);
    }

    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(0, getpid());
    signal(SIGTTOU, SIG_DFL);

    return 0;
}

int mysh_bg(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: bg <pid>\n");
        return -1;
    }

    pid_t pid;
    int job_id = -1;

    if (argv[1][0] == '%')
    {
        job_id = atoi(argv[1] + 1);
        pid = get_pgid_by_job_id(job_id);
        if (pid < 0)
        {
            printf("mysh: bg %s: no such job\n", argv[1]);
            return -1;
        }
    }
    else
    {
        pid = atoi(argv[1]);
    }

    if (kill(-pid, SIGCONT) < 0)
    {
        printf("mysh: bg %d: job not found\n", pid);
        return -1;
    }

    if (job_id > 0)
    {
        set_job_status(job_id, STATUS_CONTINUED);
        print_job_status(job_id);
    }

    return 0;
}

int mysh_kill(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: kill <pid>\n");
        return -1;
    }

    pid_t pid;
    int job_id = -1;

    if (argv[1][0] == '%')
    {
        job_id = atoi(argv[1] + 1);
        pid = get_pgid_by_job_id(job_id);
        if (pid < 0)
        {
            printf("mysh: kill %s: no such job\n", argv[1]);
            return -1;
        }
        pid = -pid;
    }
    else
    {
        pid = atoi(argv[1]);
    }

    if (kill(pid, SIGKILL) < 0)
    {
        printf("mysh: kill %d: job not found\n", pid);
        return 0;
    }

    if (job_id > 0)
    {
        set_job_status(job_id, STATUS_TERMINATED);
        print_job_status(job_id);
        if (wait_for_job(job_id) >= 0)
        {
            remove_job(job_id);
        }
    }

    return 1;
}

int mysh_export(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: export KEY=VALUE\n");
        return -1;
    }

    return putenv(argv[1]);
}

int mysh_unset(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: unset KEY\n");
        return -1;
    }

    return unsetenv(argv[1]);
}

void mysh_exit()
{
    exit(0);
}
int mysh_execute_builtin_command(struct process *proc)
{
    int status = 1;

    switch (proc->type)
    {
    case COMMAND_EXIT:
        mysh_exit();
        break;
    case COMMAND_CD:
        mysh_cd(proc->argc, proc->argv);
        break;
    case COMMAND_JOBS:
        mysh_jobs(proc->argc, proc->argv);
        break;
    case COMMAND_FG:
        mysh_fg(proc->argc, proc->argv);
        break;
    case COMMAND_BG:
        mysh_bg(proc->argc, proc->argv);
        break;
    case COMMAND_KILL:
        mysh_kill(proc->argc, proc->argv);
        break;
    case COMMAND_EXPORT:
        mysh_export(proc->argc, proc->argv);
        break;
    case COMMAND_UNSET:
        mysh_unset(proc->argc, proc->argv);
        break;
    default:
        status = 0;
        break;
    }

    return status;
}

int insert_job(struct job *job)
{
    int id = get_next_job_id();

    if (id < 0)
    {
        return -1;
    }
    job->id = id;
    jobs[id] = job;
    return id;
}

int mysh_launch_process(struct job *job, struct process *proc, int in_fd, int out_fd, int mode)
{
    proc->status = STATUS_RUNNING;
    // printf("launching");
    if (proc->type != COMMAND_EXTERNAL && mysh_execute_builtin_command(proc))
    {
        return 0;
    }

    pid_t childpid;
    int status = 0;

    childpid = fork();

    if (childpid < 0)
    {
        return -1;
    }
    else if (childpid == 0)
    {
        proc->pid = getpid();
        if (job->pgid > 0)
        {
            setpgid(0, job->pgid);
        }
        else
        {
            job->pgid = proc->pid;
            setpgid(0, job->pgid);
        }

        if (in_fd != 0)
        {
            dup2(in_fd, 0);
            close(in_fd);
        }

        if (out_fd != 1)
        {
            dup2(out_fd, 1);
            close(out_fd);
        }

        if (execvp(proc->argv[0], proc->argv) < 0)
        {
            printf("mysh: %s: command not found\n", proc->argv[0]);
            exit(0);
        }

        exit(0);
    }
    else
    {
        proc->pid = childpid;
        if (job->pgid > 0)
        {
            setpgid(childpid, job->pgid);
        }
        else
        {
            job->pgid = proc->pid;
            setpgid(childpid, job->pgid);
        }

        if (mode == FOREGROUND_EXECUTION)
        {
            tcsetpgrp(0, job->pgid);
            status = wait_for_job(job->id);
            signal(SIGTTOU, SIG_IGN);
            tcsetpgrp(0, getpid());
            signal(SIGTTOU, SIG_DFL);
        }
    }

    return status;
}

int execute(struct job *job)
{
    struct process *proc;
    int c_status = 0, in_fd = 0, fd[2], job_id = -1;
    // printf("%s", job->command);

    if (job->root->type == COMMAND_EXTERNAL)
    {
        job_id = insert_job(job);
    }

    for (proc = job->root; proc != NULL; proc = proc->next)
    {
        if (proc == job->root && proc->input_path != NULL)
        {
            in_fd = open(proc->input_path, O_RDONLY); // open for reading only
            if (in_fd < 0)
            {
                // im gonna cry this is a lot of work
                printf("mysh: no such file or directory: %s\n", proc->input_path);
                remove_job(job_id);
                return -1;
            }
        }
        if (proc->next != NULL)
        {
            pipe(fd);
            c_status = mysh_launch_process(job, proc, in_fd, fd[1], PIPELINE_EXECUTION);
            close(fd[1]);
            in_fd = fd[0];
        }
        else
        {
            int out_fd = 1;
            if (proc->output_path != NULL)
            {
                out_fd = open(proc->output_path, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                if (out_fd < 0)
                {
                    out_fd = 1;
                }
            }
            c_status = mysh_launch_process(job, proc, in_fd, out_fd, job->mode);
        }
    }

    if (job->root->type == COMMAND_EXTERNAL)
    {
        if (c_status >= 0 && job->mode == FOREGROUND_EXECUTION)
        {
            remove_job(job_id);
        }
        else if (job->mode == BACKGROUND_EXECUTION)
        {
            print_processes_of_job(job_id);
        }
    }

    return c_status;
}

char *read_line()
{
    char *buffer;
    size_t bufsize = 32;
    buffer = (char *)malloc(BUFSIZE * sizeof(char));

    if (!buffer)
    {
        print_error();
    }

    getline(&buffer, &bufsize, stdin);
    return buffer;
}

void loop()
{
    char *line;
    struct job *job;
    signal(SIGINT, sigintHandler);
    signal(SIGTSTP, sigintHandler);
    while (1)
    {
        printf("icsh> ");
        line = read_line();
        if (strlen(line) == 1)
        {
            continue;
        }
        job = parse_command(line);
        status = execute(job);
        free(line);
    }
}

void start()
{

    pid_t pid = getpid();
    setpgid(pid, pid);
    tcsetpgrp(0, pid);

    for (int i = 0; i < NR_JOBS; i++)
    {
        jobs[i] = NULL;
    }
    getcwd(cur_dir, sizeof(cur_dir));
}

int main()
{
    start();
    loop();
    return 0;
}
