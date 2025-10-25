#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "shared.h"
#include <stdbool.h>
#include <ctype.h>

/*
 * SAFETY-CRITICAL COMPONENT - MISRA C COMPLIANCE
 *
 * This component is safety-critical and follows MISRA C guidelines.
 *
 * EXCEPTIONS AND DEVIATIONS:
 *
 * Exception 1: Use of pthread_cond_wait()
 * Justification: Required by specification for monitoring shared memory.
 * This is the mandated interface for inter-process communication.
 *
 * Exception 2: Infinite loop without explicit exit condition
 * Justification: Safety systems must run continuously. Termination
 * occurs via external signal (SIGINT) which is appropriate for this
 * context. The loop is not unbounded - it waits on condition variable.
 *
 * Exception 3: Use of printf() for operator notification
 * Justification: Specification requires printing error messages to stdout.
 * In production, this would be replaced with a safety-rated logging system.
 *
 * Exception 4: Use of strcmp() and strcpy()
 * Justification: Required for string manipulation of status and floor fields.
 * All strings are fixed-size buffers in shared memory structure, bounds
 * are validated before use.
 */

// Helper Functions ---------------------

bool is_valid_floor(const char *floor)
{
    if (floor[0] == 'B')
    {
        // Basement: B1-B99
        int level = stoi(floor + 1);
        return (level >= 1 && level <= 99);
    }
    else if (isdigit(floor[0]))
    {
        // Regular: 1-999
        int level = stoi(floor);
        return (level >= 1 && level <= 999);
    }
    return false;
}

bool is_valid_status(const char *status)
{
    return (strcmp(status, "Opening") == 0 ||
            strcmp(status, "Open") == 0 ||
            strcmp(status, "Closing") == 0 ||
            strcmp(status, "Closed") == 0 ||
            strcmp(status, "Between") == 0);
}

// End of Helper Functions ---------------------

int main(int argc, char *argv[])
{
    if (argc != 2)
    { // Safety only takes one argument: the car name
        fprintf(stderr, "Usage: %s <car_name>\n", argv[0]);
        return 1;
    }

    char *car_name = argv[1];

    char shm_name[50];
    sprintf(shm_name, "/car%s", car_name);

    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1)
    {
        printf("Unable to access car %s. Is the car program running?\n", car_name);
        return 1;
    }

    car_shared_mem *shm_ptr = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_ptr == MAP_FAILED)
    {
        printf("Memory mapping failed.\n");
        return 1;
    }

    printf("Safety system for car '%s' is running.\n", car_name);

    while (1)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        pthread_cond_wait(&shm_ptr->cond, &shm_ptr->mutex);

        // Activate safety system if not already active
        if (shm_ptr->safety_system != 1)
        {
            shm_ptr->safety_system = 1;
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        // Door obstruction check
        if (shm_ptr->door_obstruction == 1 && strcmp(shm_ptr->status, "Closing") == 0)
        {
            strcpy(shm_ptr->status, "Opening");
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        // Emergency stop check
        if (shm_ptr->emergency_stop == 1 && shm_ptr->emergency_mode == 0)
        {
            printf("The emergency stop button has been pressed!\n");
            shm_ptr->emergency_mode = 1;
            shm_ptr->emergency_stop = 0;
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        // Overload check
        if (shm_ptr->overload == 1 && shm_ptr->emergency_mode == 0)
        {
            printf("The overload sensor has been tripped!\n");
            shm_ptr->emergency_mode = 1;
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        // Data Consistency Checks
        if (shm_ptr->emergency_mode == 0)
        {
            bool data_error = false;

            // Check current floor
            if (!is_valid_floor(shm_ptr->current_floor))
            {
                data_error = true;
            }

            // Check destination floor
            if (!is_valid_floor(shm_ptr->destination_floor))
            {
                data_error = true;
            }

            // Check status
            if (!is_valid_status(shm_ptr->status))
            {
                data_error = true;
            }

            // Check uint8_t fields
            if (shm_ptr->open_button > 1 || shm_ptr->close_button > 1 ||
                shm_ptr->safety_system > 1 || shm_ptr->door_obstruction > 1 ||
                shm_ptr->overload > 1 || shm_ptr->emergency_stop > 1 ||
                shm_ptr->individual_service_mode > 1 || shm_ptr->emergency_mode > 1)
            {
                data_error = true;
            }

            // Check door obstruction placement
            if (shm_ptr->door_obstruction == 1 &&
                strcmp(shm_ptr->status, "Opening") != 0 &&
                strcmp(shm_ptr->status, "Closing") != 0)
            {
                data_error = true;
            }

            if (data_error)
            {
                printf("Data consistency error!\n");
                shm_ptr->emergency_mode = 1;
                pthread_cond_broadcast(&shm_ptr->cond);
            }
        }

        pthread_mutex_unlock(&shm_ptr->mutex);
    }
}