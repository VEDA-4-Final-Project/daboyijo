#ifndef _HUB75_IOCTL_H
#define _HUB75_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>    // 커널 쪽 _IO* 매크로
#else
#include <sys/ioctl.h>      // 유저 쪽 _IO* 매크로
#endif

#define HUB75_IOC_MAGIC 'H'

/* 화면 지우기 - 인자 없는 순수 명령 */
#define HUB75_CLEAR             _IO (HUB75_IOC_MAGIC, 0)

/* 밝기 0~255 - set(유저→커널) / get(커널→유저) */
#define HUB75_SET_BRIGHTNESS    _IOW(HUB75_IOC_MAGIC, 1, int)
#define HUB75_GET_BRIGHTNESS    _IOR(HUB75_IOC_MAGIC, 2, int)

/* 감마 0/1 - set / get */
#define HUB75_SET_GAMMA         _IOW(HUB75_IOC_MAGIC, 3, int)
#define HUB75_GET_GAMMA         _IOR(HUB75_IOC_MAGIC, 4, int)

/* 프레임 경계까지 대기 - 인자 없음 (부드러운 애니메이션용) */
#define HUB75_WAIT_VSYNC        _IO (HUB75_IOC_MAGIC, 5)

#endif
