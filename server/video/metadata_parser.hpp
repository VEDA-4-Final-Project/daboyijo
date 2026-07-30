#pragma once

#include <string>
#include <vector>

#include "detection.hpp"

// ONVIF MetadataStream XML에서 WiseAI 객체 감지 결과를 추출한다.
// 무거운 XML 라이브러리 대신, 우리가 쓰는 태그(tt:Object/BoundingBox/Class)만
// 문자열 스캔으로 뽑는 경량 파서. 카메라가 뱉는 실제 XML 구조 기준:
//
//   <tt:Frame UtcTime="...">
//     <tt:Transformation><tt:Scale x=".." y=".."/></tt:Transformation>
//     <tt:Object ObjectId="209" Parent="210">          ← Head는 Human을 Parent로 가리킴
//       <tt:Shape>
//         <tt:BoundingBox left="238" top="206" right="607" bottom="644"/>
//         <tt:CenterOfGravity x="488.0" y="341.0"/>
//       </tt:Shape>
//       <tt:Class>
//         <tt:ClassCandidate><tt:Type>Other</tt:Type><tt:Likelihood>0.87</tt:Likelihood></tt:ClassCandidate>
//         <tt:Type Likelihood="0.87">Head</tt:Type>     ← 최종 판정 (우선 사용, 후보는 폴백)
//       </tt:Class>
//     </tt:Object>
//   </tt:Frame>
//
// BoundingBox·CenterOfGravity 좌표는 센서 픽셀값이라, tt:Transformation의
// Scale/Translate로 0~1 정규화한다. Transformation이 없는 패킷(분할 수신 등)은
// 좌표를 해석할 수 없으므로 통째로 스킵한다.
class MetadataParser {
public:
    // 메타데이터 패킷(XML 조각) 하나를 파싱해 사람 객체 목록을 반환.
    // ObjectDetection 프레임이 없으면 빈 벡터.
    static std::vector<Detection> parse(const std::string& xml);
};
