/*
 * NDS Card EEPROM dump/restore tool.  Works over wifi, which makes it useful
 * for slot-1 backup devices.
 *
 * Note, this software is released to the public domain.  I make no guarantees
 * regarding it's quality, safety, pleasure of reading, etc.  I'm not liable
 * for any damage caused by it's use or misuse.
 *
 * Brett Kosinski
 */

#include <nds.h>
#include <nds/arm9/console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include <fat.h>

#include <dswifi9.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define MAX(a, b) (((a) >= (b)) ? (a) : (b))

#define VERSION "1.1"

char *server_name;
int port;

/*********************
 * Utility functions.
 *********************/

char *strip(char *str)
{
  char *end = &(str[strlen(str) - 1]);

  while (1) {
    if (isspace(*str)) {
      str++;
    } else if (isspace(*end)) {
      *end-- = '\0';
    } else {
      break;
    }
  }

  return str;
}

/*************************
 * Wifi Support Functions
 *************************/

void *sgIP_malloc(int size) {
  return malloc(size);
}

void sgIP_free(void *ptr) {
  free(ptr);
}

void sgIP_dbgprint(char *txt, ...) {
}

void wifi_timer(void) {
  Wifi_Timer(50);
}

void arm9_synctoarm7() {
   REG_IPC_FIFO_TX = 0x87654321;
}

void arm9_fifo() {
  u32 value = REG_IPC_FIFO_RX;

  if (value == 0x87654321) {
    Wifi_Sync();
  }
}

void wifi_setup() {
  REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_SEND_CLEAR; // enable & clear FIFO

  u32 Wifi_pass = Wifi_Init(WIFIINIT_OPTION_USELED);

  REG_IPC_FIFO_TX = 0x12345678;
  REG_IPC_FIFO_TX = Wifi_pass;

  *((volatile u16 *)0x0400010E) = 0; // disable timer3

  irqSet(IRQ_TIMER3, wifi_timer); // setup timer IRQ
  irqEnable(IRQ_TIMER3);

  irqSet(IRQ_FIFO_NOT_EMPTY, arm9_fifo); // setup fifo IRQ
  irqEnable(IRQ_FIFO_NOT_EMPTY);

  REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_RECV_IRQ; // enable FIFO IRQ

  Wifi_SetSyncHandler(arm9_synctoarm7); // tell wifi lib to use our handler to notify arm7

  // set timer3
  *((volatile u16 *)0x0400010C) = -6553; // 6553.1 * 256 cycles = ~50ms;
  *((volatile u16 *)0x0400010E) = 0x00C2; // enable, irq, 1/256 clock

  while(Wifi_CheckInit() == 0) { // wait for arm7 to be initted successfully
    swiWaitForVBlank();  
  }
}

int wifi_connect() {
  Wifi_AutoConnect(); // request connect

  while(1) {
    int status = Wifi_AssocStatus(); // check status

    switch (status) {
      case ASSOCSTATUS_ASSOCIATED:
        iprintf("Connected to AP.\n");
        return 0;

      case ASSOCSTATUS_CANNOTCONNECT:
        iprintf("Could not connect to AP!\n");
        return -1;

      default:
        break;
    }
  }
}

/*****************
 * Main App Logic
 *****************/

/*
 * Initialize various DS subsystems.  This includes video, FAT and Wifi.
 */
int init()
{
  /* Get interrupts going. */

  irqInit();
  irqEnable(IRQ_VBLANK);

  /* Initialize the display. */

  videoSetMode(0);
  videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE);
  vramSetBankC(VRAM_C_SUB_BG);

  SUB_BG0_CR = BG_MAP_BASE(31);

  BG_PALETTE_SUB[255] = RGB15(31,31,31);

  consoleInitDefault((u16*)SCREEN_BASE_BLOCK_SUB(31), (u16*)CHAR_BASE_BLOCK_SUB(0), 16);

  /* Nice little welcome message. */

  iprintf("Welcome to savsender %s!\n", VERSION);

  /* Get FAT set up. */

  if (! fatInit(1024, 0)) {
    iprintf("Error initializing libfat!\n");
    return 0;
  }

  /* Get WiFi initialized. */

  wifi_setup();

  sysSetBusOwners(BUS_OWNER_ARM9, BUS_OWNER_ARM9);

  if (wifi_connect()) {
    iprintf("Aborting.\n");

    return -1;
  }

  return 0;
}

/*
 * The oh-so-rudimentary config file parser.  Warning, this is very 
 * rudimentary.
 */
int read_config_file()
{
  FILE *file;
  char tmp[256];

  iprintf("Opening config file...\n");

  file = fopen("fat:/data/settings/savsender.conf", "rb");

  if (file == NULL) {
    iprintf("Error opening config file!");

    return -1;
  }

  server_name = (char *)malloc(256);

  fgets(server_name, 256, file);
  fgets(tmp, sizeof(tmp), file);

  fclose(file);

  server_name = strip(server_name);
  port = atoi(strip(tmp));

  iprintf("Server is at %s:%d...\n", server_name, port);

  return 0;
}

/*
 * Establish a TCP connection to our server at the host and port specified.
 */
int connect_to_server()
{
  int fd;
  struct hostent *host;
  struct sockaddr_in addr;
  long ip;

  iprintf("Resolving host...\n");

  host = gethostbyname(server_name);

  if (host == NULL) {
    iprintf("Error!\n");
    return -1;
  }

  fd = socket(AF_INET, SOCK_STREAM, 0);

  ip = *((unsigned long *)(host->h_addr_list[0]));

  iprintf("Resolved to %d.%d.%d.%d\n", 
          ip & 0xFF,
          (ip >> 8) & 0xFF,
          (ip >> 16) & 0xFF,
          (ip >> 24) & 0xFF);

  addr.sin_addr.s_addr = ip;
  addr.sin_port = htons(port);

  iprintf("Connecting to server...\n");

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    iprintf("Error!\n");
    closesocket(fd);
    return -1;
  }

  return fd;
}

/*
 * Send the EEPROM contents of the inserted card to the server/port configured.
 */
void send_eeprom_contents()
{
  u8 *buffer;
  u8 *tmp;
  int size;
  int type;

  int fd;

  /*
   * First, read the EEPROM.
   */
  type = cardEepromGetType();
  size = cardEepromGetSize();

  if ((type < 0) || (size <= 0)) {
    iprintf("Error determining EEPROM size!\n");

    return;
  }

  iprintf("Detected EEPROM of type %d, size %d\n", type, size);

  iprintf("Reading %d bytes from EEPROM...\n", size);

  buffer = (u8 *)malloc(size);

  cardReadEeprom(0, buffer, size, type);

  /*
   * Now send the data to the host.
   */

  fd = connect_to_server();
  tmp = buffer;

  iprintf("Sending EEPROM contents");

  while (tmp < (buffer + size))
  {
    int count = send(fd, tmp, MAX(buffer - tmp, 256), 0);

    if (tmp < 0) {
      iprintf("\nError during send!\n");
      break;
    } else if (((buffer - tmp) % 1024) == 0) {
      iprintf(".");
    }

    tmp += count;

    swiWaitForVBlank();
    swiWaitForVBlank();
  }

  closesocket(fd);

  free(buffer);
}

/*
 * Connects to the server configured and reads sufficient bytes to write to
 * the EEPROM for the inserted card.  Then writes the data to the card.
 */
void recv_eeprom_contents()
{
  u8 *buffer;
  u8 *tmp;

  int size, type;
  int fd;

  /*
   * First, determine the EEPROM type.
   */
  type = cardEepromGetType();
  size = cardEepromGetSize();

  if ((type < 0) || (size <= 0)) {
    iprintf("Error determining EEPROM size!\n");

    return;
  }

  iprintf("Detected EEPROM of type %d, size %d\n", type, size);

  buffer = (u8 *)malloc(size);

  /*
   * Now read the data from the server.
   */

  fd = connect_to_server();
  tmp = buffer;

  iprintf("Reading data from server\n");

  while (tmp < (buffer + size)) {
    int count = recv(fd, tmp, MAX(buffer - tmp, 256), 0);

    if (count < 0) {
      iprintf("\nError during recv!\n");

      closesocket(fd);
      free(buffer);

      return;
    } else if (((buffer - tmp) % 1024) == 0) {
      iprintf(".");
    }

    tmp += count;

    swiWaitForVBlank();
    swiWaitForVBlank();
  }

  closesocket(fd);

  /*
   * Finally, write the data to the EEPROM.
   */
  cardEepromChipErase();
  cardWriteEeprom(0, buffer, size, type);

  free(buffer);
}

/*
 * The, ah... main event.  Har har.
 */
int main(void) 
{
  if (init() < 0) {
    return 0;
  }

  if (read_config_file() < 0) {
    return 0;
  }

  iprintf("Press A to dump, or B to restore.\n");

  while(1) {
    swiWaitForVBlank();

    scanKeys();

    int pressed = keysDown();

    if (pressed & KEY_A) {
      send_eeprom_contents();
    } else if (pressed & KEY_B) {
      recv_eeprom_contents();
    } else {
      continue;
    } 

    iprintf("Done!\n");
    iprintf("Press A to dump, or B to restore.\n");
  }

  return 0;
}
