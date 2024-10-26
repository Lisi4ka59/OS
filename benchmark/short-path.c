#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#define V 10 // Увеличенное количество вершин в графе

int graph[V][V] = {
        {0, 10, 20, 0, 0, 0, 0, 0, 0, 0},
        {10, 0, 30, 5, 0, 0, 0, 0, 0, 0},
        {20, 30, 0, 15, 6, 0, 0, 0, 0, 0},
        {0, 5, 15, 0, 8, 0, 0, 0, 0, 0},
        {0, 0, 6, 8, 0, 12, 0, 0, 0, 0},
        {0, 0, 0, 0, 12, 0, 7, 0, 0, 0},
        {0, 0, 0, 0, 0, 7, 0, 9, 0, 0},
        {0, 0, 0, 0, 0, 0, 9, 0, 11, 0},
        {0, 0, 0, 0, 0, 0, 0, 11, 0, 13},
        {0, 0, 0, 0, 0, 0, 0, 0, 13, 0}
};

int minPath = INT_MAX;

void findShortestPath(int current, int visited[], int pathLength, int start) {
    visited[current] = 1;

    int allVisited = 1;
    for (int i = 0; i < V; i++) {
        if (!visited[i] && graph[current][i] != 0) {
            allVisited = 0;
            findShortestPath(i, visited, pathLength + graph[current][i], start);
        }
    }

    if (allVisited) {
        pathLength += graph[current][start];
        if (pathLength < minPath) {
            minPath = pathLength;
        }
    }

    visited[current] = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <repetitions>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int repetitions = atoi(argv[1]);

    for (int rep = 0; rep < repetitions; rep++) {
        int visited[V] = {0};
        minPath = INT_MAX;
        findShortestPath(0, visited, 0, 0);
        //printf("Repetition %d: Shortest path length is %d\n", rep, minPath);
    }

    return 0;
}
