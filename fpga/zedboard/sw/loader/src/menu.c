/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ROM list selection shared by the terminal UI and the OLED/buttons.
 */

#include "menu.h"

void menu_init(menu_t *m, int count, int sel)
{
	m->count = count;
	m->sel = (sel >= 0 && sel < count) ? sel : 0;
	m->num = 0;
	m->num_ms = 0;
}

void menu_move(menu_t *m, int delta)
{
	int s;

	m->num = 0;
	if (m->count <= 0)
		return;
	s = m->sel + delta;
	if (delta == 1 || delta == -1) {
		s = (s + m->count) % m->count;
	} else {
		if (s < 0)
			s = 0;
		if (s >= m->count)
			s = m->count - 1;
	}
	m->sel = s;
}

int menu_digit(menu_t *m, int digit, uint32_t now_ms)
{
	int n = digit;

	if (m->num > 0 && now_ms - m->num_ms < MENU_DIGIT_MS)
		n = m->num * 10 + digit;
	if (n > m->count)
		n = digit;				// "1", "5" with 9 ROMs goes to 5
	m->num_ms = now_ms;
	if (n < 1 || n > m->count) {
		m->num = 0;
		return 0;
	}
	m->num = n;
	m->sel = n - 1;
	return 1;
}

void menu_tick(menu_t *m, uint32_t now_ms)
{
	if (m->num && now_ms - m->num_ms >= MENU_DIGIT_MS)
		m->num = 0;
}

int menu_scroll(int top, int sel, int count, int height)
{
	if (height <= 0)
		return 0;
	if (sel < top)
		top = sel;
	if (sel >= top + height)
		top = sel - height + 1;
	if (top > count - height)
		top = count - height;
	if (top < 0)
		top = 0;
	return top;
}

int menu_marquee(int len, int width, uint32_t ms)
{
	uint32_t steps, t;

	if (len <= width)
		return 0;
	steps = (uint32_t)(len - width);
	t = ms % (2 * MARQUEE_HOLD_MS + steps * MARQUEE_STEP_MS);
	if (t < MARQUEE_HOLD_MS)
		return 0;
	t -= MARQUEE_HOLD_MS;
	if (t >= steps * MARQUEE_STEP_MS)
		return (int)steps;
	return (int)(t / MARQUEE_STEP_MS) + 1;
}
