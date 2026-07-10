#include "metadata_parser.hpp"

#include <cstdlib>

namespace {

// s에서 pos 이후 attr="값" 형태의 값을 읽는다. 없으면 def.
std::string attr(const std::string& s, size_t start, size_t end,
                 const std::string& name, const std::string& def = "") {
    std::string key = name + "=\"";
    size_t p = s.find(key, start);
    if (p == std::string::npos || p >= end) {
        return def;
    }
    p += key.size();
    size_t q = s.find('"', p);
    if (q == std::string::npos || q > end) {
        return def;
    }
    return s.substr(p, q - p);
}

float attrF(const std::string& s, size_t start, size_t end,
            const std::string& name, float def = 0.0f) {
    std::string v = attr(s, start, end, name);
    return v.empty() ? def : std::strtof(v.c_str(), nullptr);
}

// s의 [start,end) 구간에서 <tag>텍스트</tag> 값을 읽는다. 없으면 def.
std::string elemText(const std::string& s, size_t start, size_t end,
                     const std::string& tag, const std::string& def = "") {
    std::string open = "<" + tag + ">";
    size_t p = s.find(open, start);
    if (p == std::string::npos || p >= end) {
        return def;
    }
    p += open.size();
    size_t q = s.find("</" + tag + ">", p);
    if (q == std::string::npos || q > end) {
        return def;
    }
    return s.substr(p, q - p);
}

}  // namespace

std::vector<Detection> MetadataParser::parse(const std::string& xml) {
    std::vector<Detection> result;

    // ObjectDetection 프레임이 아니면 관심 없음 (모션/오디오 이벤트 등은 스킵)
    if (xml.find("WiseAI/ObjectDetection") == std::string::npos &&
        xml.find("<tt:Object ") == std::string::npos) {
        return result;
    }

    // tt:Transformation 의 Scale/Translate — 픽셀 → 0~1 정규화 계수.
    // <tt:Translate x="-1.0" y="1.0"/> <tt:Scale x="0.000772" y="-0.001316"/>
    // ONVIF 좌표계는 [-1,1], 화면 좌상단이 (-1,+1) — y는 위가 양수.
    // 화면 좌표(0=상단)로 맞추기 위해 x: (onvif+1)/2, y: (1-onvif)/2.
    float sx = 1, sy = 1, tx = 0, ty = 0;
    size_t trans = xml.find("<tt:Transformation>");
    if (trans == std::string::npos) {
        // 정규화 계수 없이 픽셀값을 해석하면 전 좌표가 1.0로 뭉개진다.
        // (패킷 분할로 Transformation이 앞 패킷에 실려온 경우 등) — 스킵.
        return result;
    }
    {
        size_t tend = xml.find("</tt:Transformation>", trans);
        if (tend == std::string::npos) tend = xml.size();
        size_t scale = xml.find("<tt:Scale", trans);
        if (scale != std::string::npos && scale < tend) {
            sx = attrF(xml, scale, tend, "x", 1);
            sy = attrF(xml, scale, tend, "y", 1);
        }
        size_t tr = xml.find("<tt:Translate", trans);
        if (tr != std::string::npos && tr < tend) {
            tx = attrF(xml, tr, tend, "x", 0);
            ty = attrF(xml, tr, tend, "y", 0);
        }
    }

    auto clamp01 = [](float n) {
        if (n < 0) n = 0;
        if (n > 1) n = 1;
        return n;
    };
    // ONVIF [-1,1] → [0,1]. x는 -1이 왼쪽이므로 그대로,
    // y는 +1이 "위"라서 뒤집어야 화면 좌표(0=상단, 1=하단)가 된다.
    // 안 뒤집으면 cy가 낙하 시 감소해 낙상 판정(cy 증가 감시)이 전부 무산된다.
    auto toNormX = [&](float v) { return clamp01((v * sx + tx + 1.0f) / 2.0f); };
    auto toNormY = [&](float v) { return clamp01((1.0f - (v * sy + ty)) / 2.0f); };

    // tt:Object 반복 추출
    size_t pos = 0;
    while ((pos = xml.find("<tt:Object ", pos)) != std::string::npos) {
        size_t obj_end = xml.find("</tt:Object>", pos);
        if (obj_end == std::string::npos) break;

        Detection d;
        d.object_id = std::atoi(attr(xml, pos, obj_end, "ObjectId", "0").c_str());
        // Head 등 하위 객체는 Parent로 자기가 속한 Human을 가리킨다
        d.parent_id = std::atoi(attr(xml, pos, obj_end, "Parent", "0").c_str());

        size_t bbox = xml.find("<tt:BoundingBox", pos);
        if (bbox != std::string::npos && bbox < obj_end) {
            float pl = attrF(xml, bbox, obj_end, "left");
            float pt = attrF(xml, bbox, obj_end, "top");
            float pr = attrF(xml, bbox, obj_end, "right");
            float pb = attrF(xml, bbox, obj_end, "bottom");
            d.left = toNormX(pl);
            d.right = toNormX(pr);
            // scale 부호에 따라 top/bottom이 뒤집힐 수 있어 정렬
            float y1 = toNormY(pt);
            float y2 = toNormY(pb);
            d.top = y1 < y2 ? y1 : y2;
            d.bottom = y1 < y2 ? y2 : y1;
        }

        // <tt:CenterOfGravity x="488.0" y="341.0"/> — 무게중심 (낙하 속도 추적용)
        size_t cog = xml.find("<tt:CenterOfGravity", pos);
        if (cog != std::string::npos && cog < obj_end) {
            d.cx = toNormX(attrF(xml, cog, obj_end, "x"));
            d.cy = toNormY(attrF(xml, cog, obj_end, "y"));
        }

        // 최종 판정 클래스: <tt:Type Likelihood="0.79">Human</tt:Type>
        size_t type = xml.find("<tt:Type ", pos);
        if (type != std::string::npos && type < obj_end) {
            d.likelihood = attrF(xml, type, obj_end, "Likelihood");
            size_t gt = xml.find('>', type);
            size_t lt = xml.find('<', gt);
            if (gt != std::string::npos && lt != std::string::npos) {
                d.type = xml.substr(gt + 1, lt - gt - 1);
            }
        } else {
            // 폴백: 최종 판정이 없으면 후보 목록에서 최고 확률 채택
            // <tt:ClassCandidate><tt:Type>Human</tt:Type><tt:Likelihood>0.79</tt:Likelihood>
            size_t cand = pos;
            while ((cand = xml.find("<tt:ClassCandidate>", cand)) != std::string::npos &&
                   cand < obj_end) {
                size_t cend = xml.find("</tt:ClassCandidate>", cand);
                if (cend == std::string::npos || cend > obj_end) break;
                std::string t = elemText(xml, cand, cend, "tt:Type");
                float lh = std::strtof(
                    elemText(xml, cand, cend, "tt:Likelihood", "0").c_str(), nullptr);
                if (!t.empty() && (d.type.empty() || lh > d.likelihood)) {
                    d.type = std::move(t);
                    d.likelihood = lh;
                }
                cand = cend + 1;
            }
        }

        result.push_back(std::move(d));
        pos = obj_end + 1;
    }

    return result;
}
