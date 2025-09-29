#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define CONTROLLER_PORT 3000
#define BACKLOG 10
#define BUFFER_SIZE 1024

typedef struct Node
{
    char floor[4];
    struct Node *next;
} Node;

typedef struct
{
    char name[50];
    int is_active; // A flag to know if this car is online

    // Status info from the car's status updates
    char current_floor[4];
    char destination_floor[4];
    char status[8];

    // The two queues
    Node *up_queue;
    Node *down_queue;
} Car;

Car connected_cars[10];

void add_to_queue(Node **head, const char *floor)
{
    Node *new_node = malloc(sizeof(Node));
    strcpy(new_node->floor, floor);
    new_node->next = NULL;

    if (*head == NULL)
    {
        *head = new_node;
    }
    else
    {
        Node *temp = *head;
        while (temp->next != NULL)
        {
            temp = temp->next;
        }
        temp->next = new_node;
    }
}

void handleCarRegistration(const char *car_name, const char *lowest_floor, const char *highest_floor)
{
    for (int i = 0; i < 10; i++)
    {
        if (connected_cars[i].is_active && strcmp(connected_cars[i].name, car_name) == 0)
        {
            // Car is already registered
            return;
        }
    }

    for (int i = 0; i < 10; i++)
    {
        if (!connected_cars[i].is_active)
        {
            strcpy(connected_cars[i].name, car_name);
            connected_cars[i].is_active = 1;
            strcpy(connected_cars[i].current_floor, lowest_floor);
            strcpy(connected_cars[i].destination_floor, lowest_floor);
            strcpy(connected_cars[i].status, "Closed");
            connected_cars[i].up_queue = NULL;
            connected_cars[i].down_queue = NULL;
            printf("Registered new car: %s (Floors: %s to %s)\n", car_name, lowest_floor, highest_floor);
            return;
        }
    }

    printf("No space to register new car: %s\n", car_name);
}

// --- Helper Function ---
void receiveMessage(int sockfd, char *buffer, int buffer_size)
{
    uint16_t len;
    if (recv(sockfd, &len, sizeof(len), 0) != sizeof(len))
    {
        return; // Client disconnected or error
    }
    len = ntohs(len);

    if (len >= buffer_size)
    {
        fprintf(stderr, "Received message is too large for buffer\n");
        return;
    }

    recv(sockfd, buffer, len, 0);
    buffer[len] = '\0';
}

void handleCallRequest(const char *source_floor, const char *destination_floor)
{
    printf("Handling call request from %s to %s\n", source_floor, destination_floor);
    // For simplicity, just assign to the first active car
    for (int i = 0; i < 10; i++)
    {
        if (connected_cars[i].is_active)
        {
            if (strcmp(source_floor, destination_floor) < 0)
            {
                add_to_queue(&connected_cars[i].up_queue, source_floor);
                add_to_queue(&connected_cars[i].up_queue, destination_floor);
            }
            else
            {
                add_to_queue(&connected_cars[i].down_queue, source_floor);
                add_to_queue(&connected_cars[i].down_queue, destination_floor);
            }
            printf("Assigned call to car: %s\n", connected_cars[i].name);
            return;
        }
    }
    printf("No active cars to handle the call request.\n");
}

// --- Main Program Logic ---
int main(int argc, char *argv[])
{
    if (argc != 1)
    {
        fprintf(stderr, "Usage: %s (no arguments)\n", argv[0]);
        return 1;
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
            continue; // Don't crash, just try to accept the next one
        }

        printf("Accepted a new connection.\n");

        char buffer[BUFFER_SIZE];
        receiveMessage(clientfd, buffer, sizeof(buffer));
        printf("Received message: [%s]\n", buffer);

        if (strncmp(buffer, "CALL", 4) == 0)
        {
            char source[4], dest[4];
            sscanf(buffer, "%*s %3s %3s", source, dest);

            handleCallRequest(source, dest);
            const char *reply = "ACK Call request received";
            uint16_t len = strlen(reply);
            uint16_t net_len = htons(len);
            send(clientfd, &net_len, sizeof(net_len), 0);
            send(clientfd, reply, len, 0);
            printf("Sent reply: [%s]\n", reply);
            close(clientfd);
            printf("Closed connection.\n\n");
        }

        else if (strncmp(buffer, "CAR", 3) == 0)
        {
            // It's a car registration message
            char car_name[50], lowest[4], highest[4];
            sscanf(buffer, "%*s %49s %3s %3s", car_name, lowest, highest);

            // Call your function to add the car to the list
            handleCarRegistration(car_name, lowest, highest);
        }

        else
        {
            const char *reply = "ERROR Unknown command";
            uint16_t len = strlen(reply);
            uint16_t net_len = htons(len);
            send(clientfd, &net_len, sizeof(net_len), 0);
            send(clientfd, reply, len, 0);
            printf("Sent reply: [%s]\n", reply);
            close(clientfd);
            printf("Closed connection.\n\n");
        }
    }

    return 0; // This line will not be reached
}