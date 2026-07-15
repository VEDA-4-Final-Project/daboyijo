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
