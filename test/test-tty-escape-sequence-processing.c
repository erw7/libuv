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

struct screen {
  char *text;
  WORD *attributes;
  int width;
  int height;
  int length;
};

static void initialize_tty(uv_tty_t *tty_out) {
  int r;
  int ttyout_fd;

  /* Make sure we have an FD that refers to a tty */
  HANDLE handle;

  handle = CreateFileA("conout$",
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
  ASSERT(handle != INVALID_HANDLE_VALUE);
  ttyout_fd = _open_osfhandle((intptr_t) handle, 0);

  ASSERT(ttyout_fd >= 0);

  ASSERT(UV_TTY == uv_guess_handle(ttyout_fd));

  r = uv_tty_init(uv_default_loop(), tty_out, ttyout_fd, 0);  /* Writable. */
  ASSERT(r == 0);
}

static COORD get_cursor_position(HANDLE handle) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  ASSERT(GetConsoleScreenBufferInfo(handle, &info));
  return info.dwCursorPosition;
}

static BOOL is_cursor_visible(HANDLE handle) {
  CONSOLE_CURSOR_INFO info;
  ASSERT(GetConsoleCursorInfo(handle, &info));
  return info.bVisible;
}

static void write_console(uv_tty_t *tty_out, char *src) {
  int r;
  uv_buf_t buf;

  buf.base = src;
  buf.len = strlen(buf.base);

  r = uv_try_write((uv_stream_t*) tty_out, &buf, 1);
  ASSERT(r == buf.len);
}

static char* setup_screen(uv_tty_t *tty_out) {
  char *screen_buffer;
  char *p;
  int count, width, height, line;
  CONSOLE_SCREEN_BUFFER_INFO info;
  ASSERT(GetConsoleScreenBufferInfo(tty_out->handle, &info));
  width = info.dwSize.X;
  height = info.srWindow.Bottom - info.srWindow.Top + 1;
  count = info.dwSize.X * height - 1;
  screen_buffer = (char *)malloc(count * sizeof(*screen_buffer) + 1);
  ASSERT(screen_buffer != NULL);
  line = 0;
  for (p = screen_buffer; p < screen_buffer + count; p++) {
    *p = 'a' + line;
    if (((p - screen_buffer + 1) % width) == 0) {
      line = line >= 25 ? 0 : line + 1;
    }
  }
  *(screen_buffer + count) = '\0';
  write_console(tty_out, screen_buffer);
  return screen_buffer;
}

static void erase_line(struct screen *scr, int line, int pos, int dir) {
  char *start = scr->text + scr->width * (line - 1);
  char *end;
  if (dir == 0) {
    end = start + scr->width;
    start += pos - 1;
  } else if (dir == 1) {
    end = start + pos;
  } else if (dir == 2) {
    end = start + scr->width;
  } else {
    ASSERT(FALSE);
  }
  ASSERT(start < end);
  ASSERT(end - scr->text <= scr->length);
  for (; start < end ; start++) {
    *start = ' ';
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
  ASSERT(ReadConsoleOutputCharacter(tty_out->handle, scr->text, scr->length,
        origin, &length));
  ASSERT(scr->length == length);
  ASSERT(ReadConsoleOutputAttribute(tty_out->handle, scr->attributes,
        scr->length, origin, &length));
  ASSERT(scr->length == length);
}

static void compare_screen(struct screen *actual, struct screen *expect) {
  int line, col;
  char *actual_current = actual->text;
  char *expect_current = expect->text;
  while(actual_current < actual->text + actual->length &&
      expect_current < expect->text + expect->length) {
    if (*actual_current != *expect_current) {
      line = (actual_current - actual->text) / actual->width + 1;
      col = (actual_current - actual->text) - actual->width * (line - 1) + 1;
      fprintf(stderr, "line:%d col:%d expected '%c' but found '%c'\n",
          line, col, *actual_current, *expect_current);
    }
    actual_current++;
    expect_current++;
  }
}

TEST_IMPL(tty_move_cursor) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  CONSOLE_SCREEN_BUFFER_INFO info;
  COORD cursor_pos, cursor_pos_old;
  char buffer[1024];
  int top, width, height;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out);

  ASSERT(GetConsoleScreenBufferInfo(tty_out.handle, &info));
  width = info.dwSize.X;
  height = info.srWindow.Bottom - info.srWindow.Top;
  top = info.srWindow.Top;

  /* Move the cursor to home */
  snprintf(buffer, sizeof(buffer),"%sH", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(0 == cursor_pos.X);
  ASSERT(top == cursor_pos.Y);

  /* Move the cursor to the middle of the screen */
  snprintf(buffer, sizeof(buffer), "%s%d;%dH", CSI, height / 2, width / 2);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT((width / 2 - 1) == cursor_pos.X);
  ASSERT(top + (height / 2 - 1) == cursor_pos.Y);

  /* cursor up */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%sA", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y - 1 == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor up nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dA", CSI, height / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y - height / 4 == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor down */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%sB", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y + 1 == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor down nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dB", CSI, height / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y + height / 4 == cursor_pos.Y);
  ASSERT(cursor_pos_old.X == cursor_pos.X);

  /* cursor forward */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%sC", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X + 1 == cursor_pos.X);

  /* cursor forward nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dC", CSI, width / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X + width / 4 == cursor_pos.X);

  /* cursor back */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%sD", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X - 1 == cursor_pos.X);

  /* cursor back nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dD", CSI, width / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y == cursor_pos.Y);
  ASSERT(cursor_pos_old.X - width / 4 == cursor_pos.X);

  /* cursor next line */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%sE", CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y + 1 == cursor_pos.Y);
  ASSERT(0 == cursor_pos.X);

  /* cursor next line nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dC%s%dE",
      CSI, width / 4, CSI, height / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y + height / 4 == cursor_pos.Y);
  ASSERT(0 == cursor_pos.X);

  /* cursor previous line */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dC%sF",
      CSI, width / 4, CSI);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y - 1 == cursor_pos.Y);
  ASSERT(0 == cursor_pos.X);

  /* cursor next line nth times */
  cursor_pos_old = cursor_pos;
  snprintf(buffer, sizeof(buffer), "%s%dC%s%dF",
      CSI, width / 4, CSI, height / 4);
  write_console(&tty_out, buffer);
  cursor_pos = get_cursor_position(tty_out.handle);
  ASSERT(cursor_pos_old.Y - height / 4 == cursor_pos.Y);
  ASSERT(0 == cursor_pos.X);

  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_hide_show_cursor) {
  uv_tty_t tty_out;
  uv_loop_t* loop;
  char buffer[1024];

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out);

  /* Hide the cursor */
  snprintf(buffer, sizeof(buffer), "%s?25l", CSI);
  write_console(&tty_out, buffer);
  ASSERT(!is_cursor_visible(tty_out.handle));

  /* Show the cursor */
  snprintf(buffer, sizeof(buffer), "%s?25h", CSI);
  write_console(&tty_out, buffer);
  ASSERT(is_cursor_visible(tty_out.handle));

  /* Invalid sequence */
  snprintf(buffer, sizeof(buffer), "%s??25l", CSI);
  write_console(&tty_out, buffer);
  ASSERT(is_cursor_visible(tty_out.handle));

  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_erase) {
  int  dir;
  uv_tty_t tty_out;
  uv_loop_t* loop;
  char buffer[1024];
  struct screen scr_actual;
  struct screen scr_expect;

  uv__set_vterm_state(UV_UNSUPPORTED);

  loop = uv_default_loop();

  initialize_tty(&tty_out);

  /* Erase to right */
  dir = 0;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  erase_line(&scr_expect, scr_expect.height / 2, scr_expect.width / 2, dir);

  snprintf(buffer, sizeof(buffer), "%s%d;%dH%s%dK",
      CSI, scr_expect.height / 2, scr_expect.width / 2, CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);
  snprintf(buffer, sizeof(buffer), "%sH%sJ", CSI, CSI);
  write_console(&tty_out, buffer);

  ASSERT(scr_actual.length == scr_expect.length);
  ASSERT(scr_actual.width == scr_expect.width);
  ASSERT(scr_actual.height == scr_expect.height);
  compare_screen(&scr_actual, &scr_expect);
  ASSERT(!strncmp(scr_actual.text, scr_expect.text, scr_actual.length));

  /* Erase to Left */
  dir = 1;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  erase_line(&scr_expect, scr_expect.height / 2, scr_expect.width / 2, dir);

  snprintf(buffer, sizeof(buffer), "%s%d;%dH%s%dK",
      CSI, scr_expect.height / 2, scr_expect.width / 2, CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);
  snprintf(buffer, sizeof(buffer), "%sH%sJ", CSI, CSI);
  write_console(&tty_out, buffer);

  ASSERT(scr_actual.length == scr_expect.length);
  ASSERT(scr_actual.width == scr_expect.width);
  ASSERT(scr_actual.height == scr_expect.height);
  compare_screen(&scr_actual, &scr_expect);
  ASSERT(!strncmp(scr_actual.text, scr_expect.text, scr_actual.length));

  /* Erase All */
  dir = 2;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  erase_line(&scr_expect, scr_expect.height / 2, scr_expect.width / 2, dir);

  snprintf(buffer, sizeof(buffer), "%s%d;%dH%s%dK",
      CSI, scr_expect.height / 2, scr_expect.width / 2, CSI, dir);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);
  snprintf(buffer, sizeof(buffer), "%sH%sJ", CSI, CSI);
  write_console(&tty_out, buffer);

  ASSERT(scr_actual.length == scr_expect.length);
  ASSERT(scr_actual.width == scr_expect.width);
  ASSERT(scr_actual.height == scr_expect.height);
  compare_screen(&scr_actual, &scr_expect);
  ASSERT(!strncmp(scr_actual.text, scr_expect.text, scr_actual.length));

  /* Omit argument(Erase to right) */
  dir = 0;
  setup_screen(&tty_out);
  capture_screen(&tty_out, &scr_expect);
  erase_line(&scr_expect, scr_expect.height / 2, scr_expect.width / 2, dir);

  snprintf(buffer, sizeof(buffer), "%s%d;%dH%sK",
      CSI, scr_expect.height / 2, scr_expect.width / 2, CSI);
  write_console(&tty_out, buffer);
  capture_screen(&tty_out, &scr_actual);
  snprintf(buffer, sizeof(buffer), "%sH%sJ", CSI, CSI);
  write_console(&tty_out, buffer);

  ASSERT(scr_actual.length == scr_expect.length);
  ASSERT(scr_actual.width == scr_expect.width);
  ASSERT(scr_actual.height == scr_expect.height);
  compare_screen(&scr_actual, &scr_expect);
  ASSERT(!strncmp(scr_actual.text, scr_expect.text, scr_actual.length));

  uv_close((uv_handle_t*) &tty_out, NULL);

  uv_run(loop, UV_RUN_DEFAULT);

  MAKE_VALGRIND_HAPPY();
  return 0;
}


TEST_IMPL(tty_set_style) {
  /* TODO Implement set style test */
  return 0;
}


TEST_IMPL(tty_save_restore_cursor_position) {
  /* TODO Implement save restore cursor position */
  return 0;
}
