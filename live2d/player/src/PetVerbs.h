/**
 * PetVerbs.h — 触摸动作的中文动词（按部位）。所有给人看/给 LLM 看的文案都从这里取，保证一致：
 *   点击（click）：手 = 牵，脸 = 捏，其余 = 戳
 *   拖动（rub，来回拖）：头、手 = 摸，其余 = 揉
 * zoneKey 用协议里的部位键（head / face / hand.l / hand.r / belly …）。
 */
#pragma once

#include <cstring>

inline const char* TouchVerb(const char* zoneKey, bool rub)
{
    const bool hand = zoneKey != nullptr && std::strncmp(zoneKey, "hand", 4) == 0;
    const bool head = zoneKey != nullptr && std::strcmp(zoneKey, "head") == 0;
    const bool face = zoneKey != nullptr && std::strcmp(zoneKey, "face") == 0;
    if (rub) return (head || hand) ? "摸" : "揉";
    return hand ? "牵" : (face ? "捏" : "戳");
}
