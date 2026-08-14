-- ============================================================
--  일일 리포트 — 시연/개발용 더미 데이터
--
--    mysql --skip-ssl -h <DB> -u daboijo -p daboijo < core/sql/seed_demo.sql
--
--  ★ 이 파일은 실제 센서 없이 리포트 화면을 완성하기 위한 것이다.
--    웨어러블·카메라가 없어도 그래프·PDF·AI 요약을 만들고 검증할 수 있고,
--    시연 당일 장비가 안 붙어도 리포트는 돌아간다.
--
--  ★ 모든 데이터는 2026-08-13 하루치다. 맨 아래 정리 구문으로 통째로 지울 수 있다.
--    실제 서버가 쌓은 데이터와 섞이지 않게 날짜를 한 곳으로 몰아둔 것이다.
--
--  ★ 대상은 "지금 재원 중인 앞의 두 명"을 DB 에서 찾아 쓴다. 입소자를 다시
--    배정하거나 퇴원시켜도 이 파일을 그대로 다시 돌리면 현재 명단에 맞춰진다.
--    (id 를 박아두면 퇴원한 사람에게 데이터가 들어가 리포트 탭에서 안 보인다)
--
--    첫 번째 사람 = 조용한 쪽 : 많이 누워 있음 · 케어 많이 받음 · 낙상 1건
--    두 번째 사람 = 활발한 쪽 : 활동 많음 · 밤에 자주 일어남 · 이탈 2건
-- ============================================================
USE daboijo;

SET @day = DATE('2026-08-13');

-- 대상 두 명 (재원자 중 id 순으로 앞의 둘)
SET @r1 = (SELECT resident_id FROM residents WHERE status='재원' ORDER BY resident_id LIMIT 1);
SET @r2 = (SELECT resident_id FROM residents WHERE status='재원' ORDER BY resident_id LIMIT 1 OFFSET 1);
SET @c1 = (SELECT camera_id FROM residents WHERE resident_id = @r1);
SET @c2 = (SELECT camera_id FROM residents WHERE resident_id = @r2);

SELECT CONCAT('시드 대상 → ',
       (SELECT name FROM residents WHERE resident_id=@r1), '(id ', @r1, ', cam ', @c1, ') · ',
       (SELECT name FROM residents WHERE resident_id=@r2), '(id ', @r2, ', cam ', @c2, ')') AS '';

-- ── 먼저 정리 (여러 번 돌려도 중복이 안 쌓이게) ─────────────
DELETE FROM activity_minute WHERE DATE(minute_ts) = @day;
DELETE FROM events          WHERE DATE(occurred_at) = @day;
DELETE FROM care_logs       WHERE DATE(start_time) = @day;
DELETE FROM bed_sessions    WHERE in_at >= @day - INTERVAL 1 DAY
                              AND in_at <  @day + INTERVAL 2 DAY;


-- ── ① 활동량 + 생체데이터 (웨어러블) ───────────────────────
-- 하루 1440분을 사람마다 한 줄씩. 실제 서버(ActivityModule)가 1분 버킷으로
-- 쌓는 것과 같은 모양이다. 걸음·심박·SpO2 가 한 행에 같이 들어간다.
--
-- ★ 걸음을 시간마다 고르게 뿌리지 않는다. 사람은 몰아서 걷고 대부분은 앉아 있다.
--   그래서 "그 분에 걸었을 확률"과 "걸었다면 몇 걸음(30~70)"으로 나눠 만든다.
--   고르게 뿌리면 매분 3~8걸음이 되어 '활동한 분' 지표가 0이 되어버린다.
--
-- ★ 심박은 걸음과 연동시킨다. 걸은 분은 활동 심박(85~110), 안 걸었지만 깨어
--   있으면 안정 심박(68~82), 자는 동안은 서맥에 가깝게(52~64). 이렇게 해야
--   그래프에서 걸음 막대와 심박 선이 같이 오르내려 그럴듯해 보인다.
--   그냥 랜덤으로 뿌리면 톱니 모양이 되어 한눈에 가짜인 게 보인다.
INSERT INTO activity_minute (resident_id, minute_ts, steps_delta, steps_cum, hr_avg, spo2_avg)
SELECT rid, minute_ts, d,
       SUM(d) OVER (PARTITION BY rid ORDER BY minute_ts ROWS UNBOUNDED PRECEDING),
       -- 걸었으면 활동 심박, 안 걸었으면 시간대에 따라 안정/수면 심박
       CASE WHEN d > 0 THEN 85 + FLOOR(RAND() * 25)
            WHEN asleep = 1 THEN 52 + FLOOR(RAND() * 12)
            ELSE 68 + FLOOR(RAND() * 14) END,
       -- 자는 동안 SpO2 가 살짝 내려가는 것도 실제와 비슷하게
       CASE WHEN asleep = 1 THEN 93 + FLOOR(RAND() * 5)
            ELSE 95 + FLOOR(RAND() * 5) END
FROM (
    SELECT p.rid,
           TIMESTAMP(@day) + INTERVAL s.seq MINUTE AS minute_ts,
           CASE WHEN s.seq DIV 60 < 6 OR s.seq DIV 60 >= 21
                  OR s.seq DIV 60 BETWEEN 12 AND 13 THEN 1 ELSE 0 END AS asleep,
           CASE WHEN RAND() < (CASE
                    WHEN s.seq DIV 60 <  6 THEN 0.00   -- 수면
                    WHEN s.seq DIV 60 <  8 THEN 0.10   -- 기상·세면
                    WHEN s.seq DIV 60 =  8 THEN 0.25   -- 아침 식사
                    WHEN s.seq DIV 60 < 11 THEN 0.12   -- 오전
                    WHEN s.seq DIV 60 = 11 THEN 0.22   -- 점심 이동
                    WHEN s.seq DIV 60 < 14 THEN 0.00   -- 식후 낮잠
                    WHEN s.seq DIV 60 < 17 THEN 0.20   -- 오후 활동·산책
                    WHEN s.seq DIV 60 = 17 THEN 0.25   -- 저녁 식사
                    WHEN s.seq DIV 60 < 21 THEN 0.08   -- 저녁 휴식
                    ELSE 0.00                          -- 취침
                END) * p.mult
                THEN 30 + FLOOR(RAND() * 40)
                ELSE 0 END AS d
    FROM seq_0_to_1439 s
    CROSS JOIN (SELECT @r1 AS rid, 0.4 AS mult UNION ALL   -- 조용한 쪽
                SELECT @r2,        1.0) p                  -- 활발한 쪽
) t;


-- ── ② 침대 재실 세션 (누워있는 시간) ───────────────────────
-- ★ 밤 취침이 자정을 넘는 것이 이 데이터의 핵심이다. 리포트가 날짜 구간과의
--   교집합을 제대로 자르는지 이걸로 검증한다. "시작한 날"로 몰아 세면
--   8/13 이 8시간이 아니라 엉뚱한 값이 나온다.
--
--   @r1 의 8/13 기대값: (00:00~06:45) 405분 + (13:00~14:30) 90분
--                      + (21:50~24:00) 130분 = 625분 = 10시간 25분
INSERT INTO bed_sessions (camera_id, resident_id, in_at, out_at, duration_sec) VALUES
  -- 조용한 쪽 — 전날 밤부터 이어짐(자정 걸침) + 낮잠
  (@c1, @r1, '2026-08-12 22:30:00', '2026-08-13 06:45:00', 29700),
  (@c1, @r1, '2026-08-13 13:00:00', '2026-08-13 14:30:00',  5400),
  (@c1, @r1, '2026-08-13 21:50:00', '2026-08-14 07:00:00', 33000),
  -- 활발한 쪽 — 밤중에 두 번 일어남(아래 침상이탈 이벤트와 시각이 맞는다)
  (@c2, @r2, '2026-08-12 23:10:00', '2026-08-13 05:40:00', 23400),
  (@c2, @r2, '2026-08-13 06:05:00', '2026-08-13 07:20:00',  4500),
  (@c2, @r2, '2026-08-13 12:40:00', '2026-08-13 13:30:00',  3000),
  (@c2, @r2, '2026-08-13 22:15:00', '2026-08-14 06:30:00', 29700);


-- ── ③ 이벤트 ───────────────────────────────────────────────
-- 종류·발생원·확인여부를 섞어서 리포트의 내역 표시를 전부 확인할 수 있게 한다.
INSERT INTO events (occurred_at, camera_id, resident_id, event_type, source, clip_url, confirmed_at, confirmed_by) VALUES
  -- 조용한 쪽: 새벽 낙상 (카메라) — 관제사가 확인 처리함
  ('2026-08-13 03:12:07.420', @c1, @r1, 'FALL',           'CAMERA',
   CONCAT('ch', @c1, '_1786716727420_FALL.mp4'), '2026-08-13 03:14:30', '박민용'),
  -- 조용한 쪽: 오후 생체신호 이상 (웨어러블) — 미확인
  ('2026-08-13 15:47:11.000', @c1, @r1, 'VITAL_ABNORMAL', 'WEARABLE',
   CONCAT('ch', @c1, '_1786761431000_VITAL_ABNORMAL.mp4'), NULL, NULL),
  -- 활발한 쪽: 침상이탈 2건 — 위 재실 세션이 끊긴 시각과 일치시킨다
  ('2026-08-13 05:40:03.100', @c2, @r2, 'EGRESS',         'CAMERA',
   CONCAT('ch', @c2, '_1786725603100_EGRESS.mp4'), '2026-08-13 05:43:00', '박민용'),
  ('2026-08-13 12:40:00.000', @c2, @r2, 'EGRESS',         'CAMERA',
   CONCAT('ch', @c2, '_1786750800000_EGRESS.mp4'), NULL, NULL);


-- ── ④ 케어로그 ─────────────────────────────────────────────
-- ★ 두 사람이 같은 방(같은 camera_id)이면 요양사가 한 번 들어올 때 두 사람 모두에게
--   같은 시간이 기록된다 — 서버의 insertCareLogs 가 하는 일과 같은 모양이다.
--   그래서 회진은 start_time 을 똑같이 맞춰 두 행씩 넣는다.
--   낙상 대응 케어만 한 사람 것으로 둔다(그 사람에게만 일어난 일이므로).
INSERT INTO care_logs (camera_id, resident_id, caregiver, start_time, end_time, duration_sec) VALUES
  -- 새벽 낙상 대응 — 낙상 당사자에게만
  (@c1, @r1, NULL, '2026-08-13 03:13:40', '2026-08-13 03:22:10', 510),
  -- 이탈 확인 — 이탈 당사자에게만
  (@c2, @r2, NULL, '2026-08-13 05:42:00', '2026-08-13 05:47:30', 330),
  -- 아침 회진 — 같은 방이라 두 사람 모두
  (@c1, @r1, NULL, '2026-08-13 08:05:00', '2026-08-13 08:09:20', 260),
  (@c2, @r2, NULL, '2026-08-13 08:05:00', '2026-08-13 08:09:20', 260),
  -- 오후 회진 — 두 사람 모두
  (@c1, @r1, NULL, '2026-08-13 14:31:00', '2026-08-13 14:34:15', 195),
  (@c2, @r2, NULL, '2026-08-13 14:31:00', '2026-08-13 14:34:15', 195),
  -- 저녁 회진 — 두 사람 모두
  (@c1, @r1, NULL, '2026-08-13 20:10:00', '2026-08-13 20:16:40', 400),
  (@c2, @r2, NULL, '2026-08-13 20:10:00', '2026-08-13 20:16:40', 400);


-- ============================================================
--  확인 — Qt 리포트 화면과 같은 값이 나와야 한다
-- ============================================================
SELECT r.name AS 이름,
       (SELECT COALESCE(SUM(steps_delta),0) FROM activity_minute
         WHERE resident_id=r.resident_id AND DATE(minute_ts)=@day)              AS 걸음,
       (SELECT COALESCE(SUM(steps_delta>=10),0) FROM activity_minute
         WHERE resident_id=r.resident_id AND DATE(minute_ts)=@day)              AS 활동분,
       (SELECT CONCAT(ROUND(AVG(hr_avg)),' bpm') FROM activity_minute
         WHERE resident_id=r.resident_id AND DATE(minute_ts)=@day)              AS 평균심박,
       (SELECT COALESCE(SUM(TIMESTAMPDIFF(SECOND,
                 GREATEST(in_at, TIMESTAMP(@day)),
                 LEAST(COALESCE(out_at,NOW()), TIMESTAMP(@day)+INTERVAL 1 DAY))),0) DIV 60
          FROM bed_sessions WHERE resident_id=r.resident_id
           AND in_at < TIMESTAMP(@day)+INTERVAL 1 DAY
           AND COALESCE(out_at,NOW()) > TIMESTAMP(@day))                        AS 누운분,
       (SELECT COALESCE(SUM(duration_sec),0) FROM care_logs
         WHERE resident_id=r.resident_id AND DATE(start_time)=@day)             AS 케어초,
       (SELECT COUNT(*) FROM events
         WHERE resident_id=r.resident_id AND DATE(occurred_at)=@day)            AS 이벤트
FROM residents r WHERE r.resident_id IN (@r1, @r2) ORDER BY r.resident_id;


-- ============================================================
--  ⚠️ 지울 때 (실데이터가 쌓이기 시작하면 반드시 지울 것)
-- ============================================================
-- DELETE FROM activity_minute WHERE DATE(minute_ts)   = '2026-08-13';
-- DELETE FROM events          WHERE DATE(occurred_at) = '2026-08-13';
-- DELETE FROM care_logs       WHERE DATE(start_time)  = '2026-08-13';
-- DELETE FROM bed_sessions    WHERE in_at >= '2026-08-12' AND in_at < '2026-08-15';
