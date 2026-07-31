#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/kthread.h>   	// 커널 스레드
#include <linux/delay.h>     	// ndelay, usleep_range
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/io.h>			// ioremap, writel
#include <linux/vmalloc.h>		// vmalloc_user, vfree
#include <linux/mm.h>			// PAGE_ALIGN, vm_area_struct
#include <linux/fb.h>			// fbdev 프레임버퍼 서브시스템
#include <linux/wait.h>			// wait_event_interruptible, waitqueue
#include "hub75-ioctl.h"		// ioctl 명령 번호

#define GPIO_BASE		0xFE200000		// BCM2711(Pi 4) GPIO 물리 베이스
#define GPIO_LEN		0xB4			// GPIO 레지스터 영역 크기
#define GPSET0			0x1C			// 비트 N=1 → GPIO N HIGH
#define GPCLR0			0x28			// 비트 N=1 → GPIO N LOW

/* PWM 하드웨어 (OE 펄스용) - BCM2711 */
#define PWM_BASE		0xFE20C000		// PWM0 물리 베이스
#define PWM_LEN			0x28
#define PWM_CTL			0x00			// 제어
#define PWM_STA			0x04			// 상태
#define PWM_RNG1		0x10			// 채널1 주기 (틱 수)
#define PWM_FIF1		0x18			// FIFO 입구
#define PWM_CTL_CLRF1	BIT(6)			// FIFO 비우기
#define PWM_CTL_USEF1	BIT(5)			// FIFO 사용
#define PWM_CTL_POLA1	BIT(4)			// 출력 반전 (쉴 때 HIGH = 소등)
#define PWM_CTL_PWEN1	BIT(0)			// 채널1 켜기
#define PWM_STA_EMPT1	BIT(1)			// FIFO 비었음

/* 클럭 매니저 (PWM 에 클럭 공급) */
#define CM_BASE			0xFE101000
#define CM_LEN			0xA8
#define CM_PWMCTL		0xA0			// PWM 클럭 제어
#define CM_PWMDIV		0xA4			// PWM 클럭 분주
#define CM_PASSWD		(0x5A << 24)	// 모든 쓰기에 필요한 암호
#define CM_KILL			BIT(5)			// 클럭 강제 정지
#define CM_ENAB			BIT(4)			// 클럭 켜기
#define CM_SRC_OSC		1				// 소스 = 54MHz 오실레이터
#define CM_DIV			6				// 54MHz / 6 = 9MHz -> 틱 111ns

#define GPFSEL1			0x04			// GPIO 10~19 기능 선택 레지스터
#define OE_PIN			12
#define OE_FSEL_SHIFT	((OE_PIN % 10) * 3)	// GPFSEL1 안에서 OE 핀의 3비트 자리

/* 핫루프용 핀 번호 - DTS 와 반드시 일치 */
#define CLK_PIN			17
static const int data_pin[6] = { 11, 27, 7, 8, 9, 10 };  // R1 G1 B1 R2 G2 B2

#define COLOR_BITS	11	// 출력 심도 (입력은 8비트, LUT 가 11비트로 확장)
#define BASE_TICKS	2	// 최하위 평면 발광 틱 - 고정 (밝기는 LUT 값 스케일링으로)
#define PLANE_BIT(v, plane) 	(((v) >> (plane)) & 1) // v(11비트 값)의 plane 번째 비트

static int brightness = 255;	// 밝기 0~255 (시간이 아니라 LUT 값을 스케일링)
module_param(brightness, int, 0644);
MODULE_PARM_DESC(brightness, "brightness 0-255 (value-domain scaling)");

static int gamma = 1; // 1: gamma 켬, 0: 끔
module_param(gamma, int, 0644);
MODULE_PARM_DESC(gamma, "apply gamma 2.2 correction (1=on, 0=off)");

static int refresh_cpu = 3;
module_param(refresh_cpu, int, 0444); // 쓰기 제한
MODULE_PARM_DESC(refresh_cpu, "CPU to bind the refresh thread to (-1 = no binding)");

// 드라이버의 모든 상태를 담는 그릇 (probe 에서 채움)
struct hub75 {
	/* 패널 크기 (DTS 에서 읽음) */
	u32					rows, cols, scan;

	/* 핀 - gpiod (런타임 사용: 주소/래치, oe 는 소등 복원용)
	   clk/data 는 probe 에서 예약만 하고 레지스터로 구동하므로 여기 없음 */
	struct gpio_desc	*lat, *oe;			// 단일 핀
	struct gpio_descs	*addr;				// 주소 핀 배열

	/* 핀 - 레지스터 직접 (핫루프: 데이터/클럭) */
	void __iomem		*gpio_regs;			// ioremap 한 GPIO 레지스터
	u32					data_mask[6];		// 데이터 6핀 비트 마스크
	u32					data_mask_all;		// 데이터 6핀 전체 마스크 (clr 여집합용)
	u32					clk_mask;			// CLK 비트 마스크
	void __iomem		*pwm_regs, *cm_regs;	// OE 펄스용 PWM / 클럭 레지스터

	/* 프레임버퍼 (단일 버퍼 - 유저가 그리는 그대로 스캔) */
	u8					*fb;				// 유일한 프레임버퍼
	size_t				fb_size;			// 한 프레임 크기 (rows*cols*3)
	struct mutex		write_lock;			// writer 끼리 직렬화
	wait_queue_head_t	vsync_wait;			// 프레임 경계 대기자
	u32					vsync_count;		// 프레임마다 증가 (경계 신호)

	/* 표시 변환 (감마 x 밝기를 구운 조회표) */
	u16					lut[256];			// 8비트 입력 -> 11비트 표시값
	int					lut_brightness;		// lut 를 구울 때 쓴 파라미터 (변경 감지용)
	int					lut_gamma;

	/* 구동 주체 */
	struct task_struct	*thread;			// 리프레시 스레드
	struct miscdevice 	misc;				// /dev/hub75 문자 장치
	struct fb_info		*info;				// fbdev /dev/fbN
	u32					pseudo_palette[16];	// truecolor 콘솔용 16색 팔레트
};

/* 감마 2.2 보정 테이블 (11비트 스케일): gamma11_base[v] = round(2047*(v/255)^2.2) */
static const u16 gamma11_base[256] = {
	   0,    0,    0,    0,    0,    0,    1,    1,
	   1,    1,    2,    2,    2,    3,    3,    4,
	   5,    5,    6,    7,    8,    8,    9,   10,
	  11,   12,   13,   15,   16,   17,   18,   20,
	  21,   23,   24,   26,   28,   29,   31,   33,
	  35,   37,   39,   41,   43,   45,   47,   50,
	  52,   54,   57,   59,   62,   65,   67,   70,
	  73,   76,   79,   82,   85,   88,   91,   94,
	  98,  101,  105,  108,  112,  115,  119,  123,
	 127,  131,  135,  139,  143,  147,  151,  155,
	 160,  164,  169,  173,  178,  183,  187,  192,
	 197,  202,  207,  212,  217,  223,  228,  233,
	 239,  244,  250,  255,  261,  267,  273,  279,
	 285,  291,  297,  303,  309,  316,  322,  328,
	 335,  342,  348,  355,  362,  369,  376,  383,
	 390,  397,  404,  412,  419,  427,  434,  442,
	 449,  457,  465,  473,  481,  489,  497,  505,
	 513,  522,  530,  539,  547,  556,  565,  573,
	 582,  591,  600,  609,  618,  628,  637,  646,
	 656,  665,  675,  685,  694,  704,  714,  724,
	 734,  744,  755,  765,  775,  786,  796,  807,
	 817,  828,  839,  850,  861,  872,  883,  894,
	 905,  917,  928,  940,  951,  963,  975,  987,
	 998, 1010, 1022, 1035, 1047, 1059, 1071, 1084,
	1096, 1109, 1122, 1135, 1147, 1160, 1173, 1186,
	1199, 1213, 1226, 1239, 1253, 1266, 1280, 1294,
	1308, 1321, 1335, 1349, 1364, 1378, 1392, 1406,
	1421, 1435, 1450, 1465, 1479, 1494, 1509, 1524,
	1539, 1554, 1570, 1585, 1600, 1616, 1631, 1647,
	1663, 1678, 1694, 1710, 1726, 1743, 1759, 1775,
	1791, 1808, 1824, 1841, 1858, 1875, 1891, 1908,
	1925, 1943, 1960, 1977, 1994, 2012, 2029, 2047,
};

/*
 * 감마 x 밝기를 구운 조회표 갱신 - 파라미터가 바뀌었을 때만 불린다
 * 밝기는 시간(틱)이 아니라 여기서 값을 줄여서 만든다 (리프레시 불변)
 */
static void hub75_update_lut(struct hub75 *h)
{
	int b_raw = brightness;	// 전역 파라미터는 진입 때 딱 한 번만 읽는다
	int g = gamma;
	int b = b_raw;		// 클램프는 사본에만 (원값은 캐시용으로 보존)
	int v;
	u16 base;

	if (b < 0)   b = 0;
	if (b > 255) b = 255;	// 안 가두면 lut 가 2047 을 넘어 비트11 위로 새서
				// 밝은 픽셀이 오히려 어둡게 랩어라운드된다

	for (v = 0; v < 256; v++) {
		base = g ? gamma11_base[v] : ((u16)v << 3);	// off 면 선형 (8비트 -> 11비트 폭)
		h->lut[v] = ((u32)base * b + 127) / 255;	// 정수 반올림 스케일
	}
	h->lut_brightness = b_raw;	// 변경 감지용 - 전역 재읽기하면 그 사이 바뀐 값이 끼어든다
	h->lut_gamma = g;
}


/* PWM 을 "정밀 원샷 펄스 기계"로 준비 */
static void hub75_pwm_init(struct hub75 *h)
{
	u32 fsel;

	/* PWM: FIFO 사용 + 출력 반전(쉴 때 HIGH = 소등) + FIFO 비우기 */
	writel(PWM_CTL_USEF1 | PWM_CTL_POLA1 | PWM_CTL_CLRF1, h->pwm_regs + PWM_CTL);

	/* PWM 클럭: 정지 -> 소스 선택 -> 분주 설정 -> 켜기 (전부 암호 필요) */
	writel(CM_PASSWD | CM_KILL,              h->cm_regs + CM_PWMCTL);
	writel(CM_PASSWD | CM_SRC_OSC,           h->cm_regs + CM_PWMCTL);
	writel(CM_PASSWD | (CM_DIV << 12),       h->cm_regs + CM_PWMDIV);
	writel(CM_PASSWD | CM_ENAB | CM_SRC_OSC, h->cm_regs + CM_PWMCTL);

	/* 준비 다 된 뒤에야 핀을 ALT0(PWM 출력)로 전환 - idle 이 HIGH 라 소등 유지 */
	fsel = readl(h->gpio_regs + GPFSEL1);
	fsel &= ~(7 << OE_FSEL_SHIFT);	// OE 핀 자리(핀당 3비트) 비우고
	fsel |= (4 << OE_FSEL_SHIFT);	// ALT0(=4) 를 넣음
	writel(fsel, h->gpio_regs + GPFSEL1);
}

/* ticks 틱짜리 LOW 펄스를 발사한다 (1틱 = CM_DIV 가 정하는 단위, 현재 약 111ns) */
static void hub75_pwm_pulse(struct hub75 *h, int ticks)
{
	if (ticks < 2)		// 하드웨어 최소값(2) 미달이면 안 쏨 = 소등 유지
		return;

	if (ticks < 16) {
		writel(ticks, h->pwm_regs + PWM_RNG1);	// 주기 = ticks
		writel(ticks, h->pwm_regs + PWM_FIF1);	// 값=주기 -> 100% 듀티 = 꽉 찬 LOW 한 주기
	} else {
		/* 주기를 짧게 쪼개 여러 번 (센티널 낭비 축소)
		   조각 수는 rem < chunk 이 보장되게 고른다 - 안 그러면
		   나머지 항목의 값이 주기보다 커져 하드웨어가 잘라버림 */
		int nchunk = (ticks < 64) ? 4 : 8;
		int chunk = ticks / nchunk;
		int rem = ticks % nchunk;
		int i;

		writel(chunk, h->pwm_regs + PWM_RNG1);
		for (i = 0; i < nchunk; i++)
			writel(chunk, h->pwm_regs + PWM_FIF1);
		if (rem)
			writel(rem, h->pwm_regs + PWM_FIF1);	// 나머지 틱 - 이 주기엔 rem 틱만 LOW
	}
	writel(0, h->pwm_regs + PWM_FIF1);	// 센티널 1 - 출력을 idle(HIGH)로 복귀시킴
	writel(0, h->pwm_regs + PWM_FIF1);	// 센티널 2 - EMPT 감지가 제대로 되게

	writel(PWM_CTL_USEF1 | PWM_CTL_POLA1 | PWM_CTL_PWEN1, h->pwm_regs + PWM_CTL); // 발사!
}

/* 펄스가 끝날 때까지 기다리고 PWM 을 재장전 상태로 되돌린다 */
static void hub75_pwm_wait(struct hub75 *h)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(10);	// 최대 10ms

	while (!(readl(h->pwm_regs + PWM_STA) & PWM_STA_EMPT1)) {
		if (time_after(jiffies, deadline)) {
			pr_warn_once("hub75: pwm pulse timeout\n");
			break;
		}
		cpu_relax();
	}

	writel(PWM_CTL_USEF1 | PWM_CTL_POLA1 | PWM_CTL_CLRF1, h->pwm_regs + PWM_CTL);
}

/*
 * hub75_pwm_init 의 역순 - OE 를 일반 출력-HIGH(소등)로 되돌리고 PWM/클럭 정지
 * quiesce(rmmod/shutdown)와 probe 에러 경로가 공용으로 쓴다
 */
static void hub75_pwm_off(struct hub75 *h)
{
	u32 fsel;

	/* OE 를 다시 일반 출력-HIGH(소등)로 - 래치를 먼저 소등값으로 채우고 전환 */
	gpiod_set_value(h->oe, 0);		// 출력 래치 = 소등(물리 HIGH)
	fsel = readl(h->gpio_regs + GPFSEL1);
	fsel &= ~(7 << OE_FSEL_SHIFT);
	fsel |= (1 << OE_FSEL_SHIFT);		// 출력(=1)로 복원
	writel(fsel, h->gpio_regs + GPFSEL1);

	/* PWM 정지 + 클럭 끄기 */
	writel(0, h->pwm_regs + PWM_CTL);
	writel(CM_PASSWD | CM_KILL, h->cm_regs + CM_PWMCTL);
}

/* 테스트 패턴: 왼쪽 어두움 -> 오른쪽 밝음 회색 그라데이션 */
static void hub75_fill_test(struct hub75 *h)
{
	int row, col;

	for (row = 0; row < h->rows; row++) {
		for (col = 0; col < h->cols; col++) {
			u8 *px = &h->fb[(row * h->cols + col) * 3];
			u8 gray = col * 255 / (h->cols - 1);
			px[0] = px[1] = px[2] = gray;
		}
	}
}

/*
 * 패널을 쉬지 않고 다시 그리는 스레드
 * 행쌍(row)마다 비트평면(plane)을 돌며
 *   [시프트(직전 펄스 발광과 겹침) -> 펄스 대기 -> 주소 -> 래치 -> 발사]
 * 평면 plane 의 발광 시간 = BASE_TICKS << plane 틱 (고정, BCM 으로 색심도 표현)
 * 밝기는 시간이 아니라 LUT 값 스케일링으로 조절 (리프레시 불변)
 * 발광 중에 다음 데이터를 시프트하는 파이프라인 - 래치 덕분에 안전
 */
static int hub75_refresh(void *arg)
{
	struct hub75 *h = arg;
	int row, col, plane;

	while (!kthread_should_stop()) {
		u8 *cur = h->fb;

		/* 밝기/감마 파라미터가 바뀌었으면 조회표를 다시 굽는다 (프레임당 비교 1번) */
		if (brightness != h->lut_brightness || gamma != h->lut_gamma)
			hub75_update_lut(h);

		for (row = 0; row < h->scan; ++row) {		// 16개 행쌍
			for (plane = 0; plane < COLOR_BITS; ++plane) {	// 비트평면
				for (col = 0; col < h->cols; ++col) { // 64번 클럭
					u8 *top = &cur[(row * h->cols + col) * 3];
					u8 *bot = &cur[((row + h->scan) * h->cols + col) * 3];
					
					int bits[6] = {
						PLANE_BIT(h->lut[top[0]], plane), PLANE_BIT(h->lut[top[1]], plane), PLANE_BIT(h->lut[top[2]], plane),
						PLANE_BIT(h->lut[bot[0]], plane), PLANE_BIT(h->lut[bot[1]], plane), PLANE_BIT(h->lut[bot[2]], plane),
					};
					u32 set = 0, clr;
					int ch;

					/* 켤 핀만 모으고, 끌 핀은 XOR 여집합으로 */
					for (ch = 0; ch < 6; ++ch)
						if (bits[ch])
							set |= h->data_mask[ch];
					clr = h->data_mask_all ^ set;	// 켠 것 빼고 데이터핀 전부
					writel(set, h->gpio_regs + GPSET0);	// HIGH 한 번에
					writel(clr, h->gpio_regs + GPCLR0);	// LOW  한 번에
					ndelay(50);

					writel(h->clk_mask, h->gpio_regs + GPSET0); // CLK up
					ndelay(50);
					writel(h->clk_mask, h->gpio_regs + GPCLR0); // CLK down
				}
				
				hub75_pwm_wait(h);

				if (plane == 0) {	// 주소는 행이 바뀔 때만 (row 하위 4비트가 그대로 A~D)
					unsigned long addr_bits = row;

					gpiod_set_array_value(h->addr->ndescs, h->addr->desc,
							      h->addr->info, &addr_bits);
				}

				gpiod_set_value(h->lat, 1);	// 래치 펄스 - 시프트한 걸 출력으로 확정
				gpiod_set_value(h->lat, 0);

				/* 발사만 하고 대기 없이 다음 반복으로 - 발광 중에 다음 시프트가 겹침 */
				hub75_pwm_pulse(h, BASE_TICKS << plane);
			}
		}

		/* 프레임 완성 신호 - 대기자 깨우기 (WRITE_ONCE 먼저, 그다음 wake) */
		WRITE_ONCE(h->vsync_count, h->vsync_count + 1);
		wake_up_interruptible(&h->vsync_wait);

		cond_resched();
	}
	return 0;
}

/* 유저가 /dev/hub75 에 쓴 픽셀을 프레임버퍼로 복사 */
static ssize_t hub75_write(struct file *file, const char __user *buf,
						size_t count, loff_t *ppos)
{
	struct hub75 *h = file->private_data;	// .open 에서 h 로 세팅해둠
	loff_t pos = *ppos; // long offset type
	ssize_t ret;

	mutex_lock(&h->write_lock);

	if (pos >= h->fb_size) {
		ret = 0;
		goto out;
	}

	if (count > h->fb_size - pos)
		count = h->fb_size - pos;
	
	if (copy_from_user(h->fb + pos, buf, count)) {
		ret = -EFAULT;
		goto out;
	}

	*ppos = pos + count;
	ret = count;
out:
	mutex_unlock(&h->write_lock);
	return ret;
}

/* 프레임 경계까지 대기 - misc ioctl 과 fbdev FBIO_WAITFORVSYNC 공용 */
static int hub75_wait_vsync(struct hub75 *h)
{
	u32 start = READ_ONCE(h->vsync_count);

	/* 카운터가 바뀔 때까지 잠듦 - 시그널 오면 중단 */
	if (wait_event_interruptible(h->vsync_wait, READ_ONCE(h->vsync_count) != start))
		return -ERESTARTSYS;
	return 0;
}

/* 제어 채널 - 밝기/감마/클리어/vsync */
static long hub75_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct hub75 *h = file->private_data;	// .open 에서 h 로 세팅해둠
	void __user *argp = (void __user *)arg;
	int v;

	/* 매직 넘버 확인 */
	if (_IOC_TYPE(cmd) != HUB75_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case HUB75_CLEAR:
		mutex_lock(&h->write_lock);
		memset(h->fb, 0, h->fb_size);
		mutex_unlock(&h->write_lock);
		return 0;

	case HUB75_SET_BRIGHTNESS:
		if (get_user(v, (int __user *)argp))
			return -EFAULT;
		brightness = v;
		return 0;

	case HUB75_GET_BRIGHTNESS:
		if (put_user(brightness, (int __user *)argp))
			return -EFAULT;
		return 0;

	case HUB75_SET_GAMMA:
		if (get_user(v, (int __user *)argp))
			return -EFAULT;
		gamma = !!v;
		return 0;

	case HUB75_GET_GAMMA:
		if (put_user(gamma, (int __user *)argp))
			return -EFAULT;
		return 0;

	case HUB75_WAIT_VSYNC:
		return hub75_wait_vsync(h);

	default:
		return -ENOTTY;
	}
}

/* fb 버퍼를 유저 vma 에 매핑 - misc 와 fbdev mmap 의 공용 로직 */
static int hub75_map_fb(struct hub75 *h, struct vm_area_struct *vma)
{
	// 오프셋 0부터만 - 우리는 버퍼 전체를 처음부터 매핑
	if (vma->vm_pgoff != 0)
		return -EINVAL;
	// 요청 크기가 프레임버퍼(페이지 정렬분)를 넘으면 거부
	if (vma->vm_end - vma->vm_start > PAGE_ALIGN(h->fb_size))
		return -EINVAL;
	// vmalloc 페이지들을 유저 vma 에 연결 - 이래서 vmalloc_user 로 잡았던 것
	return remap_vmalloc_range(vma, h->fb, 0);
}

/* misc(/dev/hub75) mmap - 공용 헬퍼로 위임 */
static int hub75_mmap(struct file *file, struct vm_area_struct *vma)
{
	return hub75_map_fb(file->private_data, vma);
}

/* open - private_data 를 miscdevice 에서 h 로 바꿔둔다
   이후 write/ioctl/mmap 이 container_of 없이 바로 h 를 씀 */
static int hub75_open(struct inode *inode, struct file *file)
{
	file->private_data = container_of(file->private_data, struct hub75, misc);
	return 0;
}

/* 유저스페이스 시스템콜을 커널 함수에 연결 */
static const struct file_operations hub75_fops = {
	.owner 			= THIS_MODULE,
	.open			= hub75_open,
	.write 			= hub75_write,
	.unlocked_ioctl	= hub75_ioctl,
	.mmap			= hub75_mmap,
	.llseek 		= default_llseek,
};

/* truecolor 팔레트 채우기 - fbcon 등이 fg/bg 색을 우리 포맷으로 팩하게 */
static int hub75_fb_setcolreg(unsigned int regno, unsigned int red,
			      unsigned int green, unsigned int blue,
			      unsigned int transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;

	if (regno >= 16)
		return -EINVAL;

	/* fbdev 는 채널당 16비트를 줌 - 8비트로 줄여 var 오프셋대로 팩 */
	pal[regno] = ((red   >> 8) << info->var.red.offset)   |
		     ((green >> 8) << info->var.green.offset) |
		     ((blue  >> 8) << info->var.blue.offset);
	return 0;
}

/* fbdev(/dev/fbN) mmap - 공용 헬퍼로 위임 (misc 와 동일 로직) */
static int hub75_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	return hub75_map_fb(info->par, vma);
}

/* fbdev ioctl - 표준 FBIO_WAITFORVSYNC 를 우리 vsync 에 연결
   순수 fbdev 앱도 /dev/hub75 없이 vsync 가능 (crtc 인덱스 arg 는 단일 디스플레이라 무시) */
static int hub75_fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case FBIO_WAITFORVSYNC:
		return hub75_wait_vsync(info->par);
	default:
		return -ENOTTY;
	}
}

/* fbdev 연산 테이블 - 그리기는 vmalloc 용 sys_* (MMIO 용 cfb_* 아님) */
static const struct fb_ops hub75_fbops = {
	.owner			= THIS_MODULE,
	.fb_read		= fb_sys_read,		// cat < /dev/fbN
	.fb_write		= fb_sys_write,		// cat > /dev/fbN
	.fb_fillrect	= sys_fillrect,		// fbcon 사각 채우기
	.fb_copyarea	= sys_copyarea,		// fbcon 스크롤
	.fb_imageblit	= sys_imageblit,	// fbcon 글리프
	.fb_setcolreg	= hub75_fb_setcolreg,
	.fb_mmap		= hub75_fb_mmap,
	.fb_ioctl		= hub75_fb_ioctl,	// FBIO_WAITFORVSYNC
};

/* fb_info 의 var(가변)/fix(고정)/ops 를 우리 패널 값으로 채운다 */
static void hub75_fb_setup(struct hub75 *h)
{
	struct fb_info *info = h->info;

	info->fbops				= &hub75_fbops;
	info->screen_buffer		= h->fb;			// vmalloc 버퍼 직접 (VIRTFB)
	info->screen_size		= h->fb_size;
	info->pseudo_palette	= h->pseudo_palette;
	info->flags				= FBINFO_VIRTFB;	// screen 이 vmalloc 임을 표시
	info->par				= h;				// 콜백에서 h 되찾기

	/* 가변 정보 - RGB888, 메모리 바이트순서 R,G,B 에 맞춘 오프셋 */
	info->var.xres = info->var.xres_virtual = h->cols;
	info->var.yres = info->var.yres_virtual = h->rows;
	info->var.bits_per_pixel = 24;
	info->var.red    	= (struct fb_bitfield){ 0,  8, 0 };	// 최하위 바이트 = R
	info->var.green  	= (struct fb_bitfield){ 8,  8, 0 };
	info->var.blue   	= (struct fb_bitfield){ 16, 8, 0 };
	info->var.transp 	= (struct fb_bitfield){ 0,  0, 0 };
	info->var.activate 	= FB_ACTIVATE_NOW;
	info->var.vmode    	= FB_VMODE_NONINTERLACED;
	info->var.width = info->var.height = -1;		// 물리 크기 미상

	/* 고정 정보 */
	strscpy(info->fix.id, "hub75", sizeof(info->fix.id));
	info->fix.type			= FB_TYPE_PACKED_PIXELS;
	info->fix.visual		= FB_VISUAL_TRUECOLOR;
	info->fix.line_length	= h->cols * 3;		// stride: 한 줄 바이트
	info->fix.smem_len		= h->fb_size;
	info->fix.accel			= FB_ACCEL_NONE;
}

/* probe 함수 */
static int hub75_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct hub75 *h;
	struct gpio_desc *clk;		// 예약+출력 초기화용 (런타임엔 레지스터로 구동)
	struct gpio_descs *data;	// 위와 동일 - devm 이라 지역변수여도 예약은 유지됨
	int ret, i;

	/* hub75 구조체 메모리 할당*/
	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	/* 매트릭스 속성 가져오기 */
	if (of_property_read_u32(np, "matrix-rows", &h->rows) ||
		of_property_read_u32(np, "matrix-cols", &h->cols) ||
		of_property_read_u32(np, "scan", &h->scan)) {
			dev_err(dev, "matrix-rows/cols/scan 속성 없음\n");
			return -EINVAL; // probe 실패
	}

	/* gpio 획득 */
	clk    = devm_gpiod_get(dev, "clk", GPIOD_OUT_LOW);
	h->lat = devm_gpiod_get(dev, "lat", GPIOD_OUT_LOW);
	h->oe  = devm_gpiod_get(dev, "oe" , GPIOD_OUT_LOW);

	h->addr = devm_gpiod_get_array(dev, "addr", GPIOD_OUT_LOW);
	data    = devm_gpiod_get_array(dev, "data", GPIOD_OUT_LOW);

	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "clk gpio\n");
	if (IS_ERR(h->lat))
		return dev_err_probe(dev, PTR_ERR(h->lat), "lat gpio\n");
	if (IS_ERR(h->oe))
		return dev_err_probe(dev, PTR_ERR(h->oe), "oe gpio\n");
	if (IS_ERR(h->addr))
		return dev_err_probe(dev, PTR_ERR(h->addr), "addr gpios\n");
	if (IS_ERR(data))
		return dev_err_probe(dev, PTR_ERR(data), "data gpios\n");

	if (h->addr->ndescs != 4 || data->ndescs != 6) {
		dev_err(dev, "addr 4개, data 6개가 필요\n");
		return -EINVAL;
	}

	/* 크기 관계 검증 - 리프레시 인덱싱이 기대는 전제들 */
	if (h->rows != 2 * h->scan || h->scan > (1u << h->addr->ndescs) || h->cols < 2) {
		dev_err(dev, "잘못된 크기: rows=%u cols=%u scan=%u\n",
			h->rows, h->cols, h->scan);
		return -EINVAL;
	}

	/* GPIO 레지스터 매핑 (devm_ 이라 자동 해제) */
	h->gpio_regs = devm_ioremap(dev, GPIO_BASE, GPIO_LEN);
	if (!h->gpio_regs)
		return -ENOMEM;

	h->clk_mask = BIT(CLK_PIN);
	for (i = 0; i < 6; ++i) {
		h->data_mask[i] = BIT(data_pin[i]); // GPIO 11이면 BIT(11) == (1 << 11)
		h->data_mask_all |= h->data_mask[i];		// 전체 마스크 (clr 여집합용)
	}

	/* PWM/클럭 레지스터 매핑 + OE 를 하드웨어 펄스로 전환 */
	h->pwm_regs = devm_ioremap(dev, PWM_BASE, PWM_LEN);
	h->cm_regs  = devm_ioremap(dev, CM_BASE, CM_LEN);
	if (!h->pwm_regs || !h->cm_regs)
		return -ENOMEM;
	hub75_pwm_init(h);

	/* 프레임버퍼 설정 */
	h->fb_size = h->rows * h->cols * 3;
	h->fb = vmalloc_user(h->fb_size);	// 페이지 정렬 + 0으로 초기화 + mmap 가능
	if (!h->fb) {
		ret = -ENOMEM;
		goto err_pwm;	// 여기부터는 OE 가 ALT0(PWM) 이라 하드웨어 복원이 필요
	}
	hub75_fill_test(h);
	hub75_update_lut(h);	// 첫 프레임부터 유효한 조회표로

	mutex_init(&h->write_lock);
	init_waitqueue_head(&h->vsync_wait);	// vsync 대기큐 준비

	/* /dev/hub75 문자 장치 등록 */
	h->misc.minor = MISC_DYNAMIC_MINOR;	// 마이너 번호 자동 배정
	h->misc.name  = "hub75";
	h->misc.fops  = &hub75_fops;
	ret = misc_register(&h->misc);
	if (ret)
		goto err_fb;

	platform_set_drvdata(pdev, h); // probe → remove 에서 h 되찾으려고 저장
	h->thread = kthread_create(hub75_refresh, h, "hub75_refresh");  // 만들기 (잠든 상태)
	if (IS_ERR(h->thread)) {
		ret = PTR_ERR(h->thread);
		goto err_misc;
	}

	// 지정한 코어가 존재하고 켜져 있으면 binding
	if (refresh_cpu >= 0 && refresh_cpu < nr_cpu_ids && cpu_online(refresh_cpu))
		kthread_bind(h->thread, refresh_cpu);

	wake_up_process(h->thread);		// 깨우기

	/* fbdev 등록 - misc 와 같은 h->fb 를 공유하는 표준 /dev/fbN */
	h->info = framebuffer_alloc(0, dev);	// par 크기 0 (팔레트는 struct 안)
	if (!h->info) {
		ret = -ENOMEM;
		goto err_thread;
	}
	hub75_fb_setup(h);
	ret = register_framebuffer(h->info);
	if (ret)
		goto err_fbinfo;

	dev_info(dev, "패널 %ux%u, 1/%u 스캔, /dev/fb%d\n",
		 h->cols, h->rows, h->scan, h->info->node);
	return 0;

err_fbinfo:
	framebuffer_release(h->info);
err_thread:
	kthread_stop(h->thread);
err_misc:
	misc_deregister(&h->misc);
err_fb:
	vfree(h->fb);
err_pwm:
	hub75_pwm_off(h);	// OE 를 ALT0 에 남기고 나가면 핀·클럭이 주인 없이 뜬다
	return ret;
}

/* 소등 절차 - remove(rmmod)와 shutdown(재부팅/전원끔)이 공용으로 사용 */
static void hub75_quiesce(struct hub75 *h)
{
	kthread_stop(h->thread);	// 스레드 정지 (fb 읽기 중단)
	hub75_pwm_off(h);			// OE 소등 복원 + PWM/클럭 정지
}

/* remove 함수 - rmmod 때 (소등 + 정리) */
static int hub75_remove(struct platform_device *pdev)
{
	struct hub75 *h = platform_get_drvdata(pdev);

	unregister_framebuffer(h->info);	// fbdev 접근 차단 (먼저)
	hub75_quiesce(h);					// 스레드 정지 + 하드웨어 소등
	misc_deregister(&h->misc);			// misc 접근 차단

	framebuffer_release(h->info);		// fb_info 해제
	vfree(h->fb);						// 버퍼 해제 (아무도 안 읽음)

	dev_info(&pdev->dev, "removed\n");
	return 0;
}

/* shutdown 훅 - 시스템 재부팅/전원끔 때 (정리는 생략, 소등만) */
static void hub75_shutdown(struct platform_device *pdev)
{
	hub75_quiesce(platform_get_drvdata(pdev));
}


/* DTS 의 compatible 과 글자까지 같아야 매칭됨 */
static const struct of_device_id hub75_of_match[] = {
	{ .compatible = "smg,hub75-matrix" },
	{ }
};
MODULE_DEVICE_TABLE(of, hub75_of_match);

/* 드라이버 명함 - 커널이 이걸 보고 probe/remove/shutdown 을 부른다 */
static struct platform_driver hub75_driver = {
	.probe    = hub75_probe,
	.remove   = hub75_remove,
	.shutdown = hub75_shutdown,	// reboot/poweroff 때 소등 보장
	.driver = {
		.name 			= "hub75",
		.of_match_table = hub75_of_match,
	},
};
module_platform_driver(hub75_driver);	// insmod/rmmod 등록 코드 자동 생성

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HUB75 LED Matrix driver");
MODULE_AUTHOR("Yehoon");
