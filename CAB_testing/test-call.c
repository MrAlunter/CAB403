#include "shared.h"
#include <stdbool.h>
#include <ctype.h>

// Tester for call point

#define DELAY 50000 // 50ms

void *server(void *);

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

int main()
{
  msg("Unable to connect to elevator system.");
  system("./call B1 3"); // Testing valid floors with elevator system unavailable
  pthread_t tid;
  pthread_create(&tid, NULL, server, NULL);
  pthread_detach(tid);
  usleep(DELAY);
  msg("RECV: CALL B21 337 : Car Test is arriving.");
  system("./call B21 337"); // Testing two valid floors
  usleep(DELAY);
  msg("You are already on that floor!");
  system("./call 152 152"); // Testing same floor
  usleep(DELAY);
  msg("RECV: CALL 416 B68 : Sorry, no car is available to take this request.");
  system("./call 416 B68"); // Testing two valid floors
  usleep(DELAY);
  msg("Invalid floor(s) specified.");
  system("./call L4 8"); // Testing wrong format floor
  usleep(DELAY);
  msg("Invalid floor(s) specified.");
  system("./call B100 B98"); // Testing out of range floor
  usleep(DELAY);
  msg("Invalid floor(s) specified.");
  system("./call 800 1000"); // Testing out of range floor

  printf("\nTests completed.\n");
}

void *server(void *_)
{
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(3000);
  a.sin_addr.s_addr = htonl(INADDR_ANY);

  int s = socket(AF_INET, SOCK_STREAM, 0);
  int opt_enable = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt_enable, sizeof(opt_enable));
  if (bind(s, (const struct sockaddr *)&a, sizeof(a)) == -1)
  {
    perror("bind()");
    exit(1);
  }

  listen(s, 10);
  int fd;
  char *msg;

  fd = accept(s, NULL, NULL);
  msg = receive_msg(fd);
  printf("RECV: %s : ", msg);
  fflush(stdout);
  free(msg);
  send_message(fd, "CAR Test");
  close(fd);

  fd = accept(s, NULL, NULL);
  msg = receive_msg(fd);
  printf("RECV: %s : ", msg);
  fflush(stdout);
  free(msg);
  send_message(fd, "UNAVAILABLE");
  close(fd);

  fd = accept(s, NULL, NULL);
  msg = receive_msg(fd);
  printf("(This shouldn't happen) RECV: %s : ", msg);
  fflush(stdout);
  free(msg);
  close(fd);
  return NULL;
}
