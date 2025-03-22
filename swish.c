#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>

#include "job_list.h"
#include "string_vector.h"
#include "swish_funcs.h"

#define CMD_LEN 512
#define PROMPT "@> "

int main(int argc, char **argv) {
    // Ignore SIGTTIN and SIGTTOU in shell process
    struct sigaction sac;
    sac.sa_handler = SIG_IGN;
    if (sigfillset(&sac.sa_mask) == -1) {
        perror("sigfillset");
        return 1;
    }
    sac.sa_flags = 0;
    if (sigaction(SIGTTIN, &sac, NULL) == -1 || sigaction(SIGTTOU, &sac, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    strvec_t tokens;
    strvec_init(&tokens);
    job_list_t jobs;
    job_list_init(&jobs);
    char cmd[CMD_LEN];

    printf("%s", PROMPT);

    while (fgets(cmd, CMD_LEN, stdin) != NULL) {
        int i = 0;
        while (cmd[i] != '\n' && cmd[i] != '\0') i++;
        cmd[i] = '\0';

        if (tokenize(cmd, &tokens) != 0) {
            printf("Failed to parse command\n");
            strvec_clear(&tokens);
            job_list_free(&jobs);
            return 1;
        }
        if (tokens.length == 0) {
            printf("%s", PROMPT);
            continue;
        }

        const char *first_token = strvec_get(&tokens, 0);

        if (strcmp(first_token, "pwd") == 0) {
            char cwd[CMD_LEN];
            if (getcwd(cwd, sizeof(cwd)) == NULL) {
                perror("getcwd");
            } else {
                printf("%s\n", cwd);
            }

        } else if (strcmp(first_token, "cd") == 0) {
            const char *dest;
            if (tokens.length == 1) {
                dest = getenv("HOME");
                if (dest == NULL) {
                    fprintf(stderr, "cd: HOME not set\n");
                    strvec_clear(&tokens);
                    printf("%s", PROMPT);
                    continue;
                }
            } else {
                dest = strvec_get(&tokens, 1);
            }
            if (chdir(dest) != 0) {
                perror("chdir");
            }

        } else if (strcmp(first_token, "exit") == 0) {
            strvec_clear(&tokens);
            break;

        } else if (strcmp(first_token, "jobs") == 0) {
            int i = 0;
            job_t *current = jobs.head;
            while (current != NULL) {
                char *status_desc = current->status == BACKGROUND ? "background" : "stopped";
                printf("%d: %s (%s)\n", i, current->name, status_desc);
                i++;
                current = current->next;
            }

        } else if (strcmp(first_token, "fg") == 0) {
            if (resume_job(&tokens, &jobs, 1) == -1) {
                printf("Failed to resume job in foreground\n");
            }

        } else if (strcmp(first_token, "bg") == 0) {
            if (resume_job(&tokens, &jobs, 0) == -1) {
                printf("Failed to resume job in background\n");
            }

        } else if (strcmp(first_token, "wait-for") == 0) {
            if (await_background_job(&tokens, &jobs) == -1) {
                printf("Failed to wait for background job\n");
            }

        } else if (strcmp(first_token, "wait-all") == 0) {
            if (await_all_background_jobs(&jobs) == -1) {
                printf("Failed to wait for all background jobs\n");
            }

        } else {
            int run_in_background = 0;
            if (strcmp(strvec_get(&tokens, tokens.length - 1), "&") == 0) {
                run_in_background = 1;
                strvec_take(&tokens, tokens.length - 1);
            }

            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
            } else if (pid == 0) {
                // CHILD
                if (run_command(&tokens) == -1) {
                    exit(1);
                }
                exit(0);
            } else {
                // PARENT
                if (run_in_background) {
                    job_list_add(&jobs, pid, strvec_get(&tokens, 0), BACKGROUND);
                } else {
                    if (tcsetpgrp(STDIN_FILENO, pid) == -1) {
                        perror("tcsetpgrp (child fg)");
                    }

                    int status;
                    if (waitpid(pid, &status, WUNTRACED) == -1) {
                        perror("waitpid");
                    }

                    if (tcsetpgrp(STDIN_FILENO, getpid()) == -1) {
                        perror("tcsetpgrp (shell fg)");
                    }

                    if (WIFSTOPPED(status)) {
                        job_list_add(&jobs, pid, strvec_get(&tokens, 0), STOPPED);
                    }
                }
            }
        }

        strvec_clear(&tokens);
        printf("%s", PROMPT);
    }

    strvec_clear(&tokens);
    job_list_free(&jobs);
    return 0;
}
