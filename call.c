#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define CONTROLLER_PORT 3000
#define CONTROLLER_IP "127.0.0.1"

void sendMessage(int sockfd, const char *msg)
{
    uint16_t len = strlen(msg);
    uint16_t net_len = htons(len);
    send(sockfd, &net_len, sizeof(net_len), 0);
    send(sockfd, msg, len, 0);
}

bool is_floor_valid(const char *floor_str)
{
    // Check if the string is empty or too long
    if (strlen(floor_str) == 0 || strlen(floor_str) > 3)
    {
        return false;
    }

    // --- First if statement: Handle Basement Floors ---
    if (floor_str[0] == 'B')
    {
        // Check if the rest of the string is a number
        for (int i = 1; i < strlen(floor_str); i++)
        {
            if (!isdigit(floor_str[i]))
                return false;
        }
        int floor_num = atoi(floor_str + 1);
        return (floor_num >= 1 && floor_num <= 99);
    }

    // --- Second if statement: Handle Regular Floors ---
    else if (isdigit(floor_str[0]))
    {
        // Check if all characters are numbers
        for (int i = 0; i < strlen(floor_str); i++)
        {
            if (!isdigit(floor_str[i]))
                return false;
        }
        int floor_num = atoi(floor_str);
        return (floor_num >= 1 && floor_num <= 999);
    }

    // If it doesn't start with 'B' or a digit, it's invalid
    return false;
}

void receiveMessage(int sockfd, char *buffer, int buffer_size)
{
    uint16_t len;
    recv(sockfd, &len, sizeof(len), 0); // Receive the length first
    len = ntohs(len);                   // Convert length back to host byte order

    if (len >= buffer_size)
    {
        fprintf(stderr, "Received message is too large for buffer\n");
        exit(1);
    }

    recv(sockfd, buffer, len, 0);
    buffer[len] = '\0'; // Null-terminate the received string
}

// ---------------------Main Function---------------------

int main(int argc, char *argv[])
{
    // 1. Check arguments
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <source_floor> <destination_floor>\n", argv[0]);
        return 1;
    }

    char *source_floor = argv[1];
    char *destination_floor = argv[2];

    if (!is_floor_valid(source_floor) || !is_floor_valid(destination_floor))
    {
        printf("Invalid floor(s) specified.\n");
        return 1;
    }

    // If source and destination are the same
    if (strcmp(source_floor, destination_floor) == 0)
    {
        printf("You are already on that floor!\n");
        return 0;
    }

    // 2. Create a TCP socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1)
    {
        perror("socket");
        return 1;
    }

    // 3. Set up the server's address struct
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(CONTROLLER_PORT);
    inet_pton(AF_INET, CONTROLLER_IP, &server_address.sin_addr);

    // 4. Connect to the server
    if (connect(sockfd, (struct sockaddr *)&server_address, sizeof(server_address)) == -1)
    {
        printf("Unable to connect to elevator system.\n");
        close(sockfd);
        return 1;
    }

    // 5. Build and send the message
    char message_buffer[100];
    sprintf(message_buffer, "CALL %s %s", source_floor, destination_floor);
    sendMessage(sockfd, message_buffer);

    // 6. Wait for and receive the reply
    char reply_buffer[200];
    receiveMessage(sockfd, reply_buffer, sizeof(reply_buffer));

    // 7. Process the reply
    if (strcmp(reply_buffer, "UNAVAILABLE") != 0)
    {
        printf("Car %s is arriving.\n", reply_buffer);
    }
    else
    {
        printf("Sorry, no car is available to take this request.\n");
    }

    // Clean up
    close(sockfd);
    return 0;
}