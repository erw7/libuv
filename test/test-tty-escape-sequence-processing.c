/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "task.h"

#include <io.h>
#include <windows.h>

#include <string.h>
#include <errno.h>

#include "../src/win/internal.h"

#define ESC "\033"
#define CSI ESC "["
#define HELLO "Hello"

#define FOREGROUND_WHITE (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define FOREGROUND_BLACK 0
#define FOREGROUND_YELLOW (FOREGROUND_RED | FOREGROUND_GREEN)
#define FOREGROUND_CYAN (FOREGROUND_GREEN | FOREGROUND_BLUE)
#define FOREGROUND_MAGENTA (FOREGROUND_RED | FOREGROUND_BLUE)
#define BACKGROUND_WHITE (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE)
#define BACKGROUND_BLACK 0
#define BACKGROUND_YELLOW (BACKGROUND_RED | BACKGROUND_GREEN)
#define BACKGROUND_CYAN (BACKGROUND_GREEN | BACKGROUND_BLUE)
#define BACKGROUND_MAGENTA (BACKGROUND_RED | BACKGROUND_BLUE)

#define F_INTENSITY      1
#define FB_INTENSITY     2
#define B_INTENSITY      5
#define INVERSE          7
#define F_INTENSITY_OFF1 21
#define F_INTENSITY_OFF2 22
#define B_INTENSITY_OFF  25
#define INVERSE_OFF      27
#define F_BLACK          30
#define F_RED            31
#define F_GREEN          32
#define F_YELLOW         33
#define F_BLUE           34
#define F_MAGENTA        35
#define F_CYAN           36
#define F_WHITE          37
#define F_DEFAULT        39
#define B_BLACK          40
#define B_RED            41
#define B_GREEN          42
#define B_YELLOW         43
#define B_BLUE           44
#define B_MAGENTA        45
#define B_CYAN           46
#define B_WHITE          47
#define B_DEFAULT        49

#define CURSOR_SIZE_SMALL     25
#define CURSOR_SIZE_MIDDLE    50
#define CURSOR_SIZE_LARGE     100

struct screen {
  char *text;
  WORD *attributes;
  int top;
  int width;
  int height;
  int length;
  WORD default_attr;
};

static void initialize_tty(uv_tty_t *tty_out, struct screen *scr) {
  int r;
  int ttyout_fd;
  CONSOLE_SCREEN_BUFFER_INFO info;
  SMALL_RECT rect;
  /* Make sure we have an FD that refers to a tty */
  HANDLE handle;

  uv__set_vterm_state(UV_UNSUPPORTED);

  handle = CreateFileA("conout$",
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
  ASSERT(handle != INVALID_HANDLE_VALUE);

  ASSERT(GetConsoleScreenBufferInfo(handle, &info));
  scr->text = NULL;
  scr->attributes = NULL;
  scr->width = info.dwSize.X;
  scr->height = info.srWindow.Bottom - info.srWindow.Top + 1;
  scr->length = scr->width * scr->height;
  scr->default_attr = info.wAttributes;

  rect.Left = 0;
  rect.Top = info.dwCursorPosition.Y + scr->height > info.dwSize.Y ?
    info.dwSize.Y - scr->height - 1 : info.dwCursorPosition.Y;
  rect.Right = scr->width - 1;
  rect.Bottom = rect.Top + scr->height - 1;
  ASSERT(SetConsoleWindowInfo(handle, TRUE, &rect));

  scr->top = rect.Top;

  ttyout_fd = _open_osfhandle((intptr_t) handle, 0);
  ASSERT(ttyout_fd >= 0);
  ASSERT(UV_TTY == uv_guess_handle(ttyout_fd));
  r = uv_tty_init(uv_default_loop(), tty_out, ttyout_fd, 0);  /* Writable. */
  ASSERT(r == 0);
}

static COORD get_cursor_position(uv_tty_t *tty_out) {
  HANDLE handle = tty_out->handle;
  CONSOLE_SCREEN_BUFFER_INFO info;
  COORD ret;
  ASSERT(GetConsoleScreenBufferInfo(handle, &info));
  ret.X = info.dwCursorPosition.X + 1;
  ret.Y = info.dwCursorPosition.Y - info.srWindow.Top + 1;
  return ret;
}

static void set_cursor_position(uv_tty_t *tty_out, COORD pos) {
  HANDLE handle = tty_out->handle;
  CONSOLE_SCREEN_BUFFER_INFO info;
  ASSERT(GetConsoleScreenBufferInfo(handle, &info));
  pos.X -= 1;
  pos.Y += info.srWindow.Top - 1;
  ASSERT(SetConsoleCursorPosition(handle, pos));
}

static void set_cursor_size(uv_tty_t *tty_out, DWORD size) {
  HANDLE handle = tty_out->handle;
  CONSOLE_CURSOR_INFO info;
  ASSERT(GetConsoleCursorInfo(handle, &info));
  info.dwSize = size;
  ASSERT(SetConsoleCursorInfo(handle, &info));
}

static void set_cursor_to_home(uv_tty_t *tty_out) {
  COORD origin = {1, 1};
  set_cursor_position(tty_out, origin);
}

static CONSOLE_CURSOR_INFO get_cursor_info(uv_tty_t *tty_out) {
  HANDLE handle = tty_out->handle;
  CONSOLE_CURSOR_INFO info;
  ASSERT(GetConsoleCursorInfo(handle, &info));
  return info;
}

static BOOL is_cursor_visible(uv_tty_t *tty_out) {
  return get_cursor_info(tty_out).bVisible;
}

static BOOL is_scrolling(uv_tty_t *tty_out, struct screen scr) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  ASSERT(GetConsoleScreenBufferInfo(tty_out->handle, &info));
  return info.srWindow.Top != scr.top;
}

static DWORD get_cursor_size(uv_tty_t *tty_out) {
  return get_cursor_info(tty_out).dwSize;
}

static void write_console(uv_tty_t *tty_out, char *src) {
  int r;
  uv_buf_t buf;

  buf.base = src;
  buf.len = strlen(buf.base);

  r = uv_try_write((uv_stream_t*) tty_out, &buf, 1);
  ASSERT(r >= 0);
  ASSERT((unsigned int)r == buf.len);
}

static void setup_screen(uv_tty_t *tty_out) {
  DWORD length, number_of_written;
  COORD origin;
  CONSOLE_SCREEN_BUFFER_INFO info;
  ASSERT(GetConsoleScreenBufferInfo(tty_out->handle, &info));
  length = info.dwSize.X * (info.srWindow.Bottom - info.srWindow.Top + 1);
  origin.X = 0;
  origin.Y = info.srWindow.Top;
  ASSERT(FillConsoleOutputCharacter(tty_out->handle, '.', length, origin,
        &number_of_written));
  ASSERT(length == number_of_written);
}

static void clear_screen(uv_tty_t *tty_out, struct screen *scr) {
  DWORD length, number_of_written;
  COORD origin;
  CONSOLE_SCREEN_BUFFER_INFO info;
  ASSERT(GetConsoleScreenBufferInfo(tty_out->handle, &info));
  length = (info.srWindow.Bottom - info.srWindow.Top + 1) * info.dwSize.X - 1;
  origin.X = 0;
  origin.Y = info.srWindow.Top;
  FillConsoleOutputCharacterA(tty_out->handle, ' ', length, origin,
      &number_of_written);
  ASSERT(length == number_of_written);
  FillConsoleOutputAttribute(tty_out->handle, scr->default_attr, length, origin,
      &number_of_written);
  ASSERT(length == number_of_written);
}

static void make_expect_screen_erase(struct screen *scr, COORD cursor_position,
    int dir, BOOL entire_screen) {
  /* beginning of line */
  char *start = scr->text + scr->width * (cursor_position.Y - 1);
  char *end;
  if (dir == 0) {
    if (entire_screen) {
      /* erase to end of screen */
      end = scr->text + scr->length;
    } else {
      /* erase to end of line */
      end = start + scr->width;
    }
    /* erase from postition of cursor */
    start += cursor_position.X - 1;
  } else if (dir == 1) {
    /* erase to position of cursor */
    end = start + cursor_position.X;
    if (entire_screen) {
      /* erase form beginning of screen */
      start = scr->text;
    }
  } else if (dir == 2) {
    if (entire_screen) {
      /* erase form beginning of screen */
      start = scr->text;
      /* erase to end of screen */
      end = scr->text + scr->length;
    } else {
      /* erase to end of line */
      end = start + scr->width;
    }
  } else {
    ASSERT(FALSE);
  }
  ASSERT(start < end);
  ASSERT(end - scr->text <= scr->length);
  for (; start < end ; start++) {
    *start = ' ';
  }
}

static void make_expect_screen_write(struct screen *scr, COORD cursor_position,
    const char *text) {
  /* postion of cursor */
  char *start = scr->text + scr->width * (cursor_position.Y - 1) +
    cursor_position.X - 1;
  size_t length = strlen(text);
  size_t remain_length = scr->length - (scr->text - start);
  length = length > remain_length ? remain_length : length;
  memcpy(start, text, length);
}

static void make_expect_screen_set_attr(struct screen *scr,
    COORD cursor_position, size_t length, WORD attr) {
  WORD *start = scr->attributes + scr->width * (cursor_position.Y - 1) +
    cursor_position.X - 1;
  size_t remain_length = scr->length - (scr->attributes - start);
  length = length > remain_length ? remain_length : length;
  while(length) {
    *start = attr;
    start++;
    length--;
  }
}

static void capture_screen(uv_tty_t *tty_out, struct screen *scr) {
  DWORD length;
  CONSOLE_SCREEN_BUFFER_INFO info;
  COORD origin;
  ASSERT(GetConsoleScreenBufferInfo(tty_out->handle, &info));
  scr->width = info.dwSize.X;
  scr->height = info.srWindow.Bottom - info.srWindow.Top + 1;
  scr->length = scr->width * scr->height;
  origin.X = 0;
  origin.Y = info.srWindow.Top;
  scr->text = (char *)malloc(scr->length * sizeof(*scr->text));
  scr->attributes = (WORD *)malloc(scr->length * sizeof(*scr->attributes));
  scr->default_attr = info.wAttributes;
  ASSERT(ReadConsoleOutputCharacter(tty_out->handle, scr->text, scr->length,
        origin, &length));
  ASSERT((unsigned int)scr->length == length);
  ASSERT(ReadConsoleOutputAttribute(tty_out->handle, scr->attributes,
        scr->length, origin, &length));
  ASSERT((unsigned int)scr->length == length);
}

static void free_screen(struct screen *scr) {
  free(scr->text);
  free(scr->attributes);
}

static BOOL compare_screen(uv_tty_t *tty_out,
    struct screen *actual, struct screen *expect) {
  int line, col;
  BOOL result = TRUE;
  int current = 0;
  if (actual->length != expect->length) {
    return FALSE;
  }
  if (actual->width != expect->width) {
    return FALSE;
  }
  if (actual->height != expect->height) {
    return FALSE;
  }
  while(current < actual->length) {
    if (*(actual->text + current) != *(expect->text + current)) {
      line = current / actual->width + 1;
      col = current - actual->width * (line - 1) + 1;
      fprintf(stderr, "line:%d col:%d expected character '%c' but found '%c'\n",
          line, col, *(expect->text + current), *(actual->text + current));
      result = FALSE;
    }
    if (*(actual->attributes + current) != *(expect->attributes + current)) {
      line = current / actual->width + 1;
      col = current - actual->width * (line - 1) + 1;
      fprintf(stderr, "line:%d col:%d expected attributes '%u' but found '%u'\n",
          line, col,
          *(expect->attributes + current), *(actual->attributes + current));
      result = FALSE;
    }
    current++;
  }
  clear_screen(tty_out, expect);
  free_screen(expect);
  free_screen(actual);
  return result;
}

TEST_IMPL(tty_cursor_up) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr;

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* cursor up one times if omitted arguments */
  snprintf(buffer, sizeof(buffer), "%sA", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y - 1 == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor up nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dA", CSI, scr.height / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y - scr.height / 4 == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor up from Window top does nothing */
  cursor_pos_old.X = 1;
  cursor_pos_old.Y = 1;
  set_cursor_position(&tty_out, cursor_pos_old);
  snprintf(buffer, sizeof(buffer), "%sA", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);
  ASSERT(!is_scrolling(&tty_out, scr));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_cursor_down) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr;

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* cursor down one times if omitted arguments */
  snprintf(buffer, sizeof(buffer), "%sB", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y + 1 == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor down nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dB", CSI, scr.height / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y + scr.height / 4 == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor down from bottom line does nothing */
  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height;
  set_cursor_position(&tty_out, cursor_pos_old);
  snprintf(buffer, sizeof(buffer), "%sB", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);
  ASSERT(!is_scrolling(&tty_out, scr));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_cursor_forward) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr;

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* cursor forward one times if omitted arguments */
  snprintf(buffer, sizeof(buffer), "%sC", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X + 1 == cursor_pos.X);

  /* cursor forward nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dC", CSI, scr.width / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X + scr.width / 4 == cursor_pos.X);

  /* cursor forward from end of line does nothing*/
  cursor_pos_old.X = scr.width;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);
  snprintf(buffer, sizeof(buffer), "%sC", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor forward from end of screen does nothing */
  cursor_pos_old.X = scr.width;
  cursor_pos_old.Y = scr.height;
  set_cursor_position(&tty_out, cursor_pos_old);
  snprintf(buffer, sizeof(buffer), "%sC", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);
  ASSERT(!is_scrolling(&tty_out, scr));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_cursor_back) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr;

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* cursor back one times if omitted arguments */
  snprintf(buffer, sizeof(buffer), "%sD", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X - 1 == cursor_pos.X);

  /* cursor back nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dD", CSI, scr.width / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X - scr.width / 4 == cursor_pos.X);

  /* cursor back from beginning of line does nothing */
  cursor_pos_old.X = 1;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);
  snprintf(buffer, sizeof(buffer), "%sD", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor back from top of screen does nothing */
  cursor_pos_old.X = 1;
  cursor_pos_old.Y = 1;
  set_cursor_position(&tty_out, cursor_pos_old);
  snprintf(buffer, sizeof(buffer), "%sD", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(1 == cursor_pos.Y);
  ASSERT(1 == cursor_pos.X);
  ASSERT(!is_scrolling(&tty_out, scr));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_cursor_next_line) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* cursor next line one times if omitted arguments */
  snprintf(buffer, sizeof(buffer), "%sE", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y + 1 == cursor_pos.Y);
  ASSERT(1 == cursor_pos.X);

  /* cursor next line nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dE", CSI, scr.height / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y + scr.height / 4 == cursor_pos.Y);
  ASSERT(1 == cursor_pos.X);

  /* cursor next line from buttom row moves beginning of line */
  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height;
  set_cursor_position(&tty_out, cursor_pos_old);
  snprintf(buffer, sizeof(buffer), "%sE", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(1 == cursor_pos.X);
  ASSERT(!is_scrolling(&tty_out, scr));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_cursor_previous_line) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* cursor previous line one times if omitted arguments */
  snprintf(buffer, sizeof(buffer), "%sF", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y - 1 == cursor_pos.Y);
  ASSERT(1 == cursor_pos.X);

  /* cursor previous line nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dF", CSI, scr.height / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos_old.Y - scr.height / 4 == cursor_pos.Y);
  ASSERT(1 == cursor_pos.X);

  /* cursor previous line from top of screen does nothing */
  cursor_pos_old.X = 1;
  cursor_pos_old.Y = 1;
  set_cursor_position(&tty_out, cursor_pos_old);
  snprintf(buffer, sizeof(buffer), "%sD", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(1 == cursor_pos.Y);
  ASSERT(1 == cursor_pos.X);
  ASSERT(!is_scrolling(&tty_out, scr));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_cursor_horizontal_move_absolute) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* Move to beginning of line if omitted argument */
  snprintf(buffer, sizeof(buffer),"%sG", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(1 == cursor_pos.X);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);

  /* Move cursor to nth character */
  snprintf(buffer, sizeof(buffer),"%s%dG", CSI, scr.width / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(scr.width / 4 == cursor_pos.X);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);

  /* Moving out of screen will fit within screen */
  snprintf(buffer, sizeof(buffer),"%s%dG", CSI, scr.width + 1);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(scr.width == cursor_pos.X);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_cursor_move_absolute) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos;
  char buffer[1024];
  struct screen scr;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos.X = scr.width / 2;
  cursor_pos.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos);

  /* Move the cursor to home if omitted arguments */
  snprintf(buffer, sizeof(buffer),"%sH", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(1 == cursor_pos.X);
  ASSERT(1 == cursor_pos.Y);

  /* Move the cursor to the middle of the screen */
  snprintf(buffer, sizeof(buffer), "%s%d;%df",
      CSI, scr.height / 2, scr.width / 2);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(scr.width / 2 == cursor_pos.X);
  ASSERT(scr.height / 2 == cursor_pos.Y);

  /* Moving out of screen will fit within screen */
  snprintf(buffer, sizeof(buffer), "%s%d;%df",
      CSI, scr.height / 2, scr.width + 1);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(scr.width == cursor_pos.X);
  ASSERT(scr.height / 2 == cursor_pos.Y);

  snprintf(buffer, sizeof(buffer), "%s%d;%df",
      CSI, scr.height + 1, scr.width / 2);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(scr.width / 2 == cursor_pos.X);
  ASSERT(scr.height == cursor_pos.Y);
  ASSERT(!is_scrolling(&tty_out, scr));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_hide_show_cursor) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  char buffer[1024];
  struct screen scr;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  /* Hide the cursor */
  snprintf(buffer, sizeof(buffer), "%s?25l", CSI);
  write_console(&tty_out, buffer);
  ASSERT(!is_cursor_visible(&tty_out));

  /* Show the cursor */
  snprintf(buffer, sizeof(buffer), "%s?25h", CSI);
  write_console(&tty_out, buffer);
  ASSERT(is_cursor_visible(&tty_out));

  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_erase) {
  int  dir;
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos;
  char buffer[1024];
  struct screen scr_actual;
  struct screen scr_expect;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr_expect);

  /* Erase to below if omitted argument */
  dir = 0;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  cursor_pos.X = scr_expect.width / 2;
  cursor_pos.Y = scr_expect.height / 2;
  make_expect_screen_erase(&scr_expect, cursor_pos, dir, TRUE);

  set_cursor_position(&tty_out, cursor_pos);
  snprintf(buffer, sizeof(buffer), "%sJ", CSI);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  /* Erase to below(dir = 0) */
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  make_expect_screen_erase(&scr_expect, cursor_pos, dir, TRUE);

  set_cursor_position(&tty_out, cursor_pos);
  snprintf(buffer, sizeof(buffer), "%s%dJ", CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  /* Erase to above */
  dir = 1;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  make_expect_screen_erase(&scr_expect, cursor_pos, dir, TRUE);

  set_cursor_position(&tty_out, cursor_pos);
  snprintf(buffer, sizeof(buffer), "%s%dJ", CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  /* Erase All */
  dir = 2;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  make_expect_screen_erase(&scr_expect, cursor_pos, dir, TRUE);

  set_cursor_position(&tty_out, cursor_pos);
  snprintf(buffer, sizeof(buffer), "%s%dJ", CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_erase_line) {
  int  dir;
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos;
  char buffer[1024];
  struct screen scr_actual;
  struct screen scr_expect;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr_expect);

  /* Erase to right if omitted arguments */
  dir = 0;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  cursor_pos.X = scr_expect.width / 2;
  cursor_pos.Y = scr_expect.height / 2;
  make_expect_screen_erase(&scr_expect, cursor_pos, dir, FALSE);

  set_cursor_position(&tty_out, cursor_pos);
  snprintf(buffer, sizeof(buffer), "%sK", CSI);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  /* Erase to right(dir = 0) */
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  make_expect_screen_erase(&scr_expect, cursor_pos, dir, FALSE);

  set_cursor_position(&tty_out, cursor_pos);
  snprintf(buffer, sizeof(buffer), "%s%dK", CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  /* Erase to Left */
  dir = 1;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  make_expect_screen_erase(&scr_expect, cursor_pos, dir, FALSE);

  set_cursor_position(&tty_out, cursor_pos);
  snprintf(buffer, sizeof(buffer), "%s%dK", CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  /* Erase All */
  dir = 2;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  make_expect_screen_erase(&scr_expect, cursor_pos, dir, FALSE);

  set_cursor_position(&tty_out, cursor_pos);
  snprintf(buffer, sizeof(buffer), "%s%dK", CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_set_cursor_shape) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  DWORD saved_cursor_size;
  char buffer[1024];
  struct screen scr;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  saved_cursor_size = get_cursor_size(&tty_out);

  /* cursor size large if omitted arguments */
  set_cursor_size(&tty_out, CURSOR_SIZE_MIDDLE);
  snprintf(buffer, sizeof(buffer), "%s q", CSI);
  write_console(&tty_out, buffer);
  ASSERT(get_cursor_size(&tty_out) == CURSOR_SIZE_LARGE);

  /* cursor size large */
  set_cursor_size(&tty_out, CURSOR_SIZE_MIDDLE);
  snprintf(buffer, sizeof(buffer), "%s1 q", CSI);
  write_console(&tty_out, buffer);
  ASSERT(get_cursor_size(&tty_out) == CURSOR_SIZE_LARGE);
  set_cursor_size(&tty_out, CURSOR_SIZE_MIDDLE);
  snprintf(buffer, sizeof(buffer), "%s2 q", CSI);
  write_console(&tty_out, buffer);
  ASSERT(get_cursor_size(&tty_out) == CURSOR_SIZE_LARGE);

  /* cursor size small */
  set_cursor_size(&tty_out, CURSOR_SIZE_MIDDLE);
  snprintf(buffer, sizeof(buffer), "%s3 q", CSI);
  write_console(&tty_out, buffer);
  ASSERT(get_cursor_size(&tty_out) == CURSOR_SIZE_SMALL);
  set_cursor_size(&tty_out, CURSOR_SIZE_MIDDLE);
  snprintf(buffer, sizeof(buffer), "%s6 q", CSI);
  write_console(&tty_out, buffer);
  ASSERT(get_cursor_size(&tty_out) == CURSOR_SIZE_SMALL);

  /* Nothing occurs with arguments outside valid range */
  set_cursor_size(&tty_out, CURSOR_SIZE_MIDDLE);
  snprintf(buffer, sizeof(buffer), "%s7 q", CSI);
  write_console(&tty_out, buffer);
  ASSERT(get_cursor_size(&tty_out) == CURSOR_SIZE_MIDDLE);

  /* restore cursor size if arguments is zero */
  snprintf(buffer, sizeof(buffer), "%s0 q", CSI);
  write_console(&tty_out, buffer);
  ASSERT(get_cursor_size(&tty_out) == saved_cursor_size);

  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_set_style) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos;
  char buffer[1024];
  struct screen scr_actual;
  struct screen scr_expect;
  WORD fg, bg;
  WORD fg_attrs[9][2] = {
    {F_BLACK, FOREGROUND_BLACK},
    {F_RED, FOREGROUND_RED},
    {F_GREEN, FOREGROUND_GREEN},
    {F_YELLOW, FOREGROUND_YELLOW},
    {F_BLUE, FOREGROUND_BLUE},
    {F_MAGENTA, FOREGROUND_MAGENTA},
    {F_CYAN, FOREGROUND_CYAN},
    {F_WHITE, FOREGROUND_WHITE},
    {F_DEFAULT, 0}
  };
  WORD bg_attrs[9][2] = {
    {B_DEFAULT, 0},
    {B_BLACK, BACKGROUND_BLACK},
    {B_RED, BACKGROUND_RED},
    {B_GREEN, BACKGROUND_GREEN},
    {B_YELLOW, BACKGROUND_YELLOW},
    {B_BLUE, BACKGROUND_BLUE},
    {B_MAGENTA, BACKGROUND_MAGENTA},
    {B_CYAN, BACKGROUND_CYAN},
    {B_WHITE, BACKGROUND_WHITE}
  };
  WORD attr;
  int i, length;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr_expect);

  fg_attrs[8][1] = scr_expect.default_attr & FOREGROUND_WHITE;
  bg_attrs[0][1] = scr_expect.default_attr & BACKGROUND_WHITE;

  /* Set foreground color */
  length = ARRAY_SIZE(fg_attrs);
  for (i = 0; i < length; i++) {
    capture_screen(&tty_out, &scr_expect);
    cursor_pos.X = scr_expect.width / 2;
    cursor_pos.Y = scr_expect.height / 2;
    attr = (scr_expect.default_attr & ~FOREGROUND_WHITE) | fg_attrs[i][1];
    make_expect_screen_write(&scr_expect, cursor_pos, HELLO);
    make_expect_screen_set_attr(&scr_expect, cursor_pos, strlen(HELLO),
        attr);

    set_cursor_position(&tty_out, cursor_pos);
    snprintf(buffer, sizeof(buffer), "%s%dm%s%sm", CSI, fg_attrs[i][0], HELLO, CSI);
    write_console(&tty_out, buffer);
    capture_screen(&tty_out, &scr_actual);

    ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));
  }

  /* Set background color */
  length = ARRAY_SIZE(bg_attrs);
  for (i = 0; i < length; i++) {
    capture_screen(&tty_out, &scr_expect);
    cursor_pos.X = scr_expect.width / 2;
    cursor_pos.Y = scr_expect.height / 2;
    attr = (scr_expect.default_attr & ~BACKGROUND_WHITE) | bg_attrs[i][1];
    make_expect_screen_write(&scr_expect, cursor_pos, HELLO);
    make_expect_screen_set_attr(&scr_expect, cursor_pos, strlen(HELLO),
        attr);

    set_cursor_position(&tty_out, cursor_pos);
    snprintf(buffer, sizeof(buffer), "%s%dm%s%sm", CSI, bg_attrs[i][0], HELLO, CSI);
    write_console(&tty_out, buffer);
    capture_screen(&tty_out, &scr_actual);

    ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));
  }

  /* Set foregroud and background color */
  ASSERT(ARRAY_SIZE(fg_attrs) == ARRAY_SIZE(bg_attrs));
  length = ARRAY_SIZE(bg_attrs);
  for (i = 0; i < length; i++) {
    capture_screen(&tty_out, &scr_expect);
    cursor_pos.X = scr_expect.width / 2;
    cursor_pos.Y = scr_expect.height / 2;
    attr = scr_expect.default_attr & ~FOREGROUND_WHITE & ~BACKGROUND_WHITE;
    attr |= fg_attrs[i][1] | bg_attrs[i][1];
    make_expect_screen_write(&scr_expect, cursor_pos, HELLO);
    make_expect_screen_set_attr(&scr_expect, cursor_pos, strlen(HELLO),
        attr);

    set_cursor_position(&tty_out, cursor_pos);
    snprintf(buffer, sizeof(buffer), "%s%d;%dm%s%sm",
        CSI, bg_attrs[i][0], fg_attrs[i][0], HELLO, CSI);
    write_console(&tty_out, buffer);
    capture_screen(&tty_out, &scr_actual);

    ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));
  }

  /* Set foreground bright on */
  capture_screen(&tty_out, &scr_expect);
  cursor_pos.X = scr_expect.width / 2;
  cursor_pos.Y = scr_expect.height / 2;
  set_cursor_position(&tty_out, cursor_pos);
  attr = scr_expect.default_attr;
  attr |= FOREGROUND_INTENSITY;
  make_expect_screen_write(&scr_expect, cursor_pos, HELLO);
  make_expect_screen_set_attr(&scr_expect, cursor_pos, strlen(HELLO),
      attr);
  cursor_pos.X += strlen(HELLO);
  make_expect_screen_write(&scr_expect, cursor_pos, HELLO);
  make_expect_screen_set_attr(&scr_expect, cursor_pos, strlen(HELLO),
      attr);

  snprintf(buffer, sizeof(buffer), "%s%dm%s%s%dm%s%dm%s%s%dm",
      CSI, F_INTENSITY, HELLO, CSI, F_INTENSITY_OFF1,
      CSI, F_INTENSITY, HELLO, CSI, F_INTENSITY_OFF2);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  /* Set background bright on */
  capture_screen(&tty_out, &scr_expect);
  cursor_pos.X = scr_expect.width / 2;
  cursor_pos.Y = scr_expect.height / 2;
  set_cursor_position(&tty_out, cursor_pos);
  attr = scr_expect.default_attr;
  attr |= BACKGROUND_INTENSITY;
  make_expect_screen_write(&scr_expect, cursor_pos, HELLO);
  make_expect_screen_set_attr(&scr_expect, cursor_pos, strlen(HELLO),
      attr);

  snprintf(buffer, sizeof(buffer), "%s%dm%s%s%dm",
      CSI, B_INTENSITY, HELLO, CSI, B_INTENSITY_OFF);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  /* Inverse */
  capture_screen(&tty_out, &scr_expect);
  cursor_pos.X = scr_expect.width / 2;
  cursor_pos.Y = scr_expect.height / 2;
  set_cursor_position(&tty_out, cursor_pos);
  attr = scr_expect.default_attr;
  fg = attr & FOREGROUND_WHITE;
  bg = attr & BACKGROUND_WHITE;
  attr &= (~FOREGROUND_WHITE & ~BACKGROUND_WHITE);
  attr |= COMMON_LVB_REVERSE_VIDEO;
  attr |= fg << 4;
  attr |= bg >> 4;
  make_expect_screen_write(&scr_expect, cursor_pos, HELLO);
  make_expect_screen_set_attr(&scr_expect, cursor_pos, strlen(HELLO),
      attr);
  cursor_pos.X += strlen(HELLO);
  make_expect_screen_write(&scr_expect, cursor_pos, HELLO);

  snprintf(buffer, sizeof(buffer), "%s%dm%s%s%dm%s",
      CSI, INVERSE, HELLO, CSI, INVERSE_OFF, HELLO);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);

  ASSERT(compare_screen(&tty_out, &scr_actual, &scr_expect));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_save_restore_cursor_position) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* save the cursor position */
  snprintf(buffer, sizeof(buffer), "%ss", CSI);
  write_console(&tty_out, buffer);

  cursor_pos.X = scr.width / 4;
  cursor_pos.Y = scr.height / 4;
  set_cursor_position(&tty_out, cursor_pos);

  /* restore the cursor postion */
  snprintf(buffer, sizeof(buffer), "%su", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos.X == cursor_pos_old.X);
  ASSERT(cursor_pos.Y == cursor_pos_old.Y);

  cursor_pos_old.X = scr.width / 2;
  cursor_pos_old.Y = scr.height / 2;
  set_cursor_position(&tty_out, cursor_pos_old);

  /* save the cursor position */
  snprintf(buffer, sizeof(buffer), "%s7", ESC);
  write_console(&tty_out, buffer);

  cursor_pos.X = scr.width / 4;
  cursor_pos.Y = scr.height / 4;
  set_cursor_position(&tty_out, cursor_pos);

  /* restore the cursor postion */
  snprintf(buffer, sizeof(buffer), "%s8", ESC);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  ASSERT(cursor_pos.X == cursor_pos_old.X);
  ASSERT(cursor_pos.Y == cursor_pos_old.Y);

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

TEST_IMPL(tty_escape_sequence_processing) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  struct screen scr_actual;
  struct screen scr_expect;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out, &scr_expect);

  cursor_pos_old = get_cursor_position(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  snprintf(buffer, sizeof(buffer), "%s@%s~", CSI, CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(&tty_out);
  capture_screen(&tty_out, &scr_actual);
  ASSERT(cursor_pos.X == cursor_pos_old.X);
  ASSERT(cursor_pos.Y == cursor_pos_old.Y);
  ASSERT(compare_screen(&scr_actual, &scr_expect));

  set_cursor_to_home(&tty_out);
  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

