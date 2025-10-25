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

    Node *up_queue;
    Node *down_queue;

    char pending_destination[4];
} Car;

Car connected_cars[10];
pthread_mutex_t cars_mutex = PTHREAD_MUTEX_INITIALIZER;

// Forward declarations
void receiveMessage(int sockfd, char *buffer, int buffer_size);
void handleCarRegistration(const char *car_name, const char *lowest_floor, const char *highest_floor, int sockfd);
void handleCallRequest(const char *source_floor, const char *destination_floor, int client_fd);

// floor_to_int: convert floor string ("B1"/"1") to integer representation
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

// add_to_queue: insert floor into sorted linked list (ascending if going_up)
void add_to_queue(Node **queue, int floor, int going_up)
{
    Node *new_node = malloc(sizeof(Node));
    new_node->floor = floor;
    new_node->next = NULL;

    // Empty queue case
    if (*queue == NULL)
    {
        *queue = new_node;
        return;
    }

    Node *curr = *queue;
    Node *prev = NULL;

    // Going up - insert in ascending order
    if (going_up)
    {
        while (curr != NULL && curr->floor < floor)
        {
            prev = curr;
            curr = curr->next;
        }
    }
    // Going down - insert in descending order
    else
    {
        while (curr != NULL && curr->floor > floor)
        {
            prev = curr;
            curr = curr->next;
        }
    }

    // Insert the new node
    if (prev == NULL)
    {
        new_node->next = *queue;
        *queue = new_node;
    }
    else
    {
        new_node->next = curr;
        prev->next = new_node;
    }
}

// print_queue: debugging helper to print a queue's contents
void print_queue(const char *car_name, Node *queue, int peak_floor)
{
    // printf("DEBUG: Car %s queue [peak=%d]: ", car_name, peak_floor);
    if (queue == NULL)
    {
        printf("(empty)\n");
        return;
    }

    Node *curr = queue;
    printf("[");
    while (curr != NULL)
    {
        printf("%d", curr->floor);
        if (curr->next != NULL)
        {
            printf(", ");
        }
        curr = curr->next;
    }
    printf("]\n");
}

// insert_above_peak: insert a pickup above the current peak node
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
    Node *prev = NULL;

    while (curr->next != NULL && curr->next->floor > curr->floor)
    {
        // Still ascending
        prev = curr;
        curr = curr->next;
    }

    // Now curr is the last ascending node (the current peak)
    // Insert new peak after it
    new_node->next = curr->next;
    curr->next = new_node;

    *peak_floor = floor; // Update peak to new value
}

// append_to_descent: append a dropoff into the descending section after peak
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

    // printf("DEBUG append_to_descent: inserting %d (peak=%d), queue before: ", floor, peak_floor);
    Node *temp = *queue;
    while (temp)
    {
        printf("%d ", temp->floor);
        temp = temp->next;
    }
    printf("\n");

    // Skip to the peak node itself
    while (curr != NULL && curr->floor != peak_floor)
    {
        // printf("DEBUG: Moving to next, current=%d\n", curr->floor);
        if (curr->next == NULL)
            break;
        curr = curr->next;
    }

    if (curr == NULL || curr->floor != peak_floor)
    {
        // Couldn't find peak, just append at end
        // printf("DEBUG: Couldn't find peak, appending at end\n");
        curr = *queue;
        while (curr->next != NULL)
        {
            curr = curr->next;
        }
        curr->next = new_node;
        return;
    }

    // printf("DEBUG: Found peak at node %d\n", curr->floor);

    // Now insert in descending order after the peak
    while (curr->next != NULL && curr->next->floor > floor)
    {
        // printf("DEBUG: Skipping past %d (> %d in descent)\n", curr->next->floor, floor);
        curr = curr->next;
    }

    // printf("DEBUG: Inserting %d after %d\n", floor, curr->floor);

    new_node->next = curr->next;
    curr->next = new_node;
}

// is_floor_in_queue: check whether a floor already exists in the queue
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

// receiveMessage: read a 16-bit length-prefixed message into buffer
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

// handleCarRegistration: register or update a connected car in the array
void handleCarRegistration(const char *car_name, const char *lowest_floor, const char *highest_floor, int sockfd)
{
    pthread_mutex_lock(&cars_mutex);

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
            connected_cars[i].up_queue = NULL;
            connected_cars[i].down_queue = NULL;
            printf("Registered new car: %s (Floors: %s to %s)\n", car_name, lowest_floor, highest_floor);
            pthread_mutex_unlock(&cars_mutex);
            return;
        }
    }

    printf("No space to register new car: %s\n", car_name);
    pthread_mutex_unlock(&cars_mutex);
}

// handleCallRequest: choose a best car and forward the FLOOR assignment
void handleCallRequest(const char *source_floor, const char *destination_floor, int client_fd)
{
    printf("Handling call request from %s to %s\n", source_floor, destination_floor);

    int source = floor_to_int(source_floor);
    int dest = floor_to_int(destination_floor);
    int going_up = (dest > source); // Direction of travel

    pthread_mutex_lock(&cars_mutex);

    // Try to find a suitable car
    int best_car = -1;
    int best_distance = 99999; // Just a big number

    for (int i = 0; i < 10; i++)
    {
        if (!connected_cars[i].is_active)
            continue;

        // Check if car can serve these floors
        if (source < connected_cars[i].lowest_floor ||
            source > connected_cars[i].highest_floor ||
            dest < connected_cars[i].lowest_floor ||
            dest > connected_cars[i].highest_floor)
        {
            continue;
        }

        int car_pos = floor_to_int(connected_cars[i].current_floor);
        int car_dest = floor_to_int(connected_cars[i].destination_floor);

        // Don't use cars that are "Between" floors or have their doors open
        if (strcmp(connected_cars[i].status, "Between") == 0 ||
            strcmp(connected_cars[i].status, "Opening") == 0 ||
            strcmp(connected_cars[i].status, "Open") == 0)
        {
            continue;
        }

        // Figure out if car is moving and in which direction
        int car_going_up = (car_dest > car_pos);
        int car_moving = (car_pos != car_dest);

        // Calculate basic distance
        int distance = abs(car_pos - source);

        // If car is moving, check if request is in same direction
        if (car_moving)
        {
            if (going_up && !car_going_up)
            {
                // Car going down, we want up - make distance worse
                distance += 1000;
            }
            else if (!going_up && car_going_up)
            {
                // Car going up, we want down - make distance worse
                distance += 1000;
            }

            // If car has already passed our floor in its direction, make distance worse
            if (car_going_up && car_pos > source)
            {
                distance += 500;
            }
            else if (!car_going_up && car_pos < source)
            {
                distance += 500;
            }
        }

        // Pick the closest available car
        if (distance < best_distance)
        {
            best_car = i;
            best_distance = distance;
        }
    }

    if (best_car >= 0)
    {
        // Add both source and destination to appropriate queues
        add_to_queue(&connected_cars[best_car].up_queue, source, going_up);
        add_to_queue(&connected_cars[best_car].up_queue, dest, going_up);

        // Send first destination to car
        char floor_msg[BUFFER_SIZE];
        sprintf(floor_msg, "FLOOR %s", source_floor);
        uint16_t len = strlen(floor_msg);
        uint16_t net_len = htons(len);
        send(connected_cars[best_car].sockfd, &net_len, sizeof(net_len), 0);
        send(connected_cars[best_car].sockfd, floor_msg, len, 0);

        // Store pending destination
        strcpy(connected_cars[best_car].pending_destination, destination_floor);

        printf("Assigned call to car: %s, sent FLOOR %s\n",
               connected_cars[best_car].name, source_floor);

        // Send acknowledgment to call pad
        char ack[BUFFER_SIZE];
        sprintf(ack, "CAR %s", connected_cars[best_car].name);
        len = strlen(ack);
        net_len = htons(len);
        send(client_fd, &net_len, sizeof(net_len), 0);
        send(client_fd, ack, len, 0);

        pthread_mutex_unlock(&cars_mutex);
        return;
    }

    pthread_mutex_unlock(&cars_mutex);

    printf("No active cars to handle the call request.\n");
    char error[] = "UNAVAILABLE";
    uint16_t len = strlen(error);
    uint16_t net_len = htons(len);
    send(client_fd, &net_len, sizeof(net_len), 0);
    send(client_fd, error, len, 0);
}

// handleConnection: thread entry to process a single client (car or call pad)
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

        // --- FIX STARTS HERE: Add a loop to handle persistent car connections ---
        while (1)
        {
            receiveMessage(sockfd, buffer, sizeof(buffer));
            // Check if the car disconnected
            if (buffer[0] == '\0')
            {
                printf("Car %s disconnected\n", car_name);
                pthread_mutex_lock(&cars_mutex);
                for (int i = 0; i < 10; i++)
                {
                    if (connected_cars[i].sockfd == sockfd)
                    {
                        connected_cars[i].is_active = 0; // Mark car as inactive
                        break;
                    }
                }
                pthread_mutex_unlock(&cars_mutex);
                break; // Exit the loop
            }

            // Existing status handling logic goes inside the loop
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

                        // Simple queue simulation using destination tracking
                        // When car reaches its destination and starts opening
                        if (strcmp(status, "Opening") == 0 && strcmp(current, dest) == 0)
                        {
                            // Check if we have a pending destination
                            if (strlen(connected_cars[i].pending_destination) > 0)
                            {
                                char floor_msg[BUFFER_SIZE];
                                sprintf(floor_msg, "FLOOR %s", connected_cars[i].pending_destination);
                                uint16_t len = strlen(floor_msg);
                                uint16_t net_len = htons(len);
                                send(sockfd, &net_len, sizeof(net_len), 0);
                                send(sockfd, floor_msg, len, 0);
                                printf("Sent pending FLOOR %s to car %s\n", connected_cars[i].pending_destination, connected_cars[i].name);
                                connected_cars[i].pending_destination[0] = '\0';
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

// main: controller server loop accepting connections and dispatching threads
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