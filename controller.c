#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define CONTROLLER_PORT 3000
#define BACKLOG 10
#define BUFFER_SIZE 1024

typedef struct Node
{
    int floor;
    struct Node *next;
} Node;

typedef struct
{
    char name[50];
    int is_active;
    int sockfd;

    int lowest_floor;
    int highest_floor;

    char current_floor[4];
    char destination_floor[4];
    char status[8];

    Node *queue;
    int peak_floor; // Highest floor in current journey (turning point)

} Car;

Car connected_cars[10];
pthread_mutex_t cars_mutex = PTHREAD_MUTEX_INITIALIZER;

// Forward declarations
void receiveMessage(int sockfd, char *buffer, int buffer_size);
void handleCarRegistration(const char *car_name, const char *lowest_floor, const char *highest_floor, int sockfd);
void handleCallRequest(const char *source_floor, const char *destination_floor, int client_fd);

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

int is_floor_in_queue(Node *queue, int floor)
{
    Node *current = queue;
    while (current != NULL)
    {
        if (current->floor == floor)
        {
            return 1; // Already in queue
        }
        current = current->next;
    }
    return 0; // Not in queue
}

// Insert pickup floor below current peak (in ascending section)
void insert_below_peak(Node **queue, int peak_floor, int floor)
{
    Node *new_node = malloc(sizeof(Node));
    new_node->floor = floor;
    new_node->next = NULL;

    if (*queue == NULL)
    {
        *queue = new_node;
        return;
    }

    // Insert in ascending order until we hit the peak
    Node *curr = *queue;
    Node *prev = NULL;

    while (curr != NULL && curr->floor < peak_floor)
    {
        if (floor < curr->floor)
        {
            // Insert here
            new_node->next = curr;
            if (prev == NULL)
            {
                *queue = new_node;
            }
            else
            {
                prev->next = new_node;
            }
            return;
        }
        prev = curr;
        curr = curr->next;
    }

    // Insert before peak
    new_node->next = curr;
    if (prev == NULL)
    {
        *queue = new_node;
    }
    else
    {
        prev->next = new_node;
    }
}

// Insert pickup floor above current peak (becomes new peak)
void insert_above_peak(Node **queue, int *peak_floor, int floor)
{
    Node *new_node = malloc(sizeof(Node));
    new_node->floor = floor;
    new_node->next = NULL;

    if (*queue == NULL)
    {
        *queue = new_node;
        *peak_floor = floor;
        return;
    }

    // Find the END of the ascending section (right before it starts descending)
    Node *curr = *queue;

    while (curr->next != NULL && curr->next->floor > curr->floor)
    {
        // Still ascending
        curr = curr->next;
    }

    // Now curr is the last ascending node (the current peak)
    // Insert new peak after it
    new_node->next = curr->next;
    curr->next = new_node;

    *peak_floor = floor; // Update peak to new value
}

// Insert dropoff in descending section (after peak)
void append_to_descent(Node **queue, int peak_floor, int floor)
{
    Node *new_node = malloc(sizeof(Node));
    new_node->floor = floor;
    new_node->next = NULL;

    if (*queue == NULL)
    {
        *queue = new_node;
        return;
    }

    // Find the peak, then insert in descending order after it
    Node *curr = *queue;

    // Skip to the peak node itself
    while (curr != NULL && curr->floor != peak_floor)
    {
        if (curr->next == NULL)
            break;
        curr = curr->next;
    }

    if (curr == NULL || curr->floor != peak_floor)
    {
        // Couldn't find peak, just append at end
        curr = *queue;
        while (curr->next != NULL)
        {
            curr = curr->next;
        }
        curr->next = new_node;
        return;
    }

    // Now insert in descending order after the peak
    while (curr->next != NULL && curr->next->floor > floor)
    {
        curr = curr->next;
    }

    new_node->next = curr->next;
    curr->next = new_node;
}

void receiveMessage(int sockfd, char *buffer, int buffer_size)
{
    uint16_t len;
    if (recv(sockfd, &len, sizeof(len), 0) != sizeof(len))
    {
        buffer[0] = '\0';
        return;
    }
    len = ntohs(len);

    if (len >= buffer_size)
    {
        fprintf(stderr, "Received message is too large for buffer\n");
        buffer[0] = '\0';
        return;
    }

    recv(sockfd, buffer, len, 0);
    buffer[len] = '\0';
}

void handleCarRegistration(const char *car_name, const char *lowest_floor, const char *highest_floor, int sockfd)
{
    pthread_mutex_lock(&cars_mutex);

    // Check if car already exists (reconnecting)
    for (int i = 0; i < 10; i++)
    {
        if (connected_cars[i].is_active && strcmp(connected_cars[i].name, car_name) == 0)
        {
            connected_cars[i].sockfd = sockfd;
            pthread_mutex_unlock(&cars_mutex);
            return;
        }
    }

    // Register new car
    for (int i = 0; i < 10; i++)
    {
        if (!connected_cars[i].is_active)
        {
            strcpy(connected_cars[i].name, car_name);
            connected_cars[i].is_active = 1;
            connected_cars[i].sockfd = sockfd;
            strcpy(connected_cars[i].current_floor, lowest_floor);
            strcpy(connected_cars[i].destination_floor, lowest_floor);
            strcpy(connected_cars[i].status, "Closed");
            connected_cars[i].lowest_floor = floor_to_int(lowest_floor);
            connected_cars[i].highest_floor = floor_to_int(highest_floor);
            connected_cars[i].queue = NULL;
            connected_cars[i].peak_floor = floor_to_int(lowest_floor); // Initialize peak
            printf("Registered new car: %s (Floors: %s to %s)\n", car_name, lowest_floor, highest_floor);
            pthread_mutex_unlock(&cars_mutex);
            return;
        }
    }

    printf("No space to register new car: %s\n", car_name);
    pthread_mutex_unlock(&cars_mutex);
}

void handleCallRequest(const char *source_floor, const char *destination_floor, int client_fd)
{
    printf("Handling call request from %s to %s\n", source_floor, destination_floor);

    int source = floor_to_int(source_floor);
    int dest = floor_to_int(destination_floor);

    pthread_mutex_lock(&cars_mutex);

    for (int i = 0; i < 10; i++)
    {
        if (!connected_cars[i].is_active)
            continue;

        // Check if car can reach both floors
        if (source < connected_cars[i].lowest_floor ||
            source > connected_cars[i].highest_floor ||
            dest < connected_cars[i].lowest_floor ||
            dest > connected_cars[i].highest_floor)
        {
            continue;
        }

        // Determine effective floor (where car actually is or is going)
        int effective_floor;
        if (strcmp(connected_cars[i].status, "Closing") == 0 ||
            strcmp(connected_cars[i].status, "Between") == 0)
        {
            effective_floor = floor_to_int(connected_cars[i].destination_floor);
        }
        else
        {
            effective_floor = floor_to_int(connected_cars[i].current_floor);
        }

        // If queue is empty, initialize peak to effective floor
        if (connected_cars[i].queue == NULL)
        {
            connected_cars[i].peak_floor = effective_floor;
        }

        // Determine car's direction based on actual movement
        int car_current = floor_to_int(connected_cars[i].current_floor);
        int car_dest = floor_to_int(connected_cars[i].destination_floor);
        int is_going_up = (car_dest > car_current);
        int is_going_down = (car_dest < car_current);

        // Determine if source is ahead or behind the car
        int source_ahead = 0;

        if (is_going_up)
        {
            source_ahead = (source >= effective_floor);
        }
        else if (is_going_down)
        {
            source_ahead = (source <= effective_floor);
        }
        else
        {
            // Idle - any source is "ahead"
            source_ahead = 1;
        }

        // Determine if we're at/past the peak
        int at_or_past_peak = (car_current >= connected_cars[i].peak_floor);

        // Insert source floor
        if (!is_floor_in_queue(connected_cars[i].queue, source))
        {
            if (source > connected_cars[i].peak_floor)
            {
                // Source is new peak
                insert_above_peak(&connected_cars[i].queue, &connected_cars[i].peak_floor, source);
            }
            else if (source_ahead && source <= connected_cars[i].peak_floor && !at_or_past_peak)
            {
                // Source is ahead and below/at peak - insert in ascent
                insert_below_peak(&connected_cars[i].queue, connected_cars[i].peak_floor, source);
            }
            else
            {
                // Source is behind OR we're past the peak - insert in descent
                append_to_descent(&connected_cars[i].queue, connected_cars[i].peak_floor, source);
            }
        }

        // Insert destination floor (always goes in descent/after the journey)
        if (!is_floor_in_queue(connected_cars[i].queue, dest))
        {
            append_to_descent(&connected_cars[i].queue, connected_cars[i].peak_floor, dest);
        }

        // Recalculate peak based on entire queue
        int new_peak = connected_cars[i].queue->floor;
        Node *curr = connected_cars[i].queue;
        while (curr != NULL)
        {
            if (curr->floor > new_peak)
            {
                new_peak = curr->floor;
            }
            curr = curr->next;
        }
        connected_cars[i].peak_floor = new_peak;

        // Check if we need to send a new FLOOR message
        if (connected_cars[i].queue != NULL)
        {
            int first_floor_in_queue = connected_cars[i].queue->floor;
            int current_destination = floor_to_int(connected_cars[i].destination_floor);

            // Send FLOOR message if:
            // 1. Destination changed, OR
            // 2. Car is already at this floor but doors are closed (need to reopen)
            if (first_floor_in_queue != current_destination ||
                (first_floor_in_queue == current_destination &&
                 strcmp(connected_cars[i].status, "Closed") == 0))
            {
                char floor_str[4];
                int_to_floor(first_floor_in_queue, floor_str);

                char floor_msg[BUFFER_SIZE];
                sprintf(floor_msg, "FLOOR %s", floor_str);
                uint16_t len = strlen(floor_msg);
                uint16_t net_len = htons(len);
                send(connected_cars[i].sockfd, &net_len, sizeof(net_len), 0);
                send(connected_cars[i].sockfd, floor_msg, len, 0);

                printf("Sent FLOOR %s to car %s\n", floor_str, connected_cars[i].name);
            }
        }

        // Send acknowledgment to call pad
        char ack[BUFFER_SIZE];
        sprintf(ack, "CAR %s", connected_cars[i].name);
        uint16_t len = strlen(ack);
        uint16_t net_len = htons(len);
        send(client_fd, &net_len, sizeof(net_len), 0);
        send(client_fd, ack, len, 0);

        pthread_mutex_unlock(&cars_mutex);
        return;
    }

    pthread_mutex_unlock(&cars_mutex);

    // No car could handle the request
    printf("No active cars to handle the call request.\n");
    char error[] = "UNAVAILABLE";
    uint16_t len = strlen(error);
    uint16_t net_len = htons(len);
    send(client_fd, &net_len, sizeof(net_len), 0);
    send(client_fd, error, len, 0);
}

void *handleConnection(void *arg)
{
    int sockfd = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    receiveMessage(sockfd, buffer, sizeof(buffer));
    printf("Received message: [%s]\n", buffer);

    if (strncmp(buffer, "CAR", 3) == 0)
    {
        char car_name[50], lowest[4], highest[4];
        sscanf(buffer, "%*s %49s %3s %3s", car_name, lowest, highest);
        handleCarRegistration(car_name, lowest, highest, sockfd);
        printf("Car %s connected on socket %d\n", car_name, sockfd);

        // Handle persistent car connection
        while (1)
        {
            receiveMessage(sockfd, buffer, sizeof(buffer));

            // Check if car disconnected
            if (buffer[0] == '\0')
            {
                printf("Car %s disconnected\n", car_name);
                pthread_mutex_lock(&cars_mutex);
                for (int i = 0; i < 10; i++)
                {
                    if (connected_cars[i].sockfd == sockfd)
                    {
                        connected_cars[i].is_active = 0;
                        break;
                    }
                }
                pthread_mutex_unlock(&cars_mutex);
                break;
            }

            // Handle STATUS messages
            if (strncmp(buffer, "STATUS", 6) == 0)
            {
                char status[8], current[4], dest[4];
                sscanf(buffer, "%*s %7s %3s %3s", status, current, dest);

                pthread_mutex_lock(&cars_mutex);
                for (int i = 0; i < 10; i++)
                {
                    if (connected_cars[i].sockfd == sockfd)
                    {
                        strcpy(connected_cars[i].status, status);
                        strcpy(connected_cars[i].current_floor, current);
                        strcpy(connected_cars[i].destination_floor, dest);

                        // Car arrived at a floor - pop from queue and send next
                        if (strcmp(status, "Opening") == 0 && strcmp(current, dest) == 0)
                        {
                            if (connected_cars[i].queue != NULL)
                            {
                                // Pop the first floor
                                Node *temp = connected_cars[i].queue;
                                connected_cars[i].queue = connected_cars[i].queue->next;
                                free(temp);

                                // Recalculate peak after popping
                                if (connected_cars[i].queue != NULL)
                                {
                                    // Find new peak in remaining queue
                                    int new_peak = connected_cars[i].queue->floor;
                                    Node *curr = connected_cars[i].queue;
                                    while (curr != NULL)
                                    {
                                        if (curr->floor > new_peak)
                                        {
                                            new_peak = curr->floor;
                                        }
                                        curr = curr->next;
                                    }
                                    connected_cars[i].peak_floor = new_peak;
                                }
                                else
                                {
                                    // Queue is empty, reset peak to current floor
                                    connected_cars[i].peak_floor = floor_to_int(current);
                                }

                                // Send next floor if there is one
                                if (connected_cars[i].queue != NULL)
                                {
                                    char floor_str[4];
                                    int_to_floor(connected_cars[i].queue->floor, floor_str);

                                    char floor_msg[BUFFER_SIZE];
                                    sprintf(floor_msg, "FLOOR %s", floor_str);
                                    uint16_t len = strlen(floor_msg);
                                    uint16_t net_len = htons(len);
                                    send(sockfd, &net_len, sizeof(net_len), 0);
                                    send(sockfd, floor_msg, len, 0);

                                    printf("Sent next FLOOR %s to car %s\n", floor_str, connected_cars[i].name);
                                }
                            }
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&cars_mutex);
            }
        }
    }
    else if (strncmp(buffer, "CALL", 4) == 0)
    {
        char source[4], dest[4];
        sscanf(buffer, "%*s %3s %3s", source, dest);
        handleCallRequest(source, dest, sockfd);
    }
    else
    {
        const char *reply = "ERROR Unknown command";
        uint16_t len = strlen(reply);
        uint16_t net_len = htons(len);
        send(sockfd, &net_len, sizeof(net_len), 0);
        send(sockfd, reply, len, 0);
    }

    close(sockfd);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 1)
    {
        fprintf(stderr, "Usage: %s (no arguments)\n", argv[0]);
        return 1;
    }

    for (int i = 0; i < 10; i++)
    {
        connected_cars[i].is_active = 0;
        connected_cars[i].queue = NULL;
        connected_cars[i].peak_floor = 0;
    }

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1)
    {
        perror("socket");
        return 1;
    }

    int opt_enable = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt_enable, sizeof(opt_enable));

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(CONTROLLER_PORT);

    if (bind(listenfd, (struct sockaddr *)&server_address, sizeof(server_address)) == -1)
    {
        perror("bind");
        return 1;
    }

    if (listen(listenfd, BACKLOG) == -1)
    {
        perror("listen");
        return 1;
    }

    printf("Controller is listening on port %d...\n", CONTROLLER_PORT);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int clientfd = accept(listenfd, (struct sockaddr *)&client_addr, &client_len);
        if (clientfd == -1)
        {
            perror("accept");
            continue;
        }

        printf("Accepted a new connection.\n");

        int *sockfd_ptr = malloc(sizeof(int));
        *sockfd_ptr = clientfd;
        pthread_t thread;
        pthread_create(&thread, NULL, handleConnection, sockfd_ptr);
        pthread_detach(thread);
    }

    return 0;
}