#pragma once

// RPi 시스템 부하 측정 — B안(서버 경유 중계) 부하 검증용.
// 판정 기준: CPU 70% 이하, 온도 70°C 이하, 4채널 모두 12fps 이상.
class SystemStats {
public:
    // 전체 CPU 사용률(%). 직전 호출 이후 구간의 평균. 첫 호출은 0 반환.
    double cpuPercent();

    // SoC 온도(°C). /sys/class/thermal 기반, 읽기 실패 시(비 RPi 환경) -1.
    static double socTemperature();

private:
    unsigned long long last_total_ = 0;
    unsigned long long last_idle_ = 0;
};
