#include "border.h"
#include "hashtable.h"
#include "events.h"
#include "windows.h"
#include "mach.h"
#include "parse.h"
#include <stdio.h>

#define VERSION_OPT_LONG "--version"
#define VERSION_OPT_SHRT "-v"

#define HELP_OPT_LONG "--help"
#define HELP_OPT_SHRT "-h"

#define MAJOR 1
#define MINOR 3
#define PATCH 3

pid_t g_pid;
struct table g_windows;
struct mach_server g_mach_server;
struct settings g_settings = { .active_window = { .stype = COLOR_STYLE_SOLID,
                                                  .color = 0xffe1e3e4 },
                               .inactive_window = { .stype = COLOR_STYLE_SOLID,
                                                    .color =  0x00000000 },
                               .border_width = 4.f,
                               .border_style = BORDER_STYLE_ROUND,
                               .hidpi = false,
                               .border_order = -1,
                               .blacklist_enabled = false,
                               .whitelist_enabled = false                    };

static TABLE_HASH_FUNC(hash_windows) {
  return *(uint32_t *) key;
}

static TABLE_COMPARE_FUNC(cmp_windows) {
  return *(uint32_t *) key_a == *(uint32_t *) key_b;
}

static TABLE_HASH_FUNC(hash_blacklist) {
  // djb2 by Dan Bernstein
  unsigned long hash = 5381;
  char c;
  while((c = *((char*)key++))) {
    hash = ((hash << 5) + hash) + c;
  }
  return hash;
}

static TABLE_COMPARE_FUNC(cmp_blacklist) {
  return strcmp((char*)key_a, (char*)key_b) == 0;
}

static void message_handler(void* data, uint32_t len) {
  char* message = data;
  uint32_t update_mask = 0;
  while(message && *message) {
    update_mask |= parse_settings(&g_settings, 1, &message);
    message += strlen(message) + 1;
  }

  if (update_mask & BORDER_UPDATE_MASK_RECREATE_ALL) {
    windows_recreate_all_borders(&g_windows);
  } else if (update_mask & BORDER_UPDATE_MASK_ALL) {
    windows_update_all(&g_windows);
  } else if (update_mask & BORDER_UPDATE_MASK_ACTIVE) {
    windows_update_active(&g_windows);
  } else if (update_mask & BORDER_UPDATE_MASK_INACTIVE) {
    windows_update_inactive(&g_windows);
  }
}

static void send_args_to_server(mach_port_t port, int argc, char** argv) {
  int message_length = argc;
  int argl[argc];

  for (int i = 1; i < argc; i++) {
    argl[i] = strlen(argv[i]);
    message_length += argl[i] + 1;
  }

  char message[(sizeof(char) * message_length)];
  char* temp = message;

  for (int i = 1; i < argc; i++) {
    memcpy(temp, argv[i], argl[i]);
    temp += argl[i];
    *temp++ = '\0';
  }
  *temp++ = '\0';

  mach_send_message(port, message, message_length);
}

static void event_callback(CFMachPortRef port, void* message, CFIndex size, void* context) {
  int cid = SLSMainConnectionID();
  CGEventRef event = SLEventCreateNextEvent(cid);
  if (!event) return;
  do {
    CFRelease(event);
    event = SLEventCreateNextEvent(cid);
  } while (event);
}

int main(int argc, char** argv) {
  if (argc > 1 && ((strcmp(argv[1], VERSION_OPT_LONG) == 0)
                   || (strcmp(argv[1], VERSION_OPT_SHRT) == 0))) {
    fprintf(stdout, "borders-v%d.%d.%d\n", MAJOR, MINOR, PATCH);
    exit(EXIT_SUCCESS);
  }

  if (argc > 1 && ((strcmp(argv[1], HELP_OPT_LONG) == 0)
                   || (strcmp(argv[1], HELP_OPT_SHRT) == 0))) {
    fprintf(stdout, "Refer to the man page for help: man borders\n");
    exit(EXIT_SUCCESS);
  }

  table_init(&g_settings.blacklist, 64, hash_blacklist, cmp_blacklist);
  table_init(&g_settings.whitelist, 64, hash_blacklist, cmp_blacklist);
  uint32_t update_mask = parse_settings(&g_settings, argc - 1, argv + 1);
  mach_port_t server_port = mach_get_bs_port(BS_NAME);
  if (server_port && update_mask) {
    send_args_to_server(server_port, argc, argv);
    return 0;
  } else if (server_port) {
    error("A borders instance is already running and no valid arguments"
          " where provided. To modify properties of the running instance"
          " provide them as arguments.\n");
  }

  pid_for_task(mach_task_self(), &g_pid);
  table_init(&g_windows, 1024, hash_windows, cmp_windows);

  int cid = SLSMainConnectionID();
  events_register(cid);
  SLSWindowManagementBridgeSetDelegate(NULL);

  mach_port_t port;
  CGError error = SLSGetEventPort(cid, &port);
  CFMachPortRef cf_mach_port = NULL;
  CFRunLoopSourceRef source = NULL;
  if (error == kCGErrorSuccess) {
    CFMachPortRef cf_mach_port = CFMachPortCreateWithPort(NULL,
                                                          port,
                                                          event_callback,
                                                          NULL,
                                                          false          );

    _CFMachPortSetOptions(cf_mach_port, 0x40);
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL,
                                                              cf_mach_port,
                                                              0            );

    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    CFRelease(cf_mach_port);
    CFRelease(source);
  }

  windows_add_existing_windows(&g_windows);

  mach_server_begin(&g_mach_server, message_handler);
  if (!update_mask) execute_config_file("borders", "bordersrc");
  CFRunLoopRun();
  return 0;
}
