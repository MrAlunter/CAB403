#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "shared.h"

#define CONTROLLER_PORT 3000
#define CONTROLLER_IP "127.0.0.1"
#define BUFFER_SIZE 1024

// --- Global variable for signal handler ---
char *g_shm_name = NULL;

// --- Signal handler function ---
void handle_sigint(int sig)
{
    printf("\nCtrl+C detected. Cleaning up shared memory...\n");
    if (g_shm_name != NULL)
    {
        shm_unlink(g_shm_name);
    }
    exit(0);
}

// --- Struct for thread arguments ---
typedef struct
{
    car_shared_mem *shm_ptr;
    char *car_name;
    char *lowest_floor_str;
    char *highest_floor_str;
    int delay;
} network_thread_args;

// --- Network thread function ---
void *network_thread_function(void *args)
{
    network_thread_args *thread_args = (network_thread_args *)args;
    car_shared_mem *shm_ptr = thread_args->shm_ptr;
    int delay_ms = thread_args->delay;
    int sockfd;

    // --- Connection Loop ---
    while (1)
    {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd == -1)
        {
            perror("socket");
            sleep(1);
            continue;
        }

        struct sockaddr_in server_address;
        memset(&server_address, 0, sizeof(server_address));
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(CONTROLLER_PORT);
        inet_pton(AF_INET, CONTROLLER_IP, &server_address.sin_addr);

        if (connect(sockfd, (struct sockaddr *)&server_address, sizeof(server_address)) == -1)
        {
            printf("Car '%s' failed to connect. Retrying in %dms...\n", thread_args->car_name, delay_ms);
            close(sockfd);
            usleep(delay_ms * 1000);
            continue;
        }

        else
        {
            printf("Car '%s' connected to controller.\n", thread_args->car_name);
            break;
        }
    }

    // --- Registration Message (sent once) ---
    char message_buffer[BUFFER_SIZE];
    sprintf(message_buffer, "CAR %s %s %s",
            thread_args->car_name,
            thread_args->lowest_floor_str,
            thread_args->highest_floor_str);

    uint16_t len = strlen(message_buffer);
    uint16_t net_len = htons(len);
    send(sockfd, &net_len, sizeof(net_len), 0);
    send(sockfd, message_buffer, len, 0);
    printf("Registered with controller: [%s]\n", message_buffer);

    // --- Status Update Loop ---
    while (1)
    {
        usleep(delay_ms * 1000);

        pthread_mutex_lock(&shm_ptr->mutex);

        char status_message[BUFFER_SIZE];
        sprintf(status_message, "STATUS %s %s %s",
                shm_ptr->status,
                shm_ptr->current_floor,
                shm_ptr->destination_floor);

        pthread_mutex_unlock(&shm_ptr->mutex);

        len = strlen(status_message);
        net_len = htons(len);
        send(sockfd, &net_len, sizeof(net_len), 0);
        send(sockfd, status_message, len, 0);
    }

    free(args); // Clean up the arguments struct
    close(sockfd);
    return NULL;
}

// --- Main Function ---
int main(int argc, char *argv[])
{
    // 1. Register signal handler
    signal(SIGINT, handle_sigint);

    // 2. Parse arguments
    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s <name> <lowest> <highest> <delay>\n", argv[0]);
        return 1;
    }
    char *car_name = argv[1];
    int delay = atoi(argv[4]);

    // Set up and create shared memory
    char shm_name[50];
    sprintf(shm_name, "/car%s", car_name);
    g_shm_name = shm_name;

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (fd == -1)
    {
        perror("shm_open");
        return 1;
    }
    if (ftruncate(fd, sizeof(car_shared_mem)) == -1)
    {
        perror("ftruncate");
        return 1;
    }
    car_shared_mem *shm_ptr = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_ptr == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }

    // Initialize shared memory
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_condattr_init(&cond_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm_ptr->mutex, &mutex_attr);
    pthread_cond_init(&shm_ptr->cond, &cond_attr);
    strcpy(shm_ptr->current_floor, argv[2]);
    strcpy(shm_ptr->destination_floor, argv[2]);
    strcpy(shm_ptr->status, "Closed");
    shm_ptr->open_button = 0;
    shm_ptr->close_button = 0;
    shm_ptr->safety_system = 0;
    shm_ptr->door_obstruction = 0;
    shm_ptr->overload = 0;
    shm_ptr->emergency_stop = 0;
    shm_ptr->individual_service_mode = 0;
    shm_ptr->emergency_mode = 0;
    printf("Shared memory for car '%s' created and initialized.\n", car_name);

    // Start the network thread
    network_thread_args *args = malloc(sizeof(network_thread_args));
    args->shm_ptr = shm_ptr;
    args->car_name = car_name;
    args->lowest_floor_str = argv[2];
    args->highest_floor_str = argv[3];
    args->delay = delay;

    pthread_t network_thread_id;
    if (pthread_create(&network_thread_id, NULL, network_thread_function, args) != 0)
    {
        perror("pthread_create");
        return 1;
    }

    // Start the main local control loop
    printf("Car '%s' is now running. Press Ctrl+C to exit.\n", car_name);
    while (1)
    {
        pthread_mutex_lock(&shm_ptr->mutex);
        pthread_cond_wait(&shm_ptr->cond, &shm_ptr->mutex);

        if (shm_ptr->open_button == 1)
        {
            shm_ptr->open_button = 0;
            printf("Car '%s': Open button pressed.\n", car_name);
            // Full door logic would go here
        }
        else if (shm_ptr->close_button == 1)
        {
            shm_ptr->close_button = 0;
            printf("Car '%s': Close button pressed.\n", car_name);
            
        }

        pthread_mutex_unlock(&shm_ptr->mutex);
    }

    return 0; // Will not be reached
}