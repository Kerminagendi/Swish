#define _GNU_SOURCE

#include "swish_funcs.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"

#define MAX_ARGS 10

int tokenize(char *input, strvec_t *tokens) {
    char *token = strtok(input, " \n");
    while (token != NULL) {
        if (strvec_add(tokens, token) == -1) {
            fprintf(stderr, "tokenize: failed to add token '%s' to vector\n", token);
            return -1;
        }
        token = strtok(NULL, " \n");
    }
    return 0;
}

int run_command(strvec_t *tokens) {
    if (tokens == NULL || tokens->length == 0) {
        fprintf(stderr, "run_command: empty command\n");
        return -1;
    }

    // Put this process into its own process group
    if (setpgid(0, 0) == -1) {
        perror("setpgid");
        return -1;
    }

    // Reset SIGTTIN and SIGTTOU to default
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTTOU, &sa, NULL) == -1 || sigaction(SIGTTIN, &sa, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    // --- INPUT redirection ---
    int in_index = strvec_find(tokens, "<");
    if (in_index != -1 && in_index + 1 < tokens->length) {
        const char *in_file = strvec_get(tokens, in_index + 1);
        int in_fd = open(in_file, O_RDONLY);
        if (in_fd == -1) {
            perror("Failed to open input file");
            return -1;
        }
        if (dup2(in_fd, STDIN_FILENO) == -1) {
            perror("dup2 (input)");
            close(in_fd);
            return -1;
        }
        close(in_fd);
        strvec_take(tokens, in_index + 1);  // Remove filename
        strvec_take(tokens, in_index);      // Remove "<"
    }

    // --- OUTPUT redirection ---
    int out_index = strvec_find(tokens, ">");
    int append_index = strvec_find(tokens, ">>");
    int redir_index = -1;
    int use_append = 0;

    if (out_index != -1) {
        redir_index = out_index;
    } else if (append_index != -1) {
        redir_index = append_index;
        use_append = 1;
    }

    if (redir_index != -1 && redir_index + 1 < tokens->length) {
        const char *out_file = strvec_get(tokens, redir_index + 1);
        int flags = O_CREAT | O_WRONLY;
        flags |= use_append ? O_APPEND : O_TRUNC;

        int out_fd = open(out_file, flags, S_IRUSR | S_IWUSR);
        if (out_fd == -1) {
            perror("Failed to open output file");
            return -1;
        }
        if (dup2(out_fd, STDOUT_FILENO) == -1) {
            perror("dup2 (output)");
            close(out_fd);
            return -1;
        }
        close(out_fd);
        strvec_take(tokens, redir_index + 1);  // Remove filename
        strvec_take(tokens, redir_index);      // Remove ">" or ">>"
    }

    // Prepare arguments for execvp after removing redirection tokens
    char *argv[MAX_ARGS + 1];
    int argc = 0;
    for (int i = 0; i < tokens->length && argc < MAX_ARGS; i++) {
        argv[argc++] = (char *)strvec_get(tokens, i);
    }
    argv[argc] = NULL;

    // Run the command using execvp
    execvp(argv[0], argv);
    perror("exec");
    return -1;
}

int resume_job(strvec_t *tokens, job_list_t *jobs, int is_foreground) {
    if (tokens->length < 2) {
        fprintf(stderr, "resume_job: missing index\n");
        return -1;
    }

    int idx = atoi(strvec_get(tokens, 1));
    job_t *job = job_list_get(jobs, idx);
    if (!job) {
        fprintf(stderr, "Job index out of bounds\n");
        return -1;
    }

    if (is_foreground && tcsetpgrp(STDIN_FILENO, job->pid) == -1) {
        perror("tcsetpgrp");
        return -1;
    }

    if (kill(job->pid, SIGCONT) == -1) {
        perror("kill");
        return -1;
    }

    if (is_foreground) {
        int status;
        if (waitpid(job->pid, &status, WUNTRACED) == -1) {
            perror("waitpid");
            return -1;
        }

        if (!WIFSTOPPED(status)) {
            job_list_remove(jobs, idx);  // ✅ correct index
        } else {
            job->status = STOPPED;
        }

        if (tcsetpgrp(STDIN_FILENO, getpid()) == -1) {
            perror("tcsetpgrp (restore shell)");
            return -1;
        }
    } else {
        job->status = BACKGROUND;
    }

    return 0;
}


int await_background_job(strvec_t *tokens, job_list_t *jobs) {
    if (tokens->length < 2) {
        fprintf(stderr, "wait-for: missing index\n");
        return -1;
    }
    int index = atoi(strvec_get(tokens, 1));
    job_t *job = job_list_get(jobs, index);
    if (!job) {
        fprintf(stderr, "Job index out of bounds\n");
        return -1;
    }
    if (job->status != BACKGROUND) {
        fprintf(stderr, "Job index is for stopped process not background process\n");
        return -1;
    }

    int status;
    if (waitpid(job->pid, &status, WUNTRACED) == -1) {
        perror("waitpid");
        return -1;
    }
    if (!WIFSTOPPED(status)) {
        job_list_remove(jobs, index);  // ✅ This will free everything properly
    } else {
        job->status = STOPPED;
    }

    return 0;
}


int await_all_background_jobs(job_list_t *jobs) {
    job_t *curr = jobs->head;
    unsigned idx = 0;

    while (curr) {
        int status;
        job_t *next = curr->next;

        if (curr->status == BACKGROUND) {
            if (waitpid(curr->pid, &status, WUNTRACED) == -1) {
                perror("waitpid");
                return -1;
            }

            if (WIFSTOPPED(status)) {
                curr->status = STOPPED;
                idx++; // move to next index only if not removed
            } else {
                job_list_remove(jobs, idx);
                // no idx increment here since current was removed
            }
        } else {
            idx++;
        }

        curr = next;
    }

    return 0;
}
