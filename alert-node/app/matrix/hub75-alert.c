/*
 * hub75-alert.c - 요양원 낙상/이벤트 알림 데모 (한글 스크롤)
 *
 * 시나리오: 요양원 각 병실의 낙상 감지, 침상 이탈, 그리고 웨어러블
 *   (체온/심박/산소포화도) 센서에서 이상 이벤트가 올라온다고 가정한다
 *   이벤트가 뜨면 이 LED 패널(64x32)에 한글 경보를 큼직하게 흘려 보낸다
 *
 * 화면이 작아서(가로 64px) 문구는 오른쪽에서 왼쪽으로 스크롤한다
 *   한글은 나눔고딕Bold 16px 를 구운 hub75-font16.h 비트맵으로 그린다
 *   (기존 앱의 5x7 글꼴은 ASCII 전용이라 한글이 안 나온다)
 *
 * 픽셀도 vsync 도 표준 프레임버퍼(/dev/fbN) 하나로 받는다 - mmap 으로 그리고
 *   프레임 경계는 표준 FBIO_WAITFORVSYNC 로 기다린다. /dev/hub75 의존이 없다
 *   - hub75-scroll 데모와 같은 방식
 *
 * 이벤트는 지금은 타이머+난수로 스스로 만들어낸다 (하드웨어 없이 시연 가능)
 *   실제 센서/웨어러블 게이트웨이 연동 지점은 gen_event() 주석에 표시했다
 *
 * 빌드: make hub75-alert   (또는 app 폴더에서 make)
 * 실행: sudo ./hub75-alert
 * 종료: Ctrl-C (화면을 지우고 나간다)
 */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>			/* FBIO_WAITFORVSYNC */
#include "alert-event.h"
#include "hub75-font16.h"

#define FPS_US		30000		/* 프레임 간격 (약 33fps) - 프레임당 1px 이동 */

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* 등급별 대표색 (밝게) */
static void sev_color(enum severity s, uint8_t *r, uint8_t *g, uint8_t *b)
{
	switch (s) {
	case SEV_INFO: *r = 0;   *g = 200; *b = 60;  break;	/* 초록 */
	case SEV_WARN: *r = 255; *g = 140; *b = 0;   break;	/* 주황 */
	default:       *r = 255; *g = 30;  *b = 30;  break;	/* 빨강 */
	}
}

/* --- UTF-8 한 글자 디코드: *cp 에 코드포인트, 다음 위치 반환 --- */
static const char *utf8_next(const char *s, uint32_t *cp)
{
	uint8_t c = (uint8_t)*s;

	if (c < 0x80) {			/* ASCII */
		*cp = c;
		return s + 1;
	}
	if ((c & 0xE0) == 0xC0) {	/* 2바이트 */
		*cp = ((uint32_t)(c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
		return s + 2;
	}
	if ((c & 0xF0) == 0xE0) {	/* 3바이트 (한글이 여기) */
		*cp = ((uint32_t)(c & 0x0F) << 12) |
		      (((uint8_t)s[1] & 0x3F) << 6) |
		      ((uint8_t)s[2] & 0x3F);
		return s + 3;
	}
	*cp = '?';			/* 그 밖은 물음표로 */
	return s + 1;
}

/* 코드포인트로 글꼴 글리프 찾기 (배열이 cp 오름차순이라 이진탐색) */
static const glyph16_t *find_glyph(uint32_t cp)
{
	int lo = 0, hi = (int)FONT16_COUNT - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (font16[mid].cp == cp)
			return &font16[mid];
		if (font16[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* 문구 전체 폭(px) - 글리프 adv 합 (없는 글자는 자리 8px 로 침) */
static int measure_text(const char *text)
{
	uint32_t cp;
	int w = 0;

	while (*text) {
		text = utf8_next(text, &cp);
		const glyph16_t *g = find_glyph(cp);
		w += g ? g->adv : 8;
	}
	return w;
}

/* 글리프 하나를 (x,y)에 색 (r,g,b)로 찍는다 - 화면 밖은 잘라낸다 */
static void blit_glyph(uint8_t *fb, int cols, int rows, int stride,
		       const glyph16_t *g, int x, int y,
		       uint8_t r, uint8_t gr, uint8_t b)
{
	int row, col;

	for (row = 0; row < FONT16_H; row++) {
		uint16_t bits = g->rows[row];
		int py = y + row;

		if (py < 0 || py >= rows)
			continue;
		for (col = 0; col < 16; col++) {
			int px_x;
			uint8_t *px;

			if (!(bits & (0x8000 >> col)))
				continue;
			px_x = x + col;
			if (px_x < 0 || px_x >= cols)
				continue;
			px = &fb[py * stride + px_x * 3];
			px[0] = r; px[1] = gr; px[2] = b;
		}
	}
}

/* 문구를 x_start 위치부터 한 색으로 그린다 (스크롤 오프셋은 x_start 로 준다) */
static void draw_text(uint8_t *fb, int cols, int rows, int stride,
		      int x_start, int y, const char *text,
		      uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t cp;
	int x = x_start;

	while (*text) {
		text = utf8_next(text, &cp);
		const glyph16_t *gl = find_glyph(cp);

		if (gl) {
			/* 아예 화면 밖이면 그리기는 건너뛰고 커서만 전진 */
			if (x + 16 > 0 && x < cols)
				blit_glyph(fb, cols, rows, stride, gl, x, y, r, g, b);
			x += gl->adv;
		} else {
			x += 8;
		}
	}
}

/* 위/아래 등급색 테두리 (밝기 scale: 0=꺼짐, 255=최대 - 깜빡임용) */
static void draw_border(uint8_t *fb, int cols, int rows, int stride,
			enum severity sev, int scale)
{
	uint8_t r, g, b;
	int y, x, band = 3;

	sev_color(sev, &r, &g, &b);
	r = r * scale / 255; g = g * scale / 255; b = b * scale / 255;

	for (y = 0; y < band; y++) {
		for (x = 0; x < cols; x++) {
			uint8_t *t = &fb[y * stride + x * 3];
			uint8_t *bt = &fb[(rows - 1 - y) * stride + x * 3];
			t[0]  = r; t[1]  = g; t[2]  = b;
			bt[0] = r; bt[1] = g; bt[2] = b;
		}
	}
}

/*
 * 문구 하나를 등급색으로 passes 번 스크롤한다
 *   SEV_CRIT 이면 테두리를 깜빡여 긴급함을 준다
 * 스크롤 도중 Ctrl-C 가 오면 즉시 빠져나온다
 */
static void show_alert(uint8_t *fb, uint8_t *scratch, int cols, int rows,
		       int stride, int fbfd, const char *msg,
		       enum severity sev, int passes)
{
	size_t frame_bytes = (size_t)rows * stride;
	int text_y = (rows - FONT16_H) / 2;	/* 세로 중앙 */
	int total_w = measure_text(msg);
	int span = total_w + cols;		/* 오른쪽 등장 ~ 왼쪽 퇴장까지 */
	int offset, done = 0;
	uint8_t tr, tg, tb;
	long frame = 0;

	sev_color(sev, &tr, &tg, &tb);

	while (running && done < passes) {
		for (offset = 0; offset <= span && running; offset++, frame++) {
			int scale = 255;

			/* 긴급은 약 1Hz 로 테두리 깜빡임 (프레임 15개마다 토글) */
			if (sev == SEV_CRIT && ((frame / 15) & 1))
				scale = 40;

			memset(scratch, 0, frame_bytes);
			draw_border(scratch, cols, rows, stride, sev, scale);
			/* 문구의 왼쪽 끝 화면 x = cols - offset (오른쪽에서 흘러 들어온다) */
			draw_text(scratch, cols, rows, stride, cols - offset,
				  text_y, msg, tr, tg, tb);

			/* 프레임 경계까지 잔 뒤 한 방에 복사 - 스캔이 top 에 있을 때 앞질러 */
			ioctl(fbfd, FBIO_WAITFORVSYNC, 0);	/* 표준 fbdev vsync */
			memcpy(fb, scratch, frame_bytes);
			usleep(FPS_US);
		}
		done++;
	}
}

/* /dev/fbN 을 훑어 우리 패널(fix.id == "hub75")을 찾는다 */
static int find_hub75_fb(struct fb_var_screeninfo *var,
			 struct fb_fix_screeninfo *fix)
{
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/fb%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (ioctl(fd, FBIOGET_FSCREENINFO, fix) == 0 &&
		    strcmp(fix->id, "hub75") == 0 &&
		    ioctl(fd, FBIOGET_VSCREENINFO, var) == 0) {
			printf("패널: %s (%ux%u, %ubpp)\n", path,
			       var->xres, var->yres, var->bits_per_pixel);
			return fd;
		}
		close(fd);
	}
	return -1;
}

/* --- 이벤트 소스: 난수 데모 (하드웨어 없이 시연용) --- */

enum ev_kind {
	EV_FALL,	/* 낙상 감지 */
	EV_BEDEXIT,	/* 침상 이탈 */
	EV_FEVER,	/* 고열 (체온 센서) */
	EV_HR,		/* 심박 이상 (심박 센서) */
	EV_SPO2,	/* 산소포화도 저하 */
	EV_KIND_COUNT
};

static const int rooms[] = { 301, 302, 303, 305, 308, 311, 315 };
#define NUM_ROOMS (sizeof(rooms) / sizeof(rooms[0]))

/*
 * alert_source_fn 구현 - 이벤트 하나를 지어내 ev 를 채우고 1 을 반환한다
 *
 * MQTT 연동은 이 함수를 대체하는 소스를 하나 더 만들어 main 의
 * next_event 에 꽂으면 된다. 게이트웨이가 올려준 호실·타입·측정값을
 * 문구와 등급으로 바꾸는 일은 그 소스 안에서 끝낸다.
 */
static int demo_next_event(struct alert_event *ev)
{
	int room = rooms[rand() % NUM_ROOMS];
	enum ev_kind kind = rand() % EV_KIND_COUNT;
	size_t n = sizeof(ev->msg);

	switch (kind) {
	case EV_FALL:
		ev->sev = SEV_CRIT;
		snprintf(ev->msg, n, "긴급 %d호 낙상 발생 즉시 확인 요망", room);
		break;
	case EV_BEDEXIT:
		ev->sev = SEV_CRIT;
		snprintf(ev->msg, n, "긴급 %d호 침상 이탈 감지", room);
		break;
	case EV_FEVER:
		ev->sev = SEV_WARN;
		snprintf(ev->msg, n, "주의 %d호 고열 체온 %d.%d도",
			 room, 38 + rand() % 2, rand() % 10);
		break;
	case EV_HR:
		ev->sev = SEV_WARN;
		snprintf(ev->msg, n, "주의 %d호 심박 이상 분당 %d회",
			 room, 120 + rand() % 40);
		break;
	default: /* EV_SPO2 */
		ev->sev = SEV_WARN;
		snprintf(ev->msg, n, "주의 %d호 산소포화도 %d%%",
			 room, 85 + rand() % 5);
		break;
	}

	/* 긴급은 한 번 더 반복해 확실히 알린다 */
	ev->passes = (ev->sev == SEV_CRIT) ? 3 : 2;
	return 1;
}

int main(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int cols, rows, stride;
	size_t map_size, frame_bytes;
	uint8_t *fb, *scratch;
	int fbfd;
	alert_source_fn next_event = demo_next_event;	/* MQTT 연동 시 교체 */

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	srand((unsigned)time(NULL));

	fbfd = find_hub75_fb(&var, &fix);
	if (fbfd < 0) {
		fprintf(stderr, "hub75 프레임버퍼 못 찾음 (드라이버 로드? sudo?)\n");
		return 1;
	}
	if (var.bits_per_pixel != 24) {
		fprintf(stderr, "이 데모는 24bpp(RGB888) 전용 (현재 %ubpp)\n",
			var.bits_per_pixel);
		close(fbfd);
		return 1;
	}

	cols = var.xres;
	rows = var.yres;
	stride = fix.line_length;
	map_size = fix.smem_len;
	frame_bytes = (size_t)rows * stride;

	fb = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		close(fbfd);
		return 1;
	}

	scratch = malloc(frame_bytes);
	if (!scratch) {
		perror("malloc");
		goto cleanup;
	}

	printf("요양원 알림 데모 시작 - Ctrl-C 로 종료\n");

	while (running) {
		struct alert_event ev;

		/* 이벤트 사이 평상시: 차분한 초록으로 감시중 표시 (1회 스크롤) */
		show_alert(fb, scratch, cols, rows, stride, fbfd,
			   "정상 감시 중", SEV_INFO, 1);
		if (!running || !next_event(&ev))
			continue;

		printf("[이벤트] %s\n", ev.msg);
		show_alert(fb, scratch, cols, rows, stride, fbfd, ev.msg,
			   ev.sev, ev.passes);
	}

	memset(fb, 0, frame_bytes);		/* 나갈 때 화면을 지운다 */
	free(scratch);
cleanup:
	munmap(fb, map_size);
	close(fbfd);

	printf("\n종료했다\n");
	return 0;
}
