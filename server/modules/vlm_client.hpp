#pragma once

#include <string>
#include <vector>

// [케어 봇] 비전-언어 모델(VLM) 추상 인터페이스.
//
// 구현체를 갈아 끼우면 백엔드(Gemini / Claude / …)를 바꿀 수 있다. 봇 로직은
// 이 인터페이스에만 의존하므로 gemini_client.* 외의 파일은 손댈 일이 없다.
// 실패 사유. "빈 문자열" 하나로는 사용자에게 할 말을 고를 수 없어서 나눴다 —
// 쿼터 소진은 잠시 후 다시 눌러도 안 되지만(오늘 하루 끝), 타임아웃은 다시
// 누르면 될 수도 있다. 둘을 같은 문면으로 뭉개면 사용자가 헛되이 반복한다.
enum class VlmError {
    None,           // 성공
    NotConfigured,  // 키 미설정
    Network,        // 연결·전송 실패
    Timeout,        // 예산 초과
    Quota,          // HTTP 429 — 무료 티어 한도 소진(RPM/RPD)
    Rejected,       // 그 외 HTTP 오류(400 등) — 요청 형식·모델명 문제
    Empty,          // HTTP 200인데 본문에 텍스트가 없음(안전 필터 등)
};

class VlmClient {
public:
    using Jpeg = std::vector<unsigned char>;

    virtual ~VlmClient() = default;

    // 이미지 여러 장(촬영 순서)과 질문을 주면 한국어 상황 설명을 반환한다.
    // 실패 시 빈 문자열 — 호출자가 폴백. err 를 주면 사유가 채워진다.
    // ★ err 는 호출별 out-param 이다(멤버에 저장하지 않는다) — 버튼을 여러 명이
    //   동시에 눌러 여러 스레드가 같은 클라이언트를 쓰므로, 멤버에 두면 남의
    //   실패 사유를 내 사용자에게 보여주게 된다.
    virtual std::string describe(const std::vector<Jpeg>& frames,
                                 const std::string& question,
                                 VlmError* err = nullptr) = 0;

    // 사용 가능(키 설정됨) 여부. false면 호출자가 아예 VLM을 안 부르고 안내만.
    virtual bool available() const = 0;

    // 이미지 없이 텍스트만으로 질의(자연어 검색 질의 구조화 등 순수 언어 작업용).
    // 실패 시 빈 문자열 — 호출자가 폴백. err 는 describe()와 같다.
    virtual std::string ask(const std::string& prompt, VlmError* err = nullptr) = 0;
};
