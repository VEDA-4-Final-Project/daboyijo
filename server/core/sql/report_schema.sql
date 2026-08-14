-- ============================================================
--  다보이조 — 일일 환자 리포트용 테이블
--
--  기존 schema.sql 에 이어서 실행한다(그쪽을 지우거나 고치지 않는다).
--    mysql -h <DB호스트> -u daboijo -p daboijo < core/sql/report_schema.sql
--
--  ★ 기록 주체는 전부 "서버"다. Qt 관제 앱은 읽기만 한다.
--    관제 PC 는 퇴근하면 꺼지는데, 낙상·침상이탈은 밤에 제일 많이 난다.
--    Qt 가 기록하면 리포트에서 밤 시간대가 통째로 빈다.
--    (예외: events.confirmed_at 은 관제사의 '확인' 행위라 Qt 가 UPDATE 한다)
--
--  ★ camera_id 는 0-based 다 (care_logs 와 동일). 화면 표기만 +1 해서 "채널 1".
-- ============================================================
USE daboijo;


-- ── ① 이벤트 원장 ──────────────────────────────────────────
-- 지금까지 낙상/침상이탈/생체이상은 Qt 의 QTableWidget 에만 쌓였다.
-- 그래서 앱을 껐다 켜면 기록이 통째로 날아갔고, 지난 날짜는 조회 자체가 불가능했다.
-- 이 테이블이 그 원장(元帳)이다.
--
-- resident_id 를 굳이 따로 두는 이유: camera_id 로 매번 조인하면, 입소자가 다른
-- 방으로 옮긴 뒤에 과거 이벤트가 새 입주자 것으로 바뀐다. 발생 시점의 사람을
-- 그대로 박아둔다(스냅샷).
CREATE TABLE IF NOT EXISTS events (
    event_id     BIGINT AUTO_INCREMENT PRIMARY KEY,
    -- (3) = 밀리초. 서버/Qt 가 주고받는 timestamp 가 ms 단위라 맞춰둔다.
    occurred_at  DATETIME(3)  NOT NULL,
    camera_id    INT,
    resident_id  INT,
    -- 값을 늘릴 땐 ALTER TABLE events MODIFY event_type ENUM(...,'새값') NOT NULL;
    -- VARCHAR 대신 ENUM 인 이유: 서버가 SQL 을 문자열로 조립하는데(database.cpp),
    -- 오타가 나면 VARCHAR 는 조용히 들어가고 ENUM 은 에러가 난다.
    event_type   ENUM('FALL','EGRESS','VITAL_ABNORMAL') NOT NULL,
    -- 같은 낙상이라도 카메라가 본 것과 웨어러블이 본 것이 따로 올라온다.
    -- 리포트에서 한 사건을 두 번 세지 않으려면 이 구분이 필요하다.
    source       ENUM('CAMERA','WEARABLE') NOT NULL,
    -- 블랙박스 클립 주소. 규칙: http://<host>:<port>/ch{N}_{ms}_{FALL|EGRESS|VITAL_ABNORMAL}.mp4
    clip_url     VARCHAR(255),
    confirmed_at DATETIME     NULL,        -- NULL = 미확인
    confirmed_by VARCHAR(50)  NULL,        -- 확인한 관제사(users.name)
    created_at   TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,

    -- 리포트의 주 조회 패턴: "이 사람의 이 날짜" → 이 순서가 맞다.
    KEY idx_resident_day (resident_id, occurred_at),
    -- 이벤트 기록 탭의 전체 목록(최신순) + 캘린더 점 찍기용.
    KEY idx_occurred (occurred_at),
    CONSTRAINT fk_events_resident FOREIGN KEY (resident_id)
        REFERENCES residents(resident_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- ── ② 침대 재실 세션 ───────────────────────────────────────
-- "누워있는 시간" 의 원천. 침대 ROI 안으로 들어온 순간 행을 열고(out_at NULL),
-- 벗어난 순간 닫는다.
--
-- 지금 BedEgressModule 은 안→밖 전이만 잡아 알람을 쏘고 끝이다. 밖→안 전이도
-- 잡아서 여기에 행을 열어줘야 한다(서버 코드 변경 필요).
--
-- ★ 자정 걸침: 케어시간처럼 "시작한 날" 로 몰면 안 된다. 23시 취침 → 익일 7시
--   기상은 어제 1시간 + 오늘 7시간이다. 리포트 쿼리에서 날짜 구간과의
--   교집합으로 자른다. 아래 예시 쿼리 참고.
CREATE TABLE IF NOT EXISTS bed_sessions (
    session_id   BIGINT AUTO_INCREMENT PRIMARY KEY,
    camera_id    INT      NOT NULL,
    resident_id  INT,
    in_at        DATETIME NOT NULL,
    out_at       DATETIME NULL,            -- NULL = 지금 누워 있는 중
    -- out_at 이 정해질 때 같이 채운다. out_at - in_at 로 매번 계산해도 되지만,
    -- 리포트가 매번 그걸 하는 것보다 한 번 써두는 편이 낫다.
    duration_sec INT      NULL,
    created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,

    KEY idx_resident_day (resident_id, in_at),
    -- "이 채널에 열려 있는 세션" 을 찾을 때 쓴다(닫으러 갈 때마다 조회).
    KEY idx_open (camera_id, out_at),
    CONSTRAINT fk_bed_resident FOREIGN KEY (resident_id)
        REFERENCES residents(resident_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- ── ③ 웨어러블 분당 샘플 ───────────────────────────────────
-- "활동시간(만보기)" 의 원천.
--
-- ★ 원본 패킷을 그대로 넣지 말 것. 릴레이가 초 단위로 발행해서 4명이면 하루
--   34만 행이 된다. 서버에서 1분 버킷으로 눌러 담으면 4명 × 1440 = 5760행/일.
--   INSERT ... ON DUPLICATE KEY UPDATE 로 같은 분에 덮어쓴다.
--
-- ★ steps 는 "누적값" 이다. 그대로 쓰면 안 된다.
--     - 웨어러블이 재부팅되면 0 으로 리셋된다
--     - 2바이트라 65535 에서 롤오버한다 (relay-node/src/main_relay.cpp)
--   그래서 steps_delta = 현재 - 직전 을 계산하되, 음수가 나오면 리셋으로 보고
--   0 으로 처리한다. 이걸 빼먹으면 어느 날 활동량이 65000걸음으로 찍힌다.
--   steps_cum 은 그 판정이 틀렸을 때 되짚어보려고 원본을 같이 남기는 것이다.
CREATE TABLE IF NOT EXISTS activity_minute (
    resident_id  INT      NOT NULL,
    minute_ts    DATETIME NOT NULL,        -- 분 단위로 내린 시각 (초 = 00)
    steps_delta  INT      NOT NULL DEFAULT 0,  -- 이 1분 동안 늘어난 걸음
    steps_cum    INT      NOT NULL DEFAULT 0,  -- 웨어러블이 보낸 누적값 원본
    hr_avg       SMALLINT NULL,
    spo2_avg     SMALLINT NULL,

    -- 사람+분이 곧 식별자다. AUTO_INCREMENT 를 따로 두면 같은 분이 중복으로 쌓인다.
    PRIMARY KEY (resident_id, minute_ts),
    CONSTRAINT fk_activity_resident FOREIGN KEY (resident_id)
        REFERENCES residents(resident_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- ── ④ 일일 리포트 (LLM 요약 캐시) ──────────────────────────
-- 숫자는 여기 없다. 숫자는 위 세 테이블 + care_logs 에서 매번 집계한다.
-- 이 테이블은 "AI 가 쓴 문장" 을 붙잡아두는 곳이다.
--
-- 캐시하는 이유: 리포트는 요양 기록물이라 같은 날짜를 두 번 열었을 때 문장이
-- 달라지면 안 된다. 재생성은 사용자가 버튼을 눌렀을 때만.
--
-- metrics_json: 그 문장을 쓸 때 AI 에게 넘긴 수치 스냅샷. 나중에 "이 요약이
-- 왜 이렇게 나왔나" 를 되짚을 수 있어야 한다.
CREATE TABLE IF NOT EXISTS daily_reports (
    report_date  DATE NOT NULL,
    resident_id  INT  NOT NULL,
    metrics_json JSON     NULL,
    summary_text TEXT     NULL,
    model        VARCHAR(40) NULL,         -- 예: 'gemini-2.5-flash'
    generated_at DATETIME NULL,

    PRIMARY KEY (report_date, resident_id),
    CONSTRAINT fk_report_resident FOREIGN KEY (resident_id)
        REFERENCES residents(resident_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- ── ⑤ 기존 care_logs 보강 ──────────────────────────────────
-- 리포트는 "환자별 · 날짜별" 로 조회한다. 지금은 인덱스가 없어 풀스캔이다.
ALTER TABLE care_logs ADD INDEX IF NOT EXISTS idx_care_resident_day (resident_id, start_time);
ALTER TABLE care_logs ADD INDEX IF NOT EXISTS idx_care_start (start_time);


-- ============================================================
--  확인
-- ============================================================
SHOW TABLES;
