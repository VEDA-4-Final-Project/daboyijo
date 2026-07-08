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
//     <tt:Object ObjectId="210">
//       <tt:Shape>
//         <tt:BoundingBox left="238" top="206" right="607" bottom="644"/>
//       </tt:Shape>
//       <tt:Class><tt:Type Likelihood="0.79">Human</tt:Type></tt:Class>
//     </tt:Object>
//   </tt:Frame>
//
// BoundingBox 좌표는 센서 픽셀값이라, tt:Transformation의 Scale/Translate로
// 0~1 정규화한다. Scale이 없으면 원점 좌표로 간주하고 스킵.
class MetadataParser {
public:
    // 메타데이터 패킷(XML 조각) 하나를 파싱해 사람 객체 목록을 반환.
    // ObjectDetection 프레임이 없으면 빈 벡터.
    static std::vector<Detection> parse(const std::string& xml);
};
