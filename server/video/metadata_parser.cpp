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
    // 정규화: norm = (pixel * scale + translate + 1) / 2   → [0,1]
    // (ONVIF 좌표계는 [-1,1], 화면 좌상단이 (-1,1))
    float sx = 1, sy = 1, tx = 0, ty = 0;
    size_t trans = xml.find("<tt:Transformation>");
    if (trans != std::string::npos) {
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

    auto toNorm = [](float v, float scale, float translate) {
        // ONVIF [-1,1] 좌표 → [0,1]
        float onvif = v * scale + translate;
        float n = (onvif + 1.0f) / 2.0f;
        if (n < 0) n = 0;
        if (n > 1) n = 1;
        return n;
    };

    // tt:Object 반복 추출
    size_t pos = 0;
    while ((pos = xml.find("<tt:Object ", pos)) != std::string::npos) {
        size_t obj_end = xml.find("</tt:Object>", pos);
        if (obj_end == std::string::npos) break;

        Detection d;
        d.object_id = std::atoi(attr(xml, pos, obj_end, "ObjectId", "0").c_str());

        size_t bbox = xml.find("<tt:BoundingBox", pos);
        if (bbox != std::string::npos && bbox < obj_end) {
            float pl = attrF(xml, bbox, obj_end, "left");
            float pt = attrF(xml, bbox, obj_end, "top");
            float pr = attrF(xml, bbox, obj_end, "right");
            float pb = attrF(xml, bbox, obj_end, "bottom");
            d.left = toNorm(pl, sx, tx);
            d.right = toNorm(pr, sx, tx);
            // y축 scale이 음수라 top/bottom이 뒤집힐 수 있어 정렬
            float y1 = toNorm(pt, sy, ty);
            float y2 = toNorm(pb, sy, ty);
            d.top = y1 < y2 ? y1 : y2;
            d.bottom = y1 < y2 ? y2 : y1;
        }

        // <tt:Type Likelihood="0.79">Human</tt:Type>
        size_t type = xml.find("<tt:Type ", pos);
        if (type != std::string::npos && type < obj_end) {
            d.likelihood = attrF(xml, type, obj_end, "Likelihood");
            size_t gt = xml.find('>', type);
            size_t lt = xml.find('<', gt);
            if (gt != std::string::npos && lt != std::string::npos) {
                d.type = xml.substr(gt + 1, lt - gt - 1);
            }
        }

        result.push_back(std::move(d));
        pos = obj_end + 1;
    }

    return result;
}
