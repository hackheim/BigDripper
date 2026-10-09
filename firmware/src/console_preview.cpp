#include "console_preview.h"

#if defined(CONSOLE_PREVIEW) || defined(CONSOLE_PREVIEW_DRY_RUN)

// Serial0 is UART0 (GPIO43/44), which is what the CP2102 bridge carries: the
// same cable and port used for flashing. Plain `Serial` is native USB here
// (ARDUINO_USB_CDC_ON_BOOT=1).
static const uint32_t PREVIEW_BAUD = 115200;

// Smooths bursts: a 16-char line + CRLF is 18 bytes, so this holds ~220
// lines on top of the queue below.
static const size_t PREVIEW_TX_BUFFER = 4096;

#ifdef CONSOLE_PREVIEW

#define PREVIEW_CORE 0
static const UBaseType_t PREVIEW_QUEUE_DEPTH = 256;

enum class ItemKind : uint8_t { Column, Message, Mode, Skipped };

struct Item {
  ItemKind kind;
  uint16_t bits;      // Column
  int32_t n;          // Skipped: column count. Mode: PrintMode.
  uint32_t dropped;   // lines dropped just before this one
  char text[48];      // Message, truncated
};

static QueueHandle_t preview_queue = NULL;

// Only the print engine (valve task) pushes, so this needs no lock. It's
// carried by the next item that does fit, so the drop note lands exactly
// where the gap in the picture is.
static uint32_t pending_drops = 0;

static void push(Item &item) {
  if (preview_queue == NULL) return;
  item.dropped = pending_drops;
  if (xQueueSend(preview_queue, &item, 0) == pdTRUE) {
    pending_drops = 0;
  } else {
    pending_drops++;
  }
}

void console_preview_column(uint16_t bits) {
  Item item;
  item.kind = ItemKind::Column;
  item.bits = bits;
  push(item);
}

void console_preview_message(const QueuedText &m, bool is_default) {
  Item item;
  item.kind = ItemKind::Message;
  // Cap the text so the font and flags always fit, backing off so a UTF-8
  // character (ÆØÅ) isn't cut in half.
  int len = m.text.length();
  if (len > 20) {
    len = 20;
    while (len > 0 && (m.text[len] & 0xC0) == 0x80) len--;
  }
  snprintf(item.text, sizeof(item.text), "%.*s [%s%s]%s", len, m.text.c_str(),
           font_label(m.font), m.invert ? ", inverted" : "",
           is_default ? " (default)" : "");
  push(item);
}

void console_preview_mode(PrintMode mode) {
  Item item;
  item.kind = ItemKind::Mode;
  item.n = (int32_t)mode;
  push(item);
}

void console_preview_skipped(int32_t n) {
  Item item;
  item.kind = ItemKind::Skipped;
  item.n = n;
  push(item);
}

static void preview_task(void *arg) {
  Item item;
  char line[80];
  for (;;) {
    xQueueReceive(preview_queue, &item, portMAX_DELAY);

    if (item.dropped) {
      Serial0.printf("!! dropped %lu lines\r\n", (unsigned long)item.dropped);
    }

    switch (item.kind) {
      case ItemKind::Column: {
        uint16_t bit = 0x8000;  // COIL1 first, like set_coil_state()
        for (int i = 0; i < 16; i++) {
          line[i] = (item.bits & bit) ? '#' : '.';
          bit >>= 1;
        }
        line[16] = '\r';
        line[17] = '\n';
        // Blocks once the TX buffer is full, which is fine here: it only
        // backs up this queue, never the valve task.
        Serial0.write((const uint8_t *)line, 18);
        break;
      }
      case ItemKind::Message:
        Serial0.printf("-- %s --\r\n", item.text);
        break;
      case ItemKind::Mode:
        Serial0.printf("-- mode: %s --\r\n", mode_name((PrintMode)item.n));
        break;
      case ItemKind::Skipped:
        Serial0.printf("~~ skipped %ld ~~\r\n", (long)item.n);
        break;
    }
  }
}

#endif  // CONSOLE_PREVIEW

void console_preview_begin() {
  Serial0.setTxBufferSize(PREVIEW_TX_BUFFER);  // must come before begin()
  Serial0.begin(PREVIEW_BAUD);
  Serial0.println();

#ifdef CONSOLE_PREVIEW_DRY_RUN
  // So a dry-run board on the bike isn't mistaken for a broken one. Also on
  // native USB, in case that's the port someone plugs into.
  static const char BANNER[] = "*** DRY RUN: valves disabled (CONSOLE_PREVIEW_DRY_RUN) ***";
  Serial0.println(BANNER);
  Serial.println(BANNER);
#endif

#ifdef CONSOLE_PREVIEW
  Serial0.println("-- console preview: one line per column, COIL1 on the left --");
  Serial0.println("-- rotate 90 degrees anticlockwise to read --");
  preview_queue = xQueueCreate(PREVIEW_QUEUE_DEPTH, sizeof(Item));
  // Priority 1, below the encoder task: it only ever waits on the queue or
  // the UART, so IDLE0 keeps running and the watchdog stays fed.
  xTaskCreatePinnedToCore(preview_task, "preview", 3072, NULL, 1, NULL, PREVIEW_CORE);
#endif
}

#endif  // CONSOLE_PREVIEW || CONSOLE_PREVIEW_DRY_RUN
