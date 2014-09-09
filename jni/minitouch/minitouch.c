#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <libevdev.h>

#define MAX_CONTACTS 10
#define DEFAULT_SOCKET_PATH "/data/local/tmp/minitouch.sock"

static void usage(const char* pname)
{
  fprintf(stderr,
    "Usage: %s [-h] [-d <device>] [-s <socket>]\n"
    "  -d <device>: Use the given touch device.\n"
    "  -s <socket>: Start a unix domain socket at the given path. (%s)\n"
    "  -h:          Show help.\n",
    pname, DEFAULT_SOCKET_PATH
  );
}

typedef struct
{
  int enabled;
  int tracking_id;
  int x;
  int y;
  int pressure;
} contact_t;

typedef struct
{
  int fd;
  int score;
  char path[100];
  struct libevdev* evdev;
  int has_mtslot;
  int has_tracking_id;
  int has_key_btn_touch;
  int has_touch_major;
  int has_width_major;
  int has_pressure;
  int min_pressure;
  int max_pressure;
  int max_x;
  int max_y;
  contact_t contacts[MAX_CONTACTS];
  int max_contacts;
  int tracking_id;
} internal_state_t;

static int is_character_device(const char* devpath)
{
  struct stat statbuf;

  if (stat(devpath, &statbuf) == -1) {
    perror("stat");
    return 0;
  }

  if (!S_ISCHR(statbuf.st_mode))
  {
    return 0;
  }

  return 1;
}

static int is_multitouch_device(struct libevdev* evdev)
{
  return libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X);
}

static int consider_device(const char* devpath, internal_state_t* state)
{
  int fd = -1;
  struct libevdev* evdev = NULL;

  if (!is_character_device(devpath))
  {
    goto mismatch;
  }

  if ((fd = open(devpath, O_RDWR)) < 0)
  {
    perror("open");
    fprintf(stderr, "Unable to open device %s for inspection", devpath);
    goto mismatch;
  }

  if (libevdev_new_from_fd(fd, &evdev) < 0)
  {
    fprintf(stderr, "Note: device %s is not supported by libevdev\n", devpath);
    goto mismatch;
  }

  if (!is_multitouch_device(evdev))
  {
    goto mismatch;
  }

  int score = 1000;

  if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_TOOL_TYPE))
  {
    int tool_min = libevdev_get_abs_minimum(evdev, ABS_MT_TOOL_TYPE);
    int tool_max = libevdev_get_abs_maximum(evdev, ABS_MT_TOOL_TYPE);

    if (tool_min > MT_TOOL_FINGER || tool_max < MT_TOOL_FINGER)
    {
      fprintf(stderr, "Note: device %s is a touch device, but doesn't"
        " support fingers\n", devpath);
      goto mismatch;
    }

    score -= tool_max - MT_TOOL_FINGER;
  }

  if (state->evdev != NULL)
  {
    if (state->score >= score)
    {
      fprintf(stderr, "Note: device %s was outscored by %s (%d >= %d)\n",
        devpath, state->path, state->score, score);
      goto mismatch;
    }
    else
    {
      fprintf(stderr, "Note: device %s was outscored by %s (%d >= %d)\n",
        state->path, devpath, score, state->score);
    }
  }

  libevdev_free(state->evdev);

  state->fd = fd;
  state->score = score;
  strncpy(state->path, devpath, sizeof(state->path));
  state->evdev = evdev;

  printf("%s\n", devpath);

  return 1;

mismatch:
  libevdev_free(evdev);

  if (fd >= 0)
  {
    close(fd);
  }

  return 0;
}

static int walk_devices(const char* path, internal_state_t* state)
{
  DIR* dir;
  struct dirent* ent;
  char devpath[FILENAME_MAX];

  if ((dir = opendir(path)) != NULL)
  {
    while ((ent = readdir(dir)) != NULL)
    {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      {
        continue;
      }

      snprintf(devpath, FILENAME_MAX, "%s/%s", path, ent->d_name);

      consider_device(devpath, state);
    }

    closedir(dir);
  }
  else {
    perror("opendir");
    return -1;
  }
}

static int write_event(internal_state_t* state, uint16_t type,
  uint16_t code, int32_t value)
{
  // It seems that most devices do not require the event timestamps at all.
  // Left here for reference should such a situation arise.
  //
  //   timespec ts;
  //   clock_gettime(CLOCK_MONOTONIC, &ts);
  //   input_event event = {{ts.tv_sec, ts.tv_nsec / 1000}, type, code, value};

  struct input_event event = {{0, 0}, type, code, value};
  ssize_t result;
  ssize_t length = (ssize_t) sizeof(event);

  result = write(state->fd, &event, length);
  return result - length;
}

static int next_tracking_id(internal_state_t* state)
{
  if (state->tracking_id < INT_MAX)
  {
    state->tracking_id += 1;
  }
  else
  {
    state->tracking_id = 0;
  }

  return state->tracking_id;
}

static int type_a_touch_down(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact >= state->max_contacts || state->contacts[contact].enabled)
  {
    return 0;
  }

  state->contacts[contact].enabled = 1;
  state->contacts[contact].x = x;
  state->contacts[contact].y = y;
  state->contacts[contact].pressure = pressure;

  return 1;
}

static int type_a_touch_move(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  state->contacts[contact].enabled = 2;
  state->contacts[contact].x = x;
  state->contacts[contact].y = y;
  state->contacts[contact].pressure = pressure;

  return 1;
}

static int type_a_touch_up(internal_state_t* state, int contact)
{
  if (contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  state->contacts[contact].enabled = 3;

  return 1;
}

static int type_a_commit(internal_state_t* state)
{
  int contact;
  int found_any = 0;

  for (contact = 0; contact < state->max_contacts; ++contact)
  {
    switch (state->contacts[contact].enabled)
    {
      case 1: // WENT_DOWN
        found_any = 1;

        if (state->has_tracking_id)
          write_event(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        if (state->has_key_btn_touch)
          write_event(state, EV_KEY, BTN_TOUCH, 1);

        if (state->has_touch_major)
          write_event(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

        if (state->has_width_major)
          write_event(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

        if (state->has_pressure)
          write_event(state, EV_ABS, ABS_MT_PRESSURE, state->contacts[contact].pressure);

        write_event(state, EV_ABS, ABS_MT_POSITION_X, state->contacts[contact].x);
        write_event(state, EV_ABS, ABS_MT_POSITION_Y, state->contacts[contact].y);

        write_event(state, EV_SYN, SYN_MT_REPORT, 0);

        state->contacts[contact].enabled = 2;
        break;
      case 2: // MOVED
        found_any = 1;

        if (state->has_tracking_id)
          write_event(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        if (state->has_touch_major)
          write_event(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

        if (state->has_width_major)
          write_event(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

        if (state->has_pressure)
          write_event(state, EV_ABS, ABS_MT_PRESSURE, state->contacts[contact].pressure);

        write_event(state, EV_ABS, ABS_MT_POSITION_X, state->contacts[contact].x);
        write_event(state, EV_ABS, ABS_MT_POSITION_Y, state->contacts[contact].y);

        write_event(state, EV_SYN, SYN_MT_REPORT, 0);
        break;
      case 3: // WENT_UP
        found_any = 1;

        if (state->has_tracking_id)
          write_event(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        if (state->has_key_btn_touch)
          write_event(state, EV_KEY, BTN_TOUCH, 0);

        write_event(state, EV_SYN, SYN_MT_REPORT, 0);

        state->contacts[contact].enabled = 0;
        break;
    }
  }

  if (found_any)
    write_event(state, EV_SYN, SYN_REPORT, 0);

  return 1;
}

static int type_b_touch_down(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact >= state->max_contacts || state->contacts[contact].enabled)
  {
    return 0;
  }

  state->contacts[contact].enabled = 1;
  state->contacts[contact].tracking_id = next_tracking_id(state);

  write_event(state, EV_ABS, ABS_MT_SLOT, contact);
  write_event(state, EV_ABS, ABS_MT_TRACKING_ID,
    state->contacts[contact].tracking_id);

  if (state->has_key_btn_touch)
    write_event(state, EV_KEY, BTN_TOUCH, 1);

  if (state->has_touch_major)
    write_event(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

  if (state->has_width_major)
    write_event(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

  if (state->has_pressure)
    write_event(state, EV_ABS, ABS_MT_PRESSURE, pressure);

  write_event(state, EV_ABS, ABS_MT_POSITION_X, x);
  write_event(state, EV_ABS, ABS_MT_POSITION_Y, y);

  return 1;
}

static int type_b_touch_move(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  write_event(state, EV_ABS, ABS_MT_SLOT, contact);

  if (state->has_touch_major)
    write_event(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

  if (state->has_width_major)
    write_event(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

  if (state->has_pressure)
    write_event(state, EV_ABS, ABS_MT_PRESSURE, pressure);

  write_event(state, EV_ABS, ABS_MT_POSITION_X, x);
  write_event(state, EV_ABS, ABS_MT_POSITION_Y, y);

  return 1;
}

static int type_b_touch_up(internal_state_t* state, int contact)
{
  if (contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  state->contacts[contact].enabled = 0;

  write_event(state, EV_ABS, ABS_MT_SLOT, contact);
  write_event(state, EV_ABS, ABS_MT_TRACKING_ID, -1);

  if (state->has_key_btn_touch)
    write_event(state, EV_KEY, BTN_TOUCH, 0);

  return 1;
}

static int type_b_commit(internal_state_t* state)
{
  write_event(state, EV_SYN, SYN_REPORT, 0);

  return 1;
}

static int touch_down(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (state->has_mtslot)
  {
    return type_b_touch_down(state, contact, x, y, pressure);
  }
  else
  {
    return type_a_touch_down(state, contact, x, y, pressure);
  }
}

static int touch_move(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (state->has_mtslot)
  {
    return type_b_touch_move(state, contact, x, y, pressure);
  }
  else
  {
    return type_a_touch_move(state, contact, x, y, pressure);
  }
}

static int touch_up(internal_state_t* state, int contact)
{
  if (state->has_mtslot)
  {
    return type_b_touch_up(state, contact);
  }
  else
  {
    return type_a_touch_up(state, contact);
  }
}

static int commit(internal_state_t* state)
{
  if (state->has_mtslot)
  {
    return type_b_commit(state);
  }
  else
  {
    return type_a_commit(state);
  }
}


static int start_server(char* sockpath)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (fd < 0)
  {
    perror("creating socket");
    return fd;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, sockpath);

  int length = strlen(addr.sun_path) + sizeof(addr.sun_family);

  unlink(addr.sun_path);

  if (bind(fd, (struct sockaddr*) &addr, length) < 0)
  {
    perror("binding socket");
    close(fd);
    return -1;
  }

  listen(fd, 1);

  return fd;
}

static int read_int(int fd, int* out)
{
  char buffer[1];
}

int main(int argc, char* argv[])
{
  const char* pname = argv[0];
  const char* devroot = "/dev/input";
  char* device = NULL;
  char* socket = DEFAULT_SOCKET_PATH;

  int opt;
  while ((opt = getopt(argc, argv, "d:s:h")) != -1) {
    switch (opt) {
      case 'd':
        device = optarg;
        break;
      case 's':
        socket = optarg;
        break;
      case '?':
        usage(pname);
        return EXIT_FAILURE;
      case 'h':
        usage(pname);
        return EXIT_SUCCESS;
    }
  }

  internal_state_t state = {0};

  if (device != NULL)
  {
    if (!consider_device(device, &state))
    {
      fprintf(stderr, "%s is not a supported touch device\n", device);
      return EXIT_FAILURE;
    }
  }
  else
  {
    if (!walk_devices(devroot, &state))
    {
      fprintf(stderr, "Unable to crawl %s for touch devices\n", devroot);
      return EXIT_FAILURE;
    }
  }

  if (state.evdev == NULL)
  {
    fprintf(stderr, "Unable to find a suitable touch device\n");
    return EXIT_FAILURE;
  }

  state.has_mtslot =
    libevdev_has_event_code(state.evdev, EV_ABS, ABS_MT_SLOT);
  state.has_tracking_id =
    libevdev_has_event_code(state.evdev, EV_ABS, ABS_MT_TRACKING_ID);
  state.has_key_btn_touch =
    libevdev_has_event_code(state.evdev, EV_KEY, BTN_TOUCH);
  state.has_touch_major =
    libevdev_has_event_code(state.evdev, EV_ABS, ABS_MT_TOUCH_MAJOR);
  state.has_width_major =
    libevdev_has_event_code(state.evdev, EV_ABS, ABS_MT_WIDTH_MAJOR);

  state.has_pressure =
    libevdev_has_event_code(state.evdev, EV_ABS, ABS_MT_PRESSURE);
  state.min_pressure = state.has_pressure ?
    libevdev_get_abs_minimum(state.evdev, ABS_MT_PRESSURE) : 0;
  state.max_pressure= state.has_pressure ?
    libevdev_get_abs_maximum(state.evdev, ABS_MT_PRESSURE) : 0;

  state.max_x = libevdev_get_abs_maximum(state.evdev, ABS_MT_POSITION_X);
  state.max_y = libevdev_get_abs_maximum(state.evdev, ABS_MT_POSITION_Y);

  state.max_contacts = state.has_mtslot ?
    libevdev_get_abs_maximum(state.evdev, ABS_MT_SLOT) : 2;

  state.tracking_id = 0;

  int contact;
  for (contact = 0; contact < MAX_CONTACTS; ++contact)
  {
    state.contacts[contact].enabled = 0;
  }

  fprintf(stderr,
    "%s touch device %s (%dx%d) detected on %s (score %d)\n",
    state.has_mtslot ? "Type B" : "Type A",
    libevdev_get_name(state.evdev),
    state.max_x, state.max_y,
    state.path, state.score
  );

  fprintf(stdout, "max %d %d %d\n", state.max_x, state.max_y, state.max_pressure);

  struct sockaddr_un client_addr;
  socklen_t client_addr_length = sizeof(client_addr);

  int server_fd = start_server(socket);

  if (server_fd < 0)
  {
    fprintf(stderr, "Unable to start server on %s\n", socket);
    return EXIT_FAILURE;
  }

  while (1)
  {
    int client_fd = accept(server_fd, (struct sockaddr *) &client_addr,
      &client_addr_length);

    if (client_fd < 0)
    {
      perror("accepting client");
      exit(1);
    }

    fprintf(stderr, "Connection established\n");

    char input[80] = {0};
    int input_length = 0;
    char* cursor;
    long int contact, x, y, pressure;

    while (1)
    {
      input_length = 0;

      while (input_length < sizeof(input) &&
        read(client_fd, &input[input_length], 1) == 1)
      {
        if (input[input_length++] == '\n')
        {
          break;
        }
      }

      if (input_length <= 0)
      {
        break;
      }

      if (input[input_length - 1] != '\n')
      {
        continue;
      }

      if (input_length == 1)
      {
        continue;
      }

      cursor = (char*) &input;
      cursor += 1;

      switch (input[0])
      {
        case 'c': // COMMIT
          commit(&state);
          break;
        case 'd': // TOUCH DOWN
          contact = strtol(cursor, &cursor, 10);
          x = strtol(cursor, &cursor, 10);
          y = strtol(cursor, &cursor, 10);
          pressure = strtol(cursor, &cursor, 10);
          touch_down(&state, contact, x, y, pressure);
          break;
        case 'm': // TOUCH MOVE
          contact = strtol(cursor, &cursor, 10);
          x = strtol(cursor, &cursor, 10);
          y = strtol(cursor, &cursor, 10);
          pressure = strtol(cursor, &cursor, 10);
          touch_move(&state, contact, x, y, pressure);
          break;
        case 'u': // TOUCH UP
          contact = strtol(cursor, &cursor, 10);
          touch_up(&state, contact);
          break;
        default:
          break;
      }
    }

    fprintf(stderr, "Connection closed\n");
    close(client_fd);
  }

  close(server_fd);

  libevdev_free(state.evdev);
  close(state.fd);

  return EXIT_SUCCESS;
}
