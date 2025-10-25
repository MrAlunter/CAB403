#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include "shared.h"

/* Safety System Return Codes */
#define SAFETY_SUCCESS 0
#define SAFETY_ERROR 1
#define SAFETY_ERROR_ARGS 2
#define SAFETY_ERROR_SHM 3
#define SAFETY_ERROR_MAP 4

/* Validation Constants */
#define MAX_FLOOR 999
#define MIN_BASEMENT 1
#define MAX_BASEMENT 99
#define MAX_CAR_NAME 45
#define SHM_NAME_PREFIX "/car"

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

static bool is_valid_floor(const char *const floor)
{
    /* MISRA C: Rule 17.7 - Check pointer validity */
    if (floor == NULL)
    {
        return false;
    }

    /* MISRA C: Rule 13.2 - Explicit test of value returned from function */
    const size_t len = strlen(floor);
    if (len == 0U || len > 4U) /* Max length: B99 or 999 */
    {
        return false;
    }

    if (floor[0] == 'B')
    {
        /* Basement: B1-B99 */
        int32_t level;
        if (1 != sscanf(floor + 1, "%d", &level))
        {
            return false;
        }
        return ((level >= MIN_BASEMENT) && (level <= MAX_BASEMENT));
    }
    else if (isdigit((unsigned char)floor[0]))
    {
        /* Regular: 1-999 */
        int32_t level;
        if (1 != sscanf(floor, "%d", &level))
        {
            return false;
        }
        return ((level >= 1) && (level <= MAX_FLOOR));
    }
    return false;
}

//  Valid elevator status strings
static const char *const VALID_STATUSES[] = {
    "Opening",
    "Open",
    "Closing",
    "Closed",
    "Between",
    NULL};

static bool is_valid_status(const char *const status)
{
    // MISRA C: Rule 17.7 - Check pointer validity
    if (status == NULL)
    {
        return false;
    }

    //  MISRA C: Rule 14.4 - No assignment in conditional
    for (const char *const *curr = VALID_STATUSES; *curr != NULL; ++curr)
    {
        if (0 == strcmp(status, *curr))
        {
            return true;
        }
    }
    return false;
}

// End of Helper Functions ---------------------

static int validate_args(const int argc, char *const argv[], const char **car_name)
{

    if ((argv == NULL) || (car_name == NULL))
    {
        return SAFETY_ERROR_ARGS;
    }

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <car_name>\n", argv[0]);
        return SAFETY_ERROR_ARGS;
    }

    if (argv[1] == NULL)
    {
        fprintf(stderr, "Car name cannot be NULL\n");
        return SAFETY_ERROR_ARGS;
    }

    /* Validate car name length */
    if (strlen(argv[1]) >= MAX_CAR_NAME)
    {
        fprintf(stderr, "Car name too long\n");
        return SAFETY_ERROR_ARGS;
    }

    *car_name = argv[1];
    return SAFETY_SUCCESS;
}

int main(int argc, char *argv[])
{
    const char *car_name = NULL;
    int result = validate_args(argc, argv, &car_name);
    if (result != SAFETY_SUCCESS)
    {
        return result;
    }

    // Construct shared memory name
    char shm_name[MAX_CAR_NAME + sizeof(SHM_NAME_PREFIX)];
    if (snprintf(shm_name, sizeof(shm_name), "%s%s", SHM_NAME_PREFIX, car_name) < 0)
    {
        fprintf(stderr, "Error constructing shared memory name\n");
        return SAFETY_ERROR_SHM;
    }

    // Open shared memory
    const int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1)
    {
        fprintf(stderr, "Unable to access car %s. Is the car program running?\n", car_name);
        return SAFETY_ERROR_SHM;
    }

    // Map shared memory
    car_shared_mem *const shm_ptr = mmap(NULL, sizeof(car_shared_mem),
                                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_ptr == MAP_FAILED)
    {
        fprintf(stderr, "Memory mapping failed: %s\n", strerror(errno));
        close(fd);
        return SAFETY_ERROR_MAP;
    }

    //  Close fd after successful mmap
    close(fd);

    printf("Safety system for car '%s' is running.\n", car_name);

    // MISRA C Exception: Infinite loop for safety system
    for (;;)
    {
        //  MISRA C Exception: Use of pthread functions
        const int lock_result = pthread_mutex_lock(&shm_ptr->mutex);
        if (lock_result != 0)
        {
            fprintf(stderr, "Mutex lock failed: %s\n", strerror(lock_result));
            continue;
        }

        // MISRA C Exception: Use of pthread functions
        const int wait_result = pthread_cond_wait(&shm_ptr->cond, &shm_ptr->mutex);
        if (wait_result != 0)
        {
            pthread_mutex_unlock(&shm_ptr->mutex);
            fprintf(stderr, "Condition wait failed: %s\n", strerror(wait_result));
            continue;
        }

        //  Activate safety system if not already active
        if (shm_ptr->safety_system != 1U)
        {
            shm_ptr->safety_system = 1U;
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        //  Door obstruction check
        if ((shm_ptr->door_obstruction == 1U) &&
            (0 == strcmp(shm_ptr->status, "Closing")))
        {
            const size_t status_len = strlen("Opening") + 1U;
            if (status_len <= sizeof(shm_ptr->status))
            {
                memcpy(shm_ptr->status, "Opening", status_len);
                pthread_cond_broadcast(&shm_ptr->cond);
            }
        }

        //  Emergency stop check
        if ((shm_ptr->emergency_stop == 1U) &&
            (shm_ptr->emergency_mode == 0U))
        {
            fprintf(stderr, "Emergency stop button pressed!\n");
            shm_ptr->emergency_mode = 1U;
            shm_ptr->emergency_stop = 0U;
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        // Overload check
        if ((shm_ptr->overload == 1U) &&
            (shm_ptr->emergency_mode == 0U))
        {
            fprintf(stderr, "Overload sensor tripped!\n");
            shm_ptr->emergency_mode = 1U;
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        // Data Consistency Checks
        if (shm_ptr->emergency_mode == 0U)
        {
            bool data_error = false;

            //  Check floor validity
            if ((!is_valid_floor(shm_ptr->current_floor)) ||
                (!is_valid_floor(shm_ptr->destination_floor)))
            {
                data_error = true;
            }

            // Check status validity
            if (!is_valid_status(shm_ptr->status))
            {
                data_error = true;
            }

            // Check boolean fields are binary
            const uint8_t bool_mask =
                (shm_ptr->open_button |
                 shm_ptr->close_button |
                 shm_ptr->safety_system |
                 shm_ptr->door_obstruction |
                 shm_ptr->overload |
                 shm_ptr->emergency_stop |
                 shm_ptr->individual_service_mode |
                 shm_ptr->emergency_mode);

            if (bool_mask > 1U)
            {
                data_error = true;
            }

            // Check door obstruction state consistency
            if ((shm_ptr->door_obstruction == 1U) &&
                (0 != strcmp(shm_ptr->status, "Opening")) &&
                (0 != strcmp(shm_ptr->status, "Closing")))
            {
                data_error = true;
            }

            if (data_error)
            {
                fprintf(stderr, "Data consistency error!\n");
                shm_ptr->emergency_mode = 1U;
                pthread_cond_broadcast(&shm_ptr->cond);
            }
        }

        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    // Cleanup on exit - should not be reached due to infinite loop
    const int unmap_result = munmap(shm_ptr, sizeof(car_shared_mem));
    if (unmap_result != 0)
    {
        fprintf(stderr, "Memory unmap failed: %s\n", strerror(errno));
        return SAFETY_ERROR;
    }
    return SAFETY_SUCCESS;
}