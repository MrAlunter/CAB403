#include <stdio.h>
#include <string.h>
#include <fcntl.h>    // For O_RDWR
#include <sys/mman.h> // For shm_open, mmap
#include <unistd.h>   // For close
#include "shared.h"

// Helper function to convert floor string to integer------------------------------

// floor_to_int: convert floor string to integer (B1 -> -1)
int floor_to_int(const char *floor_str)
{
    if (floor_str[0] == 'B')
    {
        return -atoi(&floor_str[1]);
    }
    else
    {
        return atoi(floor_str);
    }
}

// int_to_floor: write textual floor string for integer (e.g. -1 -> "B1")
void int_to_floor(int floor_num, char *floor_str)
{
    if (floor_num < 0)
    {
        sprintf(floor_str, "B%d", -floor_num);
    }
    else
    {
        sprintf(floor_str, "%d", floor_num);
    }
}

// End of helper functions ---------------------

// main: CLI that manipulates shared memory to control a car (open/close/etc.)
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <car_name> <operation>\n", argv[0]);
        return 1;
    }

    char *car_name = argv[1];
    char *operation = argv[2];
    printf("Attempting to control car '%s' with operation '%s'\n", car_name, operation);
    char shm_name[50];
    // printf("Internal is looking for memory named: [%s]\n", shm_name); // Debugging output
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

    else if (strcmp(operation, "open") == 0)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        shm_ptr->open_button = 1;
        pthread_cond_broadcast(&shm_ptr->cond);
        pthread_mutex_unlock(&shm_ptr->mutex);
        printf("Signalled car %s to open doors.\n", car_name);
    }

    else if (strcmp(operation, "close") == 0)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        shm_ptr->close_button = 1;
        pthread_cond_broadcast(&shm_ptr->cond);
        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    else if (strcmp(operation, "stop") == 0)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        shm_ptr->emergency_stop = 1;
        pthread_cond_broadcast(&shm_ptr->cond);
        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    else if (strcmp(operation, "service_on") == 0)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        shm_ptr->individual_service_mode = 1;
        shm_ptr->emergency_mode = 0;
        pthread_cond_broadcast(&shm_ptr->cond);
        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    else if (strcmp(operation, "service_off") == 0)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        shm_ptr->individual_service_mode = 0;
        pthread_cond_broadcast(&shm_ptr->cond);
        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    else if (strcmp(operation, "up") == 0)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        if (shm_ptr->individual_service_mode != 1)
        {
            printf("Operation only allowed in service mode.\n");
            pthread_mutex_unlock(&shm_ptr->mutex);
            munmap(shm_ptr, sizeof(car_shared_mem));
            close(fd);
            return 1;
        }

        if (strcmp(shm_ptr->status, "Closed") != 0)
        {
            printf("Operation not allowed while doors are open.\n");
            pthread_mutex_unlock(&shm_ptr->mutex);
            munmap(shm_ptr, sizeof(car_shared_mem));
            close(fd);
            return 1;
        }

        if (strcmp(shm_ptr->status, "Between") == 0)
        {
            printf("Operation not allowed while elevator is moving.\n");
            pthread_mutex_unlock(&shm_ptr->mutex);
            munmap(shm_ptr, sizeof(car_shared_mem));
            close(fd);
            return 1;
        }

        int current_floor = floor_to_int(shm_ptr->current_floor);
        int destination_floor = current_floor + 1;
        if (destination_floor == 0)
        {
            destination_floor = 1; // B1 -> 1
        }
        char dest_floor_str[4];
        int_to_floor(destination_floor, dest_floor_str);
        strcpy(shm_ptr->destination_floor, dest_floor_str);
        pthread_cond_broadcast(&shm_ptr->cond);
        printf("Signalled car %s to move up to floor %s.\n", car_name, dest_floor_str);
        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    else if (strcmp(operation, "down") == 0)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        if (shm_ptr->individual_service_mode != 1)
        {
            printf("Operation only allowed in service mode.\n");
            pthread_mutex_unlock(&shm_ptr->mutex);
            munmap(shm_ptr, sizeof(car_shared_mem));
            close(fd);
            return 1;
        }

        if (strcmp(shm_ptr->status, "Closed") != 0)
        {
            printf("Operation not allowed while doors are open.\n");
            pthread_mutex_unlock(&shm_ptr->mutex);
            munmap(shm_ptr, sizeof(car_shared_mem));
            close(fd);
            return 1;
        }

        if (strcmp(shm_ptr->status, "Between") == 0)
        {
            printf("Operation not allowed while elevator is moving.\n");
            pthread_mutex_unlock(&shm_ptr->mutex);
            munmap(shm_ptr, sizeof(car_shared_mem));
            close(fd);
            return 1;
        }
        int current_floor = floor_to_int(shm_ptr->current_floor);
        int destination_floor = current_floor - 1; // Move down one floor
        if (destination_floor == 0)
        {
            destination_floor = -1; // 1 -> B1
        }
        char dest_floor_str[4];
        int_to_floor(destination_floor, dest_floor_str);
        strcpy(shm_ptr->destination_floor, dest_floor_str);
        pthread_cond_broadcast(&shm_ptr->cond);
        printf("Signalled car %s to move down to floor %s.\n", car_name, dest_floor_str);
        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    else
    {
        // If the operation is not recognized, print an error.
        printf("Invalid operation: %s\n", operation);
        return 1; // Exit with an error code
    }
    printf("Operation '%s' executed on car '%s'\n", operation, car_name);
    munmap(shm_ptr, sizeof(car_shared_mem));
    close(fd);
    return 0;
}
