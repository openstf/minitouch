#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <libevdev.h>

#define MAX_SUPPORTED_CONTACTS 10
#define VERSION 1
#define DEFAULT_SOCKET_NAME "minitouch"

static int g_verbose = 0;

static void usage(const char* pname)
{
  fprintf(stderr,
    "Usage: %s [-h] [-d <device>] [-n <name>] [-v] [-i] [-f <file>]\n"
    "  -d <device>: Use the given touch device. Otherwise autodetect.\n"
    "  -n <name>:   Change the name of of the abtract unix domain socket. (%s)\n"
    "  -v:          Verbose output.\n"
    "  -i:          Uses STDIN and doesn't start socket.\n"
    "  -f <file>:   Runs a file with a list of commands, doesn't start socket.\n"
    "  -h:          Show help.\n",
    pname, DEFAULT_SOCKET_NAME
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
  int max_contacts;
  int max_tracking_id;
  int tracking_id;
  contact_t contacts[MAX_SUPPORTED_CONTACTS];
  int active_contacts;
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

  int score = 10000;

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

  if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_SLOT))
  {
    score += 1000;

    // Some devices, e.g. Blackberry PRIV (STV100) have more than one surface
    // you can touch. On the PRIV, the keypad also acts as a touch screen
    // that you can swipe and scroll with. The only differences between the
    // touch devices are that one is named "touch_display" and the other
    // "touch_keypad", the keypad only supports 3 contacts and the display
    // up to 9, and the keypad has a much lower resolution. Therefore
    // increasing the score by the number of contacts should be a relatively
    // safe bet, though we may also want to decrease the score by, say, 1,
    // if the device name contains "key" just in case they decide to start
    // supporting more contacts on both touch surfaces in the future.
    int num_slots = libevdev_get_abs_maximum(evdev, ABS_MT_SLOT);
    score += num_slots;
  }

  // For Blackberry devices, see above.
  // Also some device like SO-03L it has two touch devices, one is for touch
  // one is for side sense which name is 'sec_touchscreen_side'.
  // So add one more check for '_side'. check issue #45 for more info
  const char* name = libevdev_get_name(evdev);
  if (strstr(name, "key") != NULL || strstr(name, "_side") != NULL)
  {
    score -= 1;
  }

  // Alcatel OneTouch Idol 3 has an `input_mt_wrapper` device in addition
  // to direct input. It seems to be related to accessibility, as it shows
  // a touchpoint that you can move around, and then tap to activate whatever
  // is under the point. That wrapper device lacks the direct property.
  if (libevdev_has_property(evdev, INPUT_PROP_DIRECT))
  {
    score += 10000;
  }

  // Some devices may have an additional screen. For example, Meizu Pro7 Plus
  // has a small screen on the back side of the device called sub_touch, while
  // the boring screen in the front is called main_touch. The resolution on
  // the sub_touch device is much much lower. It seems like a safe bet
  // to always prefer the larger device, as long as the score adjustment is
  // likely to be lower than the adjustment we do for INPUT_PROP_DIRECT.
  if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X))
  {
    int x = libevdev_get_abs_maximum(evdev, ABS_MT_POSITION_X);
    int y = libevdev_get_abs_maximum(evdev, ABS_MT_POSITION_Y);
    score += sqrt(x * y);
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

  if ((dir = opendir(path)) == NULL)
  {
    perror("opendir");
    return -1;
  }

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

  return 0;
}

#define WRITE_EVENT(state, type, code, value) _write_event(state, type, #type, code, #code, value)

static int _write_event(internal_state_t* state,
  uint16_t type, const char* type_name,
  uint16_t code, const char* code_name,
  int32_t value)
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

  if (g_verbose)
    fprintf(stderr, "%-12s %-20s %08x\n", type_name, code_name, value);

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

        state->active_contacts += 1;

        if (state->has_tracking_id)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        // Send BTN_TOUCH on first contact only.
        if (state->active_contacts == 1 && state->has_key_btn_touch)
          WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 1);

        if (state->has_touch_major)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

        if (state->has_width_major)
          WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

        if (state->has_pressure)
          WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, state->contacts[contact].pressure);

        WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, state->contacts[contact].x);
        WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, state->contacts[contact].y);

        WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);

        state->contacts[contact].enabled = 2;
        break;
      case 2: // MOVED
        found_any = 1;

        if (state->has_tracking_id)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        if (state->has_touch_major)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

        if (state->has_width_major)
          WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

        if (state->has_pressure)
          WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, state->contacts[contact].pressure);

        WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, state->contacts[contact].x);
        WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, state->contacts[contact].y);

        WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);
        break;
      case 3: // WENT_UP
        found_any = 1;

        state->active_contacts -= 1;

        if (state->has_tracking_id)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        // Send BTN_TOUCH only when no contacts remain.
        if (state->active_contacts == 0 && state->has_key_btn_touch)
          WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 0);

        WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);

        state->contacts[contact].enabled = 0;
        break;
    }
  }

  if (found_any)
    WRITE_EVENT(state, EV_SYN, SYN_REPORT, 0);

  return 1;
}

static int type_a_touch_panic_reset_all(internal_state_t* state)
{
  int contact;

  for (contact = 0; contact < state->max_contacts; ++contact)
  {
    switch (state->contacts[contact].enabled)
    {
      case 1: // WENT_DOWN
      case 2: // MOVED
        // Force everything to WENT_UP
        state->contacts[contact].enabled = 3;
        break;
    }
  }

  return type_a_commit(state);
}

static int type_a_touch_down(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact >= state->max_contacts)
  {
    return 0;
  }

  if (state->contacts[contact].enabled)
  {
    type_a_touch_panic_reset_all(state);
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

static int type_b_commit(internal_state_t* state)
{
  WRITE_EVENT(state, EV_SYN, SYN_REPORT, 0);

  return 1;
}

static int type_b_touch_panic_reset_all(internal_state_t* state)
{
  int contact;
  int found_any = 0;

  for (contact = 0; contact < state->max_contacts; ++contact)
  {
    if (state->contacts[contact].enabled)
    {
      state->contacts[contact].enabled = 0;
      found_any = 1;
    }
  }

  return found_any ? type_b_commit(state) : 1;
}

static int type_b_touch_down(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact >= state->max_contacts)
  {
    return 0;
  }

  if (state->contacts[contact].enabled)
  {
    type_b_touch_panic_reset_all(state);
  }

  state->contacts[contact].enabled = 1;
  state->contacts[contact].tracking_id = next_tracking_id(state);
  state->active_contacts += 1;

  WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);
  WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID,
    state->contacts[contact].tracking_id);

  // Send BTN_TOUCH on first contact only.
  if (state->active_contacts == 1 && state->has_key_btn_touch)
    WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 1);

  if (state->has_touch_major)
    WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

  if (state->has_width_major)
    WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

  if (state->has_pressure)
    WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, pressure);

  WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, x);
  WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, y);

  return 1;
}

static int type_b_touch_move(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);

  if (state->has_touch_major)
    WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

  if (state->has_width_major)
    WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

  if (state->has_pressure)
    WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, pressure);

  WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, x);
  WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, y);

  return 1;
}

static int type_b_touch_up(internal_state_t* state, int contact)
{
  if (contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  state->contacts[contact].enabled = 0;
  state->contacts[contact].enabled = 0;
  state->active_contacts -= 1;

  WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);
  WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, -1);

  // Send BTN_TOUCH only when no contacts remain.
  if (state->active_contacts == 0 && state->has_key_btn_touch)
    WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 0);

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

static int touch_panic_reset_all(internal_state_t* state)
{
  if (state->has_mtslot)
  {
    return type_b_touch_panic_reset_all(state);
  }
  else
  {
    return type_a_touch_panic_reset_all(state);
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

static int start_server(char* sockname)
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
  strncpy(&addr.sun_path[1], sockname, strlen(sockname));

  if (bind(fd, (struct sockaddr*) &addr,
    sizeof(sa_family_t) + strlen(sockname) + 1) < 0)
  {
    perror("binding socket");
    close(fd);
    return -1;
  }

  listen(fd, 1);

  return fd;
}

static void parse_input(char* buffer, internal_state_t* state)
{
  char* cursor;
  long int contact, x, y, pressure, wait;

  cursor = (char*) buffer;
  cursor += 1;

  switch (buffer[0])
  {
    case 'c': // COMMIT
      commit(state);
      break;
    case 'r': // RESET
      touch_panic_reset_all(state);
      break;
    case 'd': // TOUCH DOWN
      contact = strtol(cursor, &cursor, 10);
      x = strtol(cursor, &cursor, 10);
      y = strtol(cursor, &cursor, 10);
      pressure = strtol(cursor, &cursor, 10);
      touch_down(state, contact, x, y, pressure);
      break;
    case 'm': // TOUCH MOVE
      contact = strtol(cursor, &cursor, 10);
      x = strtol(cursor, &cursor, 10);
      y = strtol(cursor, &cursor, 10);
      pressure = strtol(cursor, &cursor, 10);
      touch_move(state, contact, x, y, pressure);
      break;
    case 'u': // TOUCH UP
      contact = strtol(cursor, &cursor, 10);
      touch_up(state, contact);
      break;
    case 'w':
      wait = strtol(cursor, &cursor, 10);
      if (g_verbose)
        fprintf(stderr, "Waiting %ld ms\n", wait);
      usleep(wait * 1000);
      break;
    default:
      break;
  }
}

static void io_handler(FILE* input, FILE* output, internal_state_t* state)
{
  setvbuf(input, NULL, _IOLBF, 1024);
  setvbuf(output, NULL, _IOLBF, 1024);

  // Tell version
  fprintf(output, "v %d\n", VERSION);

  // Tell limits
  fprintf(output, "^ %d %d %d %d\n",
          state->max_contacts, state->max_x, state->max_y, state->max_pressure);

  // Tell pid
  fprintf(output, "$ %d\n", getpid());

  char read_buffer[80];

  while (fgets(read_buffer, sizeof(read_buffer), input) != NULL)
  {
    read_buffer[strcspn(read_buffer, "\r\n")] = 0;
    parse_input(read_buffer, state);
  }
}

static void proxy_handler(FILE* input, FILE* output, int proxy_fd)
{
  char read_buffer[80];
  FILE* proxy_input;
  FILE* proxy_output;
  proxy_input = fdopen(proxy_fd, "r");
  if (proxy_input == NULL)
  {
    fprintf(stderr, "%s: fdopen(proxy_fd,'r')\n", strerror(errno));
    exit(1);
  }
  proxy_output = fdopen(dup(proxy_fd), "w");
  if (proxy_output == NULL)
  {
    fprintf(stderr, "%s: fdopen(proxy_fd,'w')\n", strerror(errno));
    exit(1);
  }
  setvbuf(proxy_input, NULL, _IOLBF, 1024);
  setvbuf(proxy_output, NULL, _IOLBF, 1024);

  // Get version from the agent
  fgets(read_buffer, sizeof(read_buffer), proxy_input);
  fprintf(output, "%s", read_buffer);

  // Get the poiner x-y range from the agent
  fgets(read_buffer, sizeof(read_buffer), proxy_input);
  fprintf(output, "%s", read_buffer);

  // Tell pid
  fprintf(output, "$ %d\n", getpid());
  fflush(output);

  // Forward every event to the agent
  while (fgets(read_buffer, sizeof(read_buffer), input) != NULL)
  {
    fprintf(proxy_output, "%s", read_buffer);
  }
}

int connect_android_service() {
  int service_fd = 0;
  struct sockaddr_un serv_addr;
  const char* const socketname = "minitouchagent";

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sun_family = AF_UNIX;
  serv_addr.sun_path[0] = '\0';
  strncpy(serv_addr.sun_path+1, socketname, strlen(socketname));

  service_fd = socket(PF_UNIX, SOCK_STREAM, 0);
  if (service_fd < 0)
  {
    perror("creating socket");
    return -1;
  }

  int err = connect(service_fd, (struct sockaddr*) &serv_addr, offsetof(struct sockaddr_un, sun_path) + 1/*\0*/ + strlen(socketname));
  if (err != 0) {
    perror("connecting socket");
    return -1;
  }
  fprintf(stderr, "using Android InputManager\n");
  return service_fd;
}

int main(int argc, char* argv[])
{
  const char* pname = argv[0];
  const char* devroot = "/dev/input";
  char* device = NULL;
  char* sockname = DEFAULT_SOCKET_NAME;
  char* stdin_file = NULL;
  int use_stdin = 0;
  int android_service_fd = -1;

  int opt;
  while ((opt = getopt(argc, argv, "d:n:vif:h")) != -1) {
    switch (opt) {
      case 'd':
        device = optarg;
        break;
      case 'n':
        sockname = optarg;
        break;
      case 'v':
        g_verbose = 1;
        break;
      case 'i':
        use_stdin = 1;
        break;
      case 'f':
        stdin_file = optarg;
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
    if (walk_devices(devroot, &state) != 0)
    {
      fprintf(stderr, "Unable to crawl %s for touch devices\n", devroot);
      return EXIT_FAILURE;
    }
  }

  if (state.evdev == NULL)
  {
    fprintf(stderr, "Unable to find a suitable touch device\n");
    android_service_fd = connect_android_service();
    if( android_service_fd == -1) {
      return EXIT_FAILURE;
    }
  } else {
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

    state.max_tracking_id = state.has_tracking_id
      ? libevdev_get_abs_maximum(state.evdev, ABS_MT_TRACKING_ID)
      : INT_MAX;

    if (!state.has_mtslot && state.max_tracking_id == 0)
    {
      // The touch device reports incorrect values. There would be no point
      // in supporting ABS_MT_TRACKING_ID at all if the maximum value was 0
      // (i.e. one contact). This happens on Lenovo Yoga Tablet B6000-F,
      // which actually seems to support ~10 contacts. So, we'll just go with
      // as many as we can and hope that the system will ignore extra contacts.
      state.max_tracking_id = MAX_SUPPORTED_CONTACTS - 1;
      fprintf(stderr,
        "Note: type A device reports a max value of 0 for ABS_MT_TRACKING_ID. "
        "This means that the device is most likely reporting incorrect "
        "information. Guessing %d.\n",
        state.max_tracking_id
      );
    }

    state.max_contacts = state.has_mtslot
      ? libevdev_get_abs_maximum(state.evdev, ABS_MT_SLOT) + 1
      : (state.has_tracking_id ? state.max_tracking_id + 1 : 2);

    state.tracking_id = 0;

    int contact;
    for (contact = 0; contact < MAX_SUPPORTED_CONTACTS; ++contact)
    {
      state.contacts[contact].enabled = 0;
    }

    fprintf(stderr,
      "%s touch device %s (%dx%d with %d contacts) detected on %s (score %d)\n",
      state.has_mtslot ? "Type B" : "Type A",
      libevdev_get_name(state.evdev),
      state.max_x, state.max_y, state.max_contacts,
      state.path, state.score
    );

    if (state.max_contacts > MAX_SUPPORTED_CONTACTS) {
      fprintf(stderr, "Note: hard-limiting maximum number of contacts to %d\n",
        MAX_SUPPORTED_CONTACTS);
      state.max_contacts = MAX_SUPPORTED_CONTACTS;
    }
  }

  FILE* input;
  FILE* output;

  if (use_stdin || stdin_file != NULL)
  {
    if (stdin_file != NULL)
    {
      // Reading from a file
      input = fopen(stdin_file, "r");
      if (NULL == input)
      {
        fprintf(stderr, "Unable to open '%s': %s\n",
                stdin_file, strerror(errno));
        exit(EXIT_FAILURE);
      }
      else
      {
        fprintf(stderr, "Reading commands from '%s'\n",
                stdin_file);
      }
    }
    else
    {
      // Reading from terminal
      input = stdin;
      fprintf(stderr, "Reading from STDIN\n");
    }

    output = stderr;
    if(android_service_fd > 0) {
      proxy_handler(input, output, android_service_fd);
    } else {
      io_handler(input, output, &state);
    }
    fclose(input);
    fclose(output);
    exit(EXIT_SUCCESS);
  }

  struct sockaddr_un client_addr;
  socklen_t client_addr_length = sizeof(client_addr);

  int server_fd = start_server(sockname);

  if (server_fd < 0)
  {
    fprintf(stderr, "Unable to start server on %s\n", sockname);
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

    input = fdopen(client_fd, "r");
    if (input == NULL)
    {
      fprintf(stderr, "%s: fdopen(client_fd,'r')\n", strerror(errno));
      exit(1);
    }

    output = fdopen(dup(client_fd), "w");
    if (output == NULL)
    {
      fprintf(stderr, "%s: fdopen(client_fd,'w')\n", strerror(errno));
      exit(1);
    }

    if(android_service_fd > 0) {
      proxy_handler(input, output, android_service_fd);
    } else {
      io_handler(input, output, &state);
    }

    fprintf(stderr, "Connection closed\n");
    fclose(input);
    fclose(output);
    close(client_fd);
  }

  close(server_fd);

  libevdev_free(state.evdev);
  close(state.fd);

  return EXIT_SUCCESS;
}
