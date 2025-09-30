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
#include <time.h>

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

// Helper function to convert floor string to integer
// B1 = -1, B2 = -2, 1 = 1, 2 = 2, etc.
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

// Helper function to convert integer back to floor string
// -1 = "B1", -2 = "B2", 1 = "1", 2 = "2", etc.
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

// Get the next floor moving toward destination
// Returns the next floor number, or current if already at destination
int get_next_floor(int current, int destination)
{
    if (current < destination)
    {
        return current + 1;
    }
    else if (current > destination)
    {
        return current - 1;
    }
    else
    {
        return current; // Already at destination
    }
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

void *network_thread_function(void *args)
{
    network_thread_args *thread_args = (network_thread_args *)args;
    car_shared_mem *shm_ptr = thread_args->shm_ptr;
    int delay_ms = thread_args->delay;

    while (1) // Outer reconnection loop
    {
        // Wait if in service/emergency mode
        pthread_mutex_lock(&shm_ptr->mutex);
        while (shm_ptr->individual_service_mode == 1 || shm_ptr->emergency_mode == 1)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (delay_ms * 1000000L);
            ts.tv_sec += ts.tv_nsec / 1000000000L;
            ts.tv_nsec = ts.tv_nsec % 1000000000L;
            pthread_cond_timedwait(&shm_ptr->cond, &shm_ptr->mutex, &ts);
        }
        pthread_mutex_unlock(&shm_ptr->mutex);

        // Try to connect (with retries)
        int sockfd;
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

            if (connect(sockfd, (struct sockaddr *)&server_address, sizeof(server_address)) == 0)
            {
                printf("Car '%s' connected to controller.\n", thread_args->car_name);
                break; // Connected successfully
            }

            printf("Car '%s' failed to connect. Retrying in %dms...\n", thread_args->car_name, delay_ms);
            close(sockfd);
            usleep(delay_ms * 1000);
        }

        // Send registration message
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

        // Set socket to non-blocking
        int flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

        // Main communication loop
        int should_disconnect = 0;
        while (!should_disconnect)
        {
            // Check for service modes
            pthread_mutex_lock(&shm_ptr->mutex);
            if (shm_ptr->individual_service_mode == 1)
            {
                pthread_mutex_unlock(&shm_ptr->mutex);
                printf("Entering individual service mode, disconnecting...\n");
                char msg[] = "INDIVIDUAL SERVICE";
                len = strlen(msg);
                net_len = htons(len);
                send(sockfd, &net_len, sizeof(net_len), 0);
                send(sockfd, msg, len, 0);
                close(sockfd);
                should_disconnect = 1;
                break;
            }
            if (shm_ptr->emergency_mode == 1)
            {
                pthread_mutex_unlock(&shm_ptr->mutex);
                printf("Entering emergency mode, disconnecting...\n");
                char msg[] = "EMERGENCY";
                len = strlen(msg);
                net_len = htons(len);
                send(sockfd, &net_len, sizeof(net_len), 0);
                send(sockfd, msg, len, 0);
                close(sockfd);
                should_disconnect = 1;
                break;
            }
            pthread_mutex_unlock(&shm_ptr->mutex);

            // Try to receive messages from controller (non-blocking)
            uint16_t msg_len_network;
            ssize_t bytes_received = recv(sockfd, &msg_len_network, sizeof(msg_len_network), 0);

            if (bytes_received == sizeof(msg_len_network))
            {
                uint16_t msg_len = ntohs(msg_len_network);
                char recv_buffer[BUFFER_SIZE];
                ssize_t total_received = 0;

                while (total_received < msg_len)
                {
                    ssize_t n = recv(sockfd, recv_buffer + total_received, msg_len - total_received, 0);
                    if (n > 0)
                    {
                        total_received += n;
                    }
                    else if (n == 0)
                    {
                        should_disconnect = 1;
                        break;
                    }
                }

                if (should_disconnect)
                {
                    close(sockfd);
                    break;
                }

                recv_buffer[msg_len] = '\0';
                printf("Received from controller: [%s]\n", recv_buffer);

                // Parse FLOOR message
                if (strncmp(recv_buffer, "FLOOR ", 6) == 0)
                {
                    char *floor = recv_buffer + 6;
                    pthread_mutex_lock(&shm_ptr->mutex);
                    strcpy(shm_ptr->destination_floor, floor);
                    pthread_cond_broadcast(&shm_ptr->cond);
                    pthread_mutex_unlock(&shm_ptr->mutex);
                    printf("Set destination floor to: %s\n", floor);
                }
            }

            // Wait before sending status update
            usleep(delay_ms * 1000);

            // Safety check and send status
            pthread_mutex_lock(&shm_ptr->mutex);

            // Increment safety counter if safety system is active
            if (shm_ptr->safety_system == 1)
            {
                shm_ptr->safety_system++;
                if (shm_ptr->safety_system >= 3)
                {
                    printf("Safety system disconnected! Entering emergency mode.\n");
                    shm_ptr->emergency_mode = 1;
                    pthread_cond_broadcast(&shm_ptr->cond);

                    char msg[] = "EMERGENCY";
                    len = strlen(msg);
                    net_len = htons(len);
                    send(sockfd, &net_len, sizeof(net_len), 0);
                    send(sockfd, msg, len, 0);
                    pthread_mutex_unlock(&shm_ptr->mutex);
                    close(sockfd);
                    break;
                }
            }

            // Check again before sending status (race condition protection)
            if (shm_ptr->individual_service_mode == 1 || shm_ptr->emergency_mode == 1)
            {
                pthread_mutex_unlock(&shm_ptr->mutex);
                continue;
            }

            // Send status update
            char status_message[BUFFER_SIZE];
            sprintf(status_message, "STATUS %s %s %s",
                    shm_ptr->status,
                    shm_ptr->current_floor,
                    shm_ptr->destination_floor);

            pthread_mutex_unlock(&shm_ptr->mutex);

            len = strlen(status_message);
            net_len = htons(len);
            if (send(sockfd, &net_len, sizeof(net_len), 0) == -1 ||
                send(sockfd, status_message, len, 0) == -1)
            {
                printf("Failed to send status, disconnecting...\n");
                close(sockfd);
                break;
            }
        }
        // Loop back to reconnection
    }

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
    char *lowest_floor_str = argv[2];
    char *highest_floor_str = argv[3];
    int delay = atoi(argv[4]);

    // Convert floor bounds to integers for validation
    int lowest_floor = floor_to_int(lowest_floor_str);
    int highest_floor = floor_to_int(highest_floor_str);

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

        // Don't do anything in emergency mode
        if (shm_ptr->emergency_mode == 1)
        {
            shm_ptr->open_button = 1; // Open doors in emergency
            pthread_cond_wait(&shm_ptr->cond, &shm_ptr->mutex);
            pthread_mutex_unlock(&shm_ptr->mutex);
            continue;
        }

        // Check if we need to move (don't wait if we're ready to move)
        if (strcmp(shm_ptr->status, "Closed") == 0 &&
            strcmp(shm_ptr->current_floor, shm_ptr->destination_floor) != 0)
        {
            // Ready to move, don't wait
            pthread_mutex_unlock(&shm_ptr->mutex);
        }
        else
        {
            // Only wait if we're not ready to move
            pthread_cond_wait(&shm_ptr->cond, &shm_ptr->mutex);
        }

        // Handle open button
        if (shm_ptr->open_button == 1)
        {
            shm_ptr->open_button = 0;
            strcpy(shm_ptr->status, "Opening");
            pthread_cond_broadcast(&shm_ptr->cond);
            pthread_mutex_unlock(&shm_ptr->mutex);
            usleep(delay * 1000);

            pthread_mutex_lock(&shm_ptr->mutex);
            strcpy(shm_ptr->status, "Open");
            pthread_cond_broadcast(&shm_ptr->cond);

            // Check if in individual service mode
            if (shm_ptr->individual_service_mode == 1)
            {
                pthread_mutex_unlock(&shm_ptr->mutex);
                continue;
            }

            struct timespec ts;
            long nsec = ts.tv_nsec + (delay * 1000000L);
            ts.tv_sec += nsec / 1000000000L;
            ts.tv_nsec = nsec % 1000000000L;

            pthread_cond_timedwait(&shm_ptr->cond, &shm_ptr->mutex, &ts);

            if (shm_ptr->close_button == 1)
            {
                shm_ptr->close_button = 0;
                strcpy(shm_ptr->status, "Closing");
                pthread_cond_broadcast(&shm_ptr->cond);
                pthread_mutex_unlock(&shm_ptr->mutex);
                usleep(delay * 1000);

                pthread_mutex_lock(&shm_ptr->mutex);
                strcpy(shm_ptr->status, "Closed");
                pthread_cond_broadcast(&shm_ptr->cond);
                pthread_mutex_unlock(&shm_ptr->mutex);
                continue;
            }

            strcpy(shm_ptr->status, "Closing");
            pthread_cond_broadcast(&shm_ptr->cond);
            pthread_mutex_unlock(&shm_ptr->mutex);
            usleep(delay * 1000);

            pthread_mutex_lock(&shm_ptr->mutex);
            strcpy(shm_ptr->status, "Closed");
            pthread_cond_broadcast(&shm_ptr->cond);
            pthread_mutex_unlock(&shm_ptr->mutex);
        }
        // Handle close button
        else if (shm_ptr->close_button == 1)
        {
            shm_ptr->close_button = 0;

            if (strcmp(shm_ptr->status, "Open") == 0)
            {
                strcpy(shm_ptr->status, "Closing");
                pthread_cond_broadcast(&shm_ptr->cond);
                pthread_mutex_unlock(&shm_ptr->mutex);
                usleep(delay * 1000);

                pthread_mutex_lock(&shm_ptr->mutex);
                strcpy(shm_ptr->status, "Closed");
                pthread_cond_broadcast(&shm_ptr->cond);
                pthread_mutex_unlock(&shm_ptr->mutex);
            }
            else
            {
                pthread_mutex_unlock(&shm_ptr->mutex);
            }
        }
        // Handle movement
        else if (strcmp(shm_ptr->status, "Closed") == 0 &&
                 strcmp(shm_ptr->current_floor, shm_ptr->destination_floor) != 0)
        {
            int current = floor_to_int(shm_ptr->current_floor);
            int destination = floor_to_int(shm_ptr->destination_floor);

            if (destination < lowest_floor || destination > highest_floor)
            {
                strcpy(shm_ptr->destination_floor, shm_ptr->current_floor);
                pthread_cond_broadcast(&shm_ptr->cond);
                pthread_mutex_unlock(&shm_ptr->mutex);
                continue;
            }

            strcpy(shm_ptr->status, "Between");
            pthread_cond_broadcast(&shm_ptr->cond);
            pthread_mutex_unlock(&shm_ptr->mutex);
            usleep(delay * 1000);

            pthread_mutex_lock(&shm_ptr->mutex);

            int next_floor = get_next_floor(current, destination);
            char next_floor_str[4];
            int_to_floor(next_floor, next_floor_str);
            strcpy(shm_ptr->current_floor, next_floor_str);

            strcpy(shm_ptr->status, "Closed");
            pthread_cond_broadcast(&shm_ptr->cond);

            if (strcmp(shm_ptr->current_floor, shm_ptr->destination_floor) == 0)
            {
                strcpy(shm_ptr->status, "Opening");
                pthread_cond_broadcast(&shm_ptr->cond);
                pthread_mutex_unlock(&shm_ptr->mutex);
                usleep(delay * 1000);

                pthread_mutex_lock(&shm_ptr->mutex);
                strcpy(shm_ptr->status, "Open");
                pthread_cond_broadcast(&shm_ptr->cond);

                if (shm_ptr->individual_service_mode == 1)
                {
                    pthread_mutex_unlock(&shm_ptr->mutex);
                    continue;
                }

                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                long nsec = ts.tv_nsec + (delay * 1000000L);
                ts.tv_sec += nsec / 1000000000L;
                ts.tv_nsec = nsec % 1000000000L;

                pthread_cond_timedwait(&shm_ptr->cond, &shm_ptr->mutex, &ts);

                if (shm_ptr->close_button == 1)
                {
                    shm_ptr->close_button = 0;
                }

                strcpy(shm_ptr->status, "Closing");
                pthread_cond_broadcast(&shm_ptr->cond);
                pthread_mutex_unlock(&shm_ptr->mutex);
                usleep(delay * 1000);

                pthread_mutex_lock(&shm_ptr->mutex);
                strcpy(shm_ptr->status, "Closed");
                pthread_cond_broadcast(&shm_ptr->cond);
                pthread_mutex_unlock(&shm_ptr->mutex);
            }
            else
            {
                pthread_mutex_unlock(&shm_ptr->mutex);
            }
        }
        else
        {
            pthread_mutex_unlock(&shm_ptr->mutex);
        }
    }

    return 0; // Will not be reached
}