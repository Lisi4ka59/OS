#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>

#define MAX_LINE 80  // Maximum length of command

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
        // Parent process waits for the child to finish
	gettimeofday(&start, NULL);
        wait(NULL);
	gettimeofday(&end, NULL);
	long seconds = (end.tv_sec - start.tv_sec);
        long microseconds = (end.tv_usec - start.tv_usec);
        double elapsed = seconds + microseconds*1e-6;
	// Print the execution time
        printf("Execution time: %.6f seconds\n", elapsed);
    }
}

int main() {
    char *args[MAX_LINE/2 + 1];  // Command line arguments
    char input[MAX_LINE];        // User input

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
