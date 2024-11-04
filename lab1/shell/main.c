#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>

#define MAX_LINE 80  // Maximum length of command

volatile sig_atomic_t child_pid = 0;  // To store the child process ID
volatile sig_atomic_t is_paused = 0;  // To track if the process is paused

// Signal handler function for SIGTSTP (Ctrl+Z)
void handle_sig(int sig) {
    if (sig == SIGTSTP) {
        if (is_paused == 0) {
            // Process is running, pause it by sending SIGSTOP
            printf("Pausing process with PID %d (Ctrl+Z pressed)\n", child_pid);
            kill(child_pid, SIGSTOP);  // Send SIGSTOP to pause the child process
            is_paused = 1;
        } else {
            // Process is paused, resume it by sending SIGCONT
            printf("Resuming process with PID %d (Ctrl+Z pressed again)\n", child_pid);
            kill(child_pid, SIGCONT);  // Send SIGCONT to resume the child process
            is_paused = 0;
        }
    }
}

// Function to execute a command
void execute_command(char *args[]) {
    struct timeval start, end;
    pid_t pid = fork();  // Fork a child process

    if (pid < 0) {
        // Fork failed
        perror("Fork failed");
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        // In child process, execute the command
        if (execvp(args[0], args) == -1) {
            perror("Execution failed");
        }
        exit(EXIT_FAILURE);
    } else {
        // Parent process stores child PID for later signaling
        child_pid = pid;
        is_paused = 0;  // Process starts in running state

        // Parent process waits for the child to finish
        gettimeofday(&start, NULL);
        while (1) {
            int status;
            pid_t result = waitpid(pid, &status, WUNTRACED | WCONTINUED);
            if (result == -1) {
                perror("waitpid failed");
                exit(EXIT_FAILURE);
            }

            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                // Process exited or was terminated
                break;
            }

            if (WIFSTOPPED(status)) {
                // Process stopped (paused)
                printf("Process with PID %d stopped (paused).\n", pid);
            }

            if (WIFCONTINUED(status)) {
                // Process resumed
                printf("Process with PID %d continued (resumed).\n", pid);
            }
        }
        gettimeofday(&end, NULL);
        long seconds = (end.tv_sec - start.tv_sec);
        long microseconds = (end.tv_usec - start.tv_usec);
        double elapsed = seconds + microseconds * 1e-6;
        // Print the execution time
        printf("Execution time: %.6f seconds\n", elapsed);
    }
}

int main(void) {
    char *args[MAX_LINE / 2 + 1];  // Command line arguments
    char input[MAX_LINE];          // User input

    // Set up signal handler for SIGTSTP (Ctrl+Z)
    signal(SIGTSTP, handle_sig);

    while (1) {
        printf("myshell> ");
        fflush(stdout);

        // Read user input
        if (!fgets(input, MAX_LINE, stdin)) {
            break;  // Exit on Ctrl+D (EOF)
        }

        // Remove newline character from input
        size_t length = strlen(input);
        if (input[length - 1] == '\n') {
            input[length - 1] = '\0';
        }

        // Tokenize input into command and arguments
        char *token = strtok(input, " ");
        int i = 0;
        while (token != NULL) {
            args[i] = token;
            i++;
            token = strtok(NULL, " ");
        }
        args[i] = NULL;

        // Exit shell on "exit" command
        if (strcmp(args[0], "exit") == 0) {
            break;
        }

        // Execute the command
        execute_command(args);
    }

    return 0;
}
