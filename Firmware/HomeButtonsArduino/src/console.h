#ifndef HOMEBUTTONS_CONSOLE_H
#define HOMEBUTTONS_CONSOLE_H

#include <Arduino.h>

#include "freertos/FreeRTOS.h"  // must precede queue.h
#include "freertos/queue.h"
#include "logger.h"

class App;

// A line-oriented command console on the serial ports.
//
// Exists because the interesting behaviour of this firmware - a press
// changing a counter, a scheduled reset firing at 03:00 - is otherwise only
// reachable by standing at the device with a finger on a button, or by
// waiting until tomorrow. `press` and `time set` make both testable in
// seconds, over the wire.
//
// Reading and executing are deliberately split across two contexts. The
// main loop blocks for up to HTTP_TIMEOUT inside a POST, so a reader task
// buffers input meanwhile and typed characters are not lost to a UART FIFO
// that nobody is draining. Execution then happens on the main loop, which
// is the task that already owns the webhook, NVS writes and the counters -
// running commands anywhere else would mean a second task on all three.
//
// Both ports are read. Serial is the native USB CDC and Serial0 is UART0 on
// the debug header; which one is attached varies by how the device is
// powered, and neither is worth privileging over the other. Output goes to
// both.
class Console : public Logger {
 public:
  explicit Console(App& app) : Logger("CON"), app_(app) {}
  Console(const Console&) = delete;

  // Opens both ports and starts the reader task.
  void begin();

  // Executes whatever complete lines the reader has buffered. Call from the
  // main loop.
  void service();

 private:
  static constexpr size_t LINE_MAXLEN = 192;
  static constexpr uint8_t LINE_QUEUE_SIZE = 4;
  static constexpr uint8_t MAX_ARGS = 8;
  static constexpr uint32_t READER_STACK = 3072;

  struct Line {
    char text[LINE_MAXLEN];
  };

  // Per-port assembly buffer. Bytes from the two ports interleave, so each
  // needs its own partial line.
  struct Port {
    Stream* stream = nullptr;
    char buf[LINE_MAXLEN] = {};
    size_t len = 0;
  };

  static void _reader_task(void* self);
  void _reader();
  // Feeds one byte into a port's buffer; pushes to the queue on newline.
  void _feed(Port& port, char c);
  void _execute(char* line);

  // Writes to both ports. Console replies are raw rather than going through
  // the logger: a reply is the answer to something typed, not a log event,
  // and tagging plus timestamping it makes tabular output unreadable.
  void __attribute__((format(printf, 2, 3))) _out(const char* fmt, ...) const;

  // --- command handlers ---
  void _cmd_help(int argc, char** argv);
  void _cmd_status(int argc, char** argv);
  void _cmd_press(int argc, char** argv);
  void _cmd_counter(int argc, char** argv);
  void _cmd_sched(int argc, char** argv);
  void _cmd_reset(int argc, char** argv);
  void _cmd_time(int argc, char** argv);
  void _cmd_sync(int argc, char** argv);
  void _cmd_post(int argc, char** argv);
  void _cmd_endpoint(int argc, char** argv);
  void _cmd_token(int argc, char** argv);
  void _cmd_wifi(int argc, char** argv);
  void _cmd_save(int argc, char** argv);
  void _cmd_sleep(int argc, char** argv);
  void _cmd_restart(int argc, char** argv);
  void _cmd_setup(int argc, char** argv);
  void _cmd_wifisetup(int argc, char** argv);

  struct Command {
    const char* name;
    const char* args;
    const char* help;
    void (Console::*fn)(int, char**);
  };
  static const Command kCommands[];
  static const size_t kNumCommands;

  // Resolves "a"/"b" or "0"/"1" to a counter index. Returns false and
  // reports the valid set if it is neither.
  bool _parse_counter_idx(const char* text, uint8_t& idx);

  App& app_;
  Port usb_{};
  Port uart_{};
  QueueHandle_t line_queue_ = nullptr;
  TaskHandle_t reader_task_h_ = nullptr;
};

#endif  // HOMEBUTTONS_CONSOLE_H
