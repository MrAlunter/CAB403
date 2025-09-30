#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "shared.h"

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

        // Set it back to 1 if it's not 1 (this is the heartbeat)
        if (shm_ptr->safety_system != 1)
        {
            shm_ptr->safety_system = 1;
            pthread_cond_broadcast(&shm_ptr->cond);
        }
        pthread_cond_broadcast(&shm_ptr->cond);

        // Door obstruction check
        if (shm_ptr->door_obstruction == 1 && strcmp(shm_ptr->status, "Closing") == 0)
        {
            strcpy(shm_ptr->status, "Opening");
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        // Emergency stop check
        if (shm_ptr->emergency_stop == 1)
        {
            printf("SAFETY: Emergency stop for car %s!\n", car_name);
            shm_ptr->emergency_mode = 1;
            shm_ptr->emergency_stop = 0;
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        // Overload check
        if (shm_ptr->overload == 1)
        {
            printf("SAFETY: Overload detected for car %s!\n", car_name);
            shm_ptr->emergency_mode = 1;
            shm_ptr->overload = 0;
            pthread_cond_broadcast(&shm_ptr->cond);
        }

        pthread_mutex_unlock(&shm_ptr->mutex);
        usleep(10000); //
    }
}