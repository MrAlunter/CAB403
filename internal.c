#include <stdio.h>
#include <string.h>
#include <fcntl.h>    // For O_RDWR
#include <sys/mman.h> // For shm_open, mmap
#include <unistd.h>   // For close
#include "shared.h"

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

        if (shm_ptr == MAP_FAILED)
        {
            printf("Memory mapping failed.\n");
            return 1;
        }
    }

    else if (strcmp(operation, "close") == 0)
    {
        // This is where we'll put the code to handle "open" operation
        pthread_mutex_lock(&shm_ptr->mutex);
        shm_ptr->close_button = 1;
        pthread_cond_broadcast(&shm_ptr->cond);
        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    else if (strcmp(operation, "stop") == 0)
    {
        // This is where we'll put the code to handle "open" operation
        pthread_mutex_lock(&shm_ptr->mutex);
        shm_ptr->emergency_stop = 1;
        pthread_cond_broadcast(&shm_ptr->cond);
        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    else if (strcmp(operation, "service_on") == 0)
    {
        // This is where we'll put the code to handle "open" operation

        pthread_mutex_lock(&shm_ptr->mutex);
        shm_ptr->individual_service_mode = 1;
        pthread_cond_broadcast(&shm_ptr->cond);
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
