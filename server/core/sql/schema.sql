CREATE DATABASE IF NOT EXISTS daboijo
    CHARACTER SET utf8mb4;
USE daboijo;

-- 입소자 (사람당 한 줄)
CREATE TABLE IF NOT EXISTS residents (
    resident_id  INT AUTO_INCREMENT PRIMARY KEY,
    name         VARCHAR(50)  NOT NULL,
    room         VARCHAR(20)  NOT NULL,
    bed          VARCHAR(20)  NOT NULL,
    camera_id    INT,
    created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 케어로그 (세션마다 한 줄, camera_id 기준으로 기록)
-- ★ duration_sec 는 실제로 요양사가 감지된 시간의 합이라 end_time - start_time 과
--    다를 수 있다. 요양사가 3분 안에 돌아오면 새 줄을 만들지 않고 이 줄의
--    duration_sec 에 더하면서 end_time 만 미는데, 그동안 자리를 비운 시간은
--    더하지 않기 때문이다. 케어시간을 구할 땐 반드시 duration_sec 를 쓸 것.
--    (하루 집계는 start_time 기준 — 자정을 걸친 케어는 시작한 날로 센다)
CREATE TABLE IF NOT EXISTS care_logs (
    log_id       INT AUTO_INCREMENT PRIMARY KEY,
    camera_id    INT NOT NULL,
    resident_id  INT,
    caregiver    VARCHAR(30),
    start_time   DATETIME NOT NULL,
    end_time     DATETIME NOT NULL,
    duration_sec INT NOT NULL,
    FOREIGN KEY (resident_id) REFERENCES residents(resident_id)
);

-- ROI 구역 (구역마다 한 줄)
CREATE TABLE IF NOT EXISTS roi_zones (
    id           INT AUTO_INCREMENT PRIMARY KEY,
    camera_id    INT          NOT NULL,
    roi_name     VARCHAR(30)  NOT NULL,
    roi_points   JSON         NOT NULL,
    created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
