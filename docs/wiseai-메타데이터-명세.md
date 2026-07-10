# WiseAI 메타데이터 명세 (카메라 → 서버, 낙상 알고리즘 입력)

- 작성: 박민용 (중앙 서버 파트) / 2026-07-09
- 대상 독자: 낙상 판정 알고리즘(core 룰엔진) 담당자
- 근거: PNM-C16083RVQ 실측 캡처 2건 (`server/tools/metadata_probe.py`로 덤프)
- 파싱 코드: [`server/video/metadata_parser.cpp`](../server/video/metadata_parser.cpp) → 결과 구조체 [`server/video/detection.hpp`](../server/video/detection.hpp)

## 1. 어디서 어떻게 오는가

카메라 RTSP 연결 하나에 **영상 트랙 + 메타데이터(data) 트랙**이 같이 실려온다.
메타데이터 트랙의 페이로드는 ONVIF `MetadataStream` XML 문서의 연속이다.

- 서버의 `RtspAvClient`가 data 패킷을 받아 `MetadataParser::parse()`로 파싱
- 파싱 결과 `std::vector<Detection>`이 **감지 콜백**으로 전달됨 (채널 번호와 함께)
- **주기: 약 5회/초** (200ms 간격, 실측). 화면에 객체가 없으면 객체 프레임은 안 옴
- 원본을 직접 보려면: `python3 server/tools/metadata_probe.py <채널> <초>`

## 2. 메타데이터는 두 종류다

### 2-1. 객체 감지 프레임 (`tt:VideoAnalytics`) — 알고리즘의 주 입력

실제 수신 예 (주석 추가):

```xml
<tt:Frame UtcTime="2026-07-09T01:20:23.066Z">        <!-- 카메라 시각, ms 단위 -->
  <tt:Transformation>                                 <!-- 픽셀→정규화 계수 (매 프레임 포함) -->
    <tt:Translate x="-1.0" y="1.0"/>
    <tt:Scale x="0.000772" y="-0.001316"/>
  </tt:Transformation>

  <tt:Object ObjectId="210">                          <!-- 몸통. ObjectId = 추적 키 -->
    <tt:Appearance>
      <tt:Shape>
        <tt:BoundingBox left="238" top="206" right="607" bottom="644"/>  <!-- 센서 픽셀값 -->
        <tt:CenterOfGravity x="422.5" y="425.0"/>     <!-- 무게중심 (픽셀값) -->
      </tt:Shape>
      <tt:Class>
        <tt:ClassCandidate>                           <!-- 후보 분류 (참고용) -->
          <tt:Type>Human</tt:Type><tt:Likelihood>0.79</tt:Likelihood>
        </tt:ClassCandidate>
        <tt:Type Likelihood="0.79">Human</tt:Type>    <!-- 최종 분류 ← 파서가 쓰는 것 -->
      </tt:Class>
    </tt:Appearance>
  </tt:Object>

  <tt:Object ObjectId="209" Parent="210">             <!-- 같은 사람의 '머리'가 별도 객체 -->
    ... <tt:Type Likelihood="0.87">Head</tt:Type>
  </tt:Object>
</tt:Frame>
```

- 분류(type)는 실측에서 **Human / Head / Other** 세 가지 관측됨
- Head는 `Parent` 속성으로 자기가 속한 Human의 ObjectId를 가리킴
- Likelihood는 0~1. **화면 가장자리에 몸이 잘리면 0.3 수준까지 떨어짐** (실측: 왼쪽 끝에 선 사람 = 0.31)

### 2-2. 이벤트 알림 (`tt:Event`) — 상태가 바뀔 때만

| 토픽 | 의미 | 활용 |
| --- | --- | --- |
| `OpenApp/WiseAI/ObjectDetection` | 화면에 사람 있음/없음 (`State`=true/false, `ClassTypes`="Person") | 재실(occupancy) 판단 |
| `VideoAnalytics/MotionDetection`, `VideoSource/MotionAlarm` | 모션 감지 | 보조 |
| `AudioSource/AudioDetection` | 소리 감지 | 낙상 소리 = 교차검증 보조 신호 후보 |
| `VideoSource/ImageTooBlurry` | 화면 흐림 (렌즈 가림) | 카메라 이상 감지 |
| `Device/DigitalInput`, `Device/Relay` | 알람 입출력 단자 | 미사용 |

**안 오는 것**: 쓰러짐 이벤트(이 펌웨어엔 없음 — 그래서 우리가 만든다), 자세/스켈레톤, 얼굴 인식 결과.

## 3. 좌표계 — 파서가 정규화해서 준다

XML의 좌표는 센서 픽셀값이고, 파서가 `Transformation`을 적용해 **0.0~1.0 정규화 좌표**로 변환한다.
알고리즘 쪽은 정규화된 값만 보면 된다:

- `(0, 0)` = 화면 **좌상단**, `(1, 1)` = 화면 **우하단**
- 즉 **cy가 커질수록 화면 아래쪽** → 넘어지면 cy 증가

변환식(참고): ONVIF 좌표 `onvif = pixel × scale + translate`는 [-1,1]이고 **위가 +1**이다.
파서는 x는 `(onvif+1)/2`, y는 `(1−onvif)/2`로 뒤집어 화면 좌표(0=상단)로 만든다.
(2026-07-10 수정: 초기 구현이 y를 안 뒤집어 cy가 상하 반전돼 있었다 — 낙상 테스트 전탐지 실패의 원인.)

## 4. 알고리즘이 받는 실제 입력: `Detection`

```cpp
struct Detection {
    int object_id;    // 추적 키 (프레임 간 동일 객체)
    int parent_id;    // Head → 소속 Human의 id (없으면 0)
    float left, top, right, bottom;  // bbox, 정규화 0~1
    float cx, cy;     // 무게중심, 정규화 0~1 — 프레임 간 cy 변화율 = 낙하 속도
    float likelihood; // 분류 확신도 0~1
    std::string type; // "Human" | "Head" | "Other"

    float width(), height();
    float aspectRatio();  // 가로/세로. 서 있으면 <1, 누우면 >1
    bool isHuman();
};
```

메타데이터 주기가 ~5fps이므로, 연속 두 프레임의 시간차는 보통 0.2초.
프레임 시각은 콜백 수신 시각(steady_clock) 기준으로 계산하면 된다.

## 5. 주의사항 (실측으로 확인된 함정들)

1. **추적 ID가 바뀐다.** 감지가 몇 초 끊겼다 재개되면 같은 사람이 새 ObjectId를 받는다
   (실측: 3.6초 소실 후 235 → 236). ID만 믿지 말고, 비슷한 위치에 재등장한
   새 ID는 같은 사람으로 잇는 로직이 필요하다.
2. **객체가 사라질 때 "요약 프레임"이 온다.** bbox가 전부 0인 마지막 프레임에
   옷 색상(`tt:HumanBody`)과 베스트샷 JPEG 다운로드 경로(`tt:ImageRef`,
   예: `/download/ch3/objectid_235_....jpg`)가 담긴다.
   → **bbox 넓이가 0인 Detection은 위치 계산에서 제외할 것** (type은 Human으로 옴).
   → ImageRef는 낙상 알림 팝업의 스냅샷으로 활용 가능 (카메라에서 HTTP 다운로드).
3. **바닥에 누운 사람은 감지가 끊길 수 있다.** "급하강 직후 Human 소실"은
   그 자체로 낙상 의심 신호로 다뤄야 한다.
4. **화면 가장자리에서는 likelihood가 낮다** (몸 잘림). likelihood 임계값을 너무
   높게 잡으면 가장자리 낙상을 놓친다.
5. 분할 수신된 XML 조각(Transformation 없음)은 파서가 통째로 버린다 —
   알고리즘 쪽에는 좌표가 오염된 데이터가 오지 않는다.

## 6. 낙상 판정에 쓸 신호 요약

| 신호 | 데이터 출처 | 판정 아이디어 |
| --- | --- | --- |
| 종횡비 반전 | `aspectRatio()` | <0.7(서있음) → >1.2(누움) 전환 |
| 낙하 속도 | `cy`의 프레임 간 변화율 | 짧은 시간(≤1초) 급하강 = 쓰러짐, 완만 = 그냥 눕기 |
| 침상 밖 여부 | bbox 위치 + 침상 ROI(video 파트 예정) | 요양원에선 침대 위 누움은 정상 — 오탐 방지 핵심 |
| 부동 지속 | bbox 변화 없음 N초 | 전환 후 안 움직이면 위험도 상승 |
| 머리 위치 | `type=="Head"`의 cy (+Parent로 몸통 연결) | 머리가 바닥 높이 = 강한 단서 |
| 추적 소실 | Human 객체 소실 타이밍 | 급하강 직후 소실 = 의심 신호 |
| 최종 판정 | + 웨어러블 바이탈 교차검증 (core) | 기획서의 다중 교차검증 |

임계값(0.7 / 1.2 / 1초 등)은 가안이며, 낙상 연기 캡처 데이터로 튜닝 예정.
