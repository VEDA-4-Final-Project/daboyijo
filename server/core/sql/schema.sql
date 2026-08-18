CREATE DATABASE IF NOT EXISTS daboijo
    CHARACTER SET utf8mb4;
USE daboijo;

-- 입소자 (사람당 한 줄)
-- wearable_id 는 릴레이 노드가 보내는 device_id 문자열과 그대로 대응한다
-- (relay-node.conf 의 device_id, 예: "wearable_01"). 웨어러블 이벤트가 올라오면
-- 이 값으로 입소자를 찾아 camera_id / room 을 얻는다.
CREATE TABLE IF NOT EXISTS residents (
    resident_id  INT AUTO_INCREMENT PRIMARY KEY,
    name         VARCHAR(50)  NOT NULL,
    room         VARCHAR(20)  NOT NULL,
    bed          VARCHAR(20)  NOT NULL,
    camera_id    INT,
    wearable_id  VARCHAR(32),
    created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_wearable_id (wearable_id)
);

-- 이미 residents 가 만들어진 DB 를 위한 이관 구문.
-- MariaDB 는 ADD COLUMN IF NOT EXISTS 를 지원한다.
ALTER TABLE residents ADD COLUMN IF NOT EXISTS wearable_id VARCHAR(32);
ALTER TABLE residents ADD UNIQUE KEY IF NOT EXISTS uk_wearable_id (wearable_id);

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

-- 침대 ROI (침대마다 한 줄)
-- 한 채널(카메라 시야)에 침대가 여러 개이고 침대마다 입소자가 다르다.
-- roi_id 는 그 채널 안에서의 침대 번호(0~7)로, Qt 화면·프로토콜·서버 판정이
-- 모두 이 번호로 같은 침대를 가리킨다.
-- resident_id 가 "이 침대는 누구 자리"라는 매핑이고, 서버는 이걸 앵커로
-- 추적 객체에 사람 이름을 붙인다(core/identity_tracker.hpp).
CREATE TABLE IF NOT EXISTS roi_zones (
    id           INT AUTO_INCREMENT PRIMARY KEY,
    camera_id    INT          NOT NULL,
    roi_id       INT          NOT NULL DEFAULT 0,
    roi_name     VARCHAR(30)  NOT NULL DEFAULT '',
    roi_points   JSON         NOT NULL,
    resident_id  INT          NULL,
    updated_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_camera_roi (camera_id, roi_id)
);

-- 이미 roi_zones 가 만들어진 DB 를 위한 이관 구문 (채널당 1개였던 시절 스키마).
ALTER TABLE roi_zones ADD COLUMN IF NOT EXISTS roi_id INT NOT NULL DEFAULT 0;
ALTER TABLE roi_zones ADD COLUMN IF NOT EXISTS resident_id INT NULL;
ALTER TABLE roi_zones ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP
    DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP;
ALTER TABLE roi_zones MODIFY COLUMN roi_name VARCHAR(30) NOT NULL DEFAULT '';
ALTER TABLE roi_zones ADD UNIQUE KEY IF NOT EXISTS uk_camera_roi (camera_id, roi_id);
