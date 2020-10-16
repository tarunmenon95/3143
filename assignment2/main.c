#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpi.h"

#define GRID_DIMENSIONS 2
#define INTERVAL_MILLISECONDS 1000
#define READING_THRESHOLD 80
#define MAX_READING_VALUE 100
#define SECONDS_TO_NANOSECONDS 1000000000

typedef struct {
    int coords[2];
    int reading;
    double timestamp;
} SatelliteReading;

// pointer to next spot to replace in infrared_readings array
int infrared_readings_latest = 0;
SatelliteReading infrared_readings[100];

int main(int argc, char* argv[]) {
    int rows, cols, max_iterations;
    int world_rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    // unique seed per process
    srand(time(NULL) + world_rank);

    // user must specify grid dimensions and max iterations in commandline args
    if (argc != 4) {
        // to avoid spamming with every process output
        if (world_rank == 0)
            printf("Usage: %s num_rows num_cols max_iterations\n", argv[0]);
        MPI_Finalize();
        exit(0);
    }

    // parse these dimensions, ensure are proper
    char* ptr;
    rows = (int)strtol(argv[1], &ptr, 10);
    if (ptr == argv[1]) {
        if (world_rank == 0)
            printf("Couldn't parse to a number: %s\n", argv[1]);
        MPI_Finalize();
        exit(0);
    }
    ptr = NULL;
    cols = (int)strtol(argv[2], &ptr, 10);
    if (ptr == argv[2]) {
        if (world_rank == 0)
            printf("Couldn't parse to a number: %s\n", argv[2]);
        MPI_Finalize();
        exit(0);
    }
    ptr = NULL;
    max_iterations = (int)strtol(argv[3], &ptr, 10);
    if (ptr == argv[3]) {
        if (world_rank == 0)
            printf("Couldn't parse to a number: %s\n", argv[3]);
        MPI_Finalize();
        exit(0);
    }
    if (rows < 1 || cols < 1) {
        if (world_rank == 0) printf("Rows and cols must be larger than 0\n");
        MPI_Finalize();
        exit(0);
    }
    // ensure enough processes in total (grid + 1 base station)
    if (rows * cols + 1 != size) {
        if (world_rank == 0)
            printf(
                "Must run with (rows * cols + 1) processes instead of %d "
                "processes\n",
                size);
        MPI_Finalize();
        exit(0);
    }

    int dimension_sizes[2] = {rows, cols};
    MPI_Comm grid_comm;
    int periods[2] = {0, 0};  // don't wrap around on any dimension
    int reorder = 1;
    MPI_Cart_create(MPI_COMM_WORLD, GRID_DIMENSIONS, dimension_sizes, periods,
                    reorder, &grid_comm);

    int base_station_world_rank = size - 1;
    if (grid_comm == MPI_COMM_NULL) {
        // base station
        double start_time, end_time, sleep_length;
        struct timespec ts;
        int iteration = 0;
        while (iteration < max_iterations) {
            start_time = MPI_Wtime();
            ++iteration;
            printf("Base station iteration: %d\n", iteration);
            end_time = MPI_Wtime();
            sleep_length = (start_time +
                            ((double)INTERVAL_MILLISECONDS / 1000) - end_time) *
                           SECONDS_TO_NANOSECONDS;
            ts.tv_sec = 0;
            ts.tv_nsec = (long)sleep_length;
            nanosleep(&ts, NULL);
        }
        char msg = 't';
        MPI_Request reqs[size - 1];
        for (int i = 0; i < size - 1; ++i)
            MPI_Isend(&msg, 1, MPI_CHAR, i, 0, MPI_COMM_WORLD, &reqs[i]);
        MPI_Waitall(size - 1, reqs, MPI_STATUSES_IGNORE);
        printf("Base station exiting\n");
    } else {
        // ground sensor
        int grid_rank;
        MPI_Comm_rank(grid_comm, &grid_rank);
        int coords[2];
        // get coordinates of this ground sensor
        MPI_Cart_coords(grid_comm, grid_rank, GRID_DIMENSIONS, coords);

        // clockwise starting from above (even if edge/corner case)
        int neighbour_readings[4];

        // to receive single char from base station (terminate sig)
        char base_recv_buffer = '\0';

        // async recv for terminate signal from base
        MPI_Request base_recv_req;
        MPI_Irecv(&base_recv_buffer, 1, MPI_CHAR, base_station_world_rank, 0,
                  MPI_COMM_WORLD, &base_recv_req);

        int recv_completed = 0;
        int reading = 0;
        double start_time, end_time, sleep_length;
        struct timespec ts;
        MPI_Test(&base_recv_req, &recv_completed, MPI_STATUS_IGNORE);
        int iteration = 0;
        while (!recv_completed || base_recv_buffer != 't') {
            start_time = MPI_Wtime();  // seconds
            ++iteration;

            for (int i = 0; i < 4; ++i) neighbour_readings[i] = -1;

            reading = rand() % (1 + MAX_READING_VALUE);

            MPI_Test(&base_recv_req, &recv_completed, MPI_STATUS_IGNORE);

            MPI_Neighbor_allgather(&reading, 1, MPI_INT, neighbour_readings, 1,
                                   MPI_INT, grid_comm);

            printf("[%d] %d:(%d,%d)|%d|%d,%d,%d,%d\n", iteration, grid_rank,
                   coords[0], coords[1], reading, neighbour_readings[0],
                   neighbour_readings[1], neighbour_readings[2],
                   neighbour_readings[3]);

            end_time = MPI_Wtime();  // seconds
            sleep_length = (start_time +
                            ((double)INTERVAL_MILLISECONDS / 1000) - end_time) *
                           SECONDS_TO_NANOSECONDS;
            ts.tv_sec = 0;
            ts.tv_nsec = (long)sleep_length;
            nanosleep(&ts, NULL);
        }
        printf("Rank %d (%d, %d) exiting\n", grid_rank, coords[0], coords[1]);
    }

    MPI_Finalize();
    exit(0);
}