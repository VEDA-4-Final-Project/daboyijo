-- ============================================================
--  다보이조 관제 시스템 — 로그인 계정 테이블
--  대상 DB: daboijo @ 172.20.35.202:3306
--
--  ※ 보통은 이 파일을 실행할 필요가 없다.
--    앱이 시작할 때 Auth::ensureSchema()가 같은 내용을 자동으로 만든다(auth.cpp).
--    DB 계정에 CREATE 권한이 없어 자동 생성이 실패할 때만 아래를 수동 실행한다:
--
--      mysql -h 172.20.35.202 -u daboijo -p daboijo < sql/users_schema.sql
--
--  정의를 고칠 때는 auth.cpp의 ensureSchema()도 같이 맞출 것.
-- ============================================================

CREATE TABLE IF NOT EXISTS users (
    user_id       INT AUTO_INCREMENT PRIMARY KEY,

    -- 로그인 자격 정보
    login_id      VARCHAR(50)  NOT NULL UNIQUE,
    pw_hash       CHAR(64)     NOT NULL COMMENT 'PBKDF2-HMAC-SHA256 파생키 32B를 hex로',
    salt          CHAR(32)     NOT NULL COMMENT '계정별 랜덤 salt 16B를 hex로',
    iterations    INT          NOT NULL DEFAULT 100000 COMMENT 'PBKDF2 반복 횟수',

    -- 표시/운영 정보
    name          VARCHAR(50)  NOT NULL COMMENT '화면 우상단에 표시할 이름',
    role          VARCHAR(20)  NOT NULL DEFAULT 'admin'
                  COMMENT '지금은 단일 권한. 역할 분리 시 admin/caregiver/viewer 등으로 사용',
    status        VARCHAR(10)  NOT NULL DEFAULT '재직'
                  COMMENT "'재직'이 아니면 비밀번호가 맞아도 로그인 차단",

    -- caregivers 테이블과의 연결 (선택). 요양사 계정을 만들 때 채운다.
    caregiver_id  INT          NULL,

    last_login_at DATETIME     NULL,
    created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- caregivers 테이블이 이미 있다면 아래 주석을 풀어 외래키를 걸어도 된다.
-- ALTER TABLE users
--   ADD CONSTRAINT fk_users_caregiver FOREIGN KEY (caregiver_id)
--   REFERENCES caregivers(caregiver_id) ON DELETE SET NULL;


-- ============================================================
--  초기 관리자 계정
--    아이디   : admin
--    비밀번호 : admin1234
--  ⚠ 시연/개발용 계정이다. 운영에 올리기 전에 반드시 비밀번호를 바꿀 것.
--    (아래 salt/pw_hash는 admin1234에 대해 미리 계산해둔 값이다)
-- ============================================================
INSERT INTO users (login_id, pw_hash, salt, iterations, name, role, status)
VALUES (
    'admin',
    'f2a40d05688b0243906f717932e3cff37c3a7d62076ea09a2a870b08197bbccb',
    '0bf951ac3427ec4a4414cd9f7b241afa',
    100000,
    '관리자',
    'admin',
    '재직'
)
ON DUPLICATE KEY UPDATE login_id = login_id;   -- 이미 있으면 건드리지 않음

SELECT user_id, login_id, name, role, status, created_at FROM users;
