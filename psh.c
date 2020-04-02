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
#define PATH_BUFSIZE 1024
#define COMMAND_BUFSIZE 1024
#define TOKEN_BUFSIZE 64
#define TOKEN_DELIMITERS " \t\r\n\a"

#define BACKGROUND_EXECUTION 0
#define FOREGROUND_EXECUTION 1
#define PIPELINE_EXECUTION 2

#define STATUS_RUNNING 0
#define STATUS_DONE 1
#define STATUS_SUSPENDED 2
#define STATUS_CONTINUED 3
#define STATUS_TERMINATED 4

#define PROC_FILTER_ALL 0
#define PROC_FILTER_DONE 1
#define PROC_FILTER_REMAINING 2

#define COLOR_NONE "\033[m"
#define COLOR_RED "\033[1;37;41m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_CYAN "\033[0;36m"

const char *STATUS_STRING[] = {
    "running",
    "done",
    "suspended",
    "continued",
    "terminated"};
struct job *jobs[NR_JOBS];
char cur_dir[PATH_BUFSIZE];
int status;

struct process
{
    char *command;
    char **argv;
    pid_t pid;
    int status;
    struct process *next;
};

struct job
{
    int id;
    struct process *proc;
    char *command;
    pid_t pgid;
    int mode;
};

int sh_cd(char **args);
int sh_jobs(char **args);
int sh_exit(char **args);

char *builtin_str[] = {
    "cd\n",
    "jobs\n",
    "exit\n"};

int (*builtin_func[])(char **) = {
    &sh_cd,
    &sh_jobs,
    &sh_exit};

int sh_num_builtins()
{
    return sizeof(builtin_str) / sizeof(char *);
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
int print_job_status(int id)
{
    if (id > NR_JOBS || jobs[id] == NULL)
    {
        return -1;
    }

    printf("[%d]", id);

    struct process *proc;
    for (proc = jobs[id]->proc; proc != NULL; proc = proc->next)
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
void print_promt()
{
    printf(COLOR_YELLOW "icsh> " COLOR_CYAN "%s |" COLOR_NONE "\n", cur_dir);
}
int sh_jobs(char **argv)
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
int sh_cd(char **args)
{
    if (args[1] == NULL)
    {
        fprintf(stderr, "icsh: expected argument to \"cd\"\n");
    }
    else
    {
        if (chdir(args[1]) != 0)
        {
            print_error();
        }
        getcwd(cur_dir, sizeof(cur_dir));
    }
    return 1;
}

int sh_exit(char **args)
{
    exit(0);
}

void print_error()
{
    perror("Unable to allocate buffer");
    exit(1);
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

struct process *create_process(char *segment)
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

    token = strtok(segment, TOKEN_DELIMITERS);
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

        token = strtok(NULL, TOKEN_DELIMITERS);
    }

    tokens[position] = NULL;

    struct process *new_proc = (struct process *)malloc(sizeof(struct process));
    new_proc->command = command;
    new_proc->argv = tokens;
    new_proc->pid = -1;
    new_proc->next = NULL;
    return new_proc;
}
int launch_process(struct job *job, struct process *proc, int mode)
{
    proc->status = STATUS_RUNNING;
    int i;

    if (proc->argv[0] == NULL)
    {
        // An empty command was entered.
        printf("No command was entered");
        return 1;
    }

    for (i = 0; i < sh_num_builtins(); i++)
    {
        printf("[%s] [%s] ", proc->argv[0], builtin_str[i]);
        if (strcmp(proc->argv[0], builtin_str[i]) == 0)
        {
            return (*builtin_func[i])(proc->argv);
        }
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
        // give new process an id
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

        if (execvp(proc->argv[0], proc->argv) < 0)
        {
            printf("mysh: %s: command not found\n", proc->argv[0]);
        }

        return 0;
    }
    // printf("childpid > 0");
    // todo: chain / redirection
    //       creating many sub process afterward

    return status;
}

int execute(struct job *job)
{
    struct process *proc;
    int job_id = -1;

    // printf("%s", job->command);
    job_id = insert_job(job);
    // do pipe here
    return launch_process(job, proc, job->mode);
}

char *read_line()
{
    char *buffer;
    size_t bufsize = 32;
    buffer = (char *)malloc(COMMAND_BUFSIZE * sizeof(char));

    if (!buffer)
    {
        print_error();
    }

    getline(&buffer, &bufsize, stdin);
    return buffer;
}

struct job *parse_command(char *line)
{
    char *command = strdup(line);

    struct process *proc = NULL;
    char *line_cursor = line, *current = line, *seg;
    int seg_len = 0, mode = FOREGROUND_EXECUTION;

    if (line[strlen(line) - 1] == '&')
    {
        mode = BACKGROUND_EXECUTION;
        line[strlen(line) - 1] = '\0';
    }
    while (1)
    {
        if (*current == '\0')
        {
            seg = (char *)malloc((seg_len + 1) * sizeof(char));
            strncpy(seg, line_cursor, seg_len);
            seg[seg_len] = '\0';

            proc = create_process(seg);
            break;
        }
        else
        {
            seg_len++;
            current++;
        }
    }

    struct job *new_job = (struct job *)malloc(sizeof(struct job));
    new_job->proc = proc;
    new_job->command = command;
    new_job->pgid = -1;
    new_job->mode = mode;
    return new_job;
}

void loop()
{
    char *line;
    struct job *job;
    // signal(SIGINT, sigintHandler);
    // signal(SIGTSTP, sigintHandler);

    while (1)
    {
        print_promt();
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