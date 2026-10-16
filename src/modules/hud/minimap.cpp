#include "minimap.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
using GetRuntimeActorList_t = std::vector<void*> (*)(void*);
using ActorIsPlayer_t = bool (*)(void*);

GetRuntimeActorList_t s_getActors = nullptr;
ActorIsPlayer_t s_isPlayer = nullptr;
MiniMapModule* g_mod = nullptr;

constexpr float kBasePxPerBlock = 6.f;
constexpr std::uint32_t kCatMob = 2;      // same bit Hitbox uses for mobs
constexpr std::uint32_t kCatMonster = 4;  // hostile mobs (ActorCategory::Monster)

std::uint32_t rgba(float a, float r, float g, float b) {
    auto c = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f); };
    return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

PLModMenu_DrawCommand rectCmd(float x, float y, float w, float h, std::uint32_t col) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_RECT_FILLED;
    c.x = x; c.y = y; c.w = w; c.h = h; c.color = col;
    return c;
}

PLModMenu_DrawCommand dotCmd(float x, float y, float radius, std::uint32_t col) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_CIRCLE_FILLED;
    c.x = x; c.y = y; c.size = radius; c.color = col;
    return c;
}
}

MiniMapModule::MiniMapModule()
    : Module("MiniMap", "Mini map showing players (crouched ones highlighted), mobs and waypoints.") {
    g_mod = this;
}

MiniMapModule::~MiniMapModule() {
    if (g_mod == this) g_mod = nullptr;
}

void MiniMapModule::onInit() {
    s_getActors = reinterpret_cast<GetRuntimeActorList_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
    s_isPlayer = reinterpret_cast<ActorIsPlayer_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (g_mod && g_mod->enabled && event.player) g_mod->onLocalPlayerTick(event.player);
    });
}

void MiniMapModule::onEnable() { m_tick = 0; }

void MiniMapModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entities.clear();
    m_hasSelf = false;
    submitDrawCommands(moduleId, {});
}

void MiniMapModule::onLocalPlayerTick(void* localPlayer) {
    auto* player = static_cast<bedrocktools::sdk::Player*>(localPlayer);
    const auto pos = player->position();
    const auto rot = player->rotation();

    std::vector<MiniMapEntity> next;
    const float range = (m_mapWidth * 0.5f) / (kBasePxPerBlock * m_zoomScale) * 1.5f;  // covers rotated corners

    if (s_getActors) {
        auto* level = player->level();
        void* manager = level ? level->actorManager() : nullptr;
        if (manager) {
            for (void* raw : s_getActors(manager)) {
                if (!raw || raw == localPlayer) continue;
                auto* actor = static_cast<bedrocktools::sdk::Actor*>(raw);
                const auto p = actor->position();
                if (std::fabs(p.x - pos.x) > range || std::fabs(p.z - pos.z) > range) continue;

                const bool isPlayer = s_isPlayer && s_isPlayer(raw);
                if (isPlayer) {
                    // A crouched player's hitbox is shorter than a standing one (1.8 -> ~1.5).
                    const auto box = actor->bounds();
                    const float h = box.max.y - box.min.y;
                    const bool sneaking = h > 1.2f && h < 1.72f;
                    next.push_back({p.x, p.z, 0, sneaking});
                } else if (actor->hasCategory(kCatMob)) {
                    next.push_back({p.x, p.z, actor->hasCategory(kCatMonster) ? 1 : 2, false});
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entities.swap(next);
    m_selfX = pos.x; m_selfY = pos.y; m_selfZ = pos.z;
    m_yaw = rot.y;
    m_hasSelf = true;
}

void MiniMapModule::addWaypointHere() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasSelf) return;
    m_waypoints.push_back({m_selfX, m_selfY, m_selfZ});
}

bool MiniMapModule::onMenuConfigChanged(std::string_view key, std::string_view) {
    if (key == "m_addWaypointButton") { addWaypointHere(); return true; }
    if (key == "m_clearWaypointsButton") {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waypoints.clear();
        return true;
    }
    return false;
}

bool MiniMapModule::showInLegacyMenu(std::string_view key) const {
    return key != "waypoints";
}

void MiniMapModule::onFrame() {
    if (!enabled) return;

    std::vector<MiniMapEntity> ents;
    std::vector<MiniMapWaypoint> wps;
    float sx, sz, yaw;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_hasSelf) return;
        ents = m_entities; wps = m_waypoints;
        sx = m_selfX; sz = m_selfZ; yaw = m_yaw;
    }

    const float W = m_mapWidth;
    const float x0 = hudPosX, y0 = hudPosY;
    const float cx = x0 + W * 0.5f, cy = y0 + W * 0.5f;
    const float half = W * 0.5f - 4.f;
    const float pxPerBlock = kBasePxPerBlock * m_zoomScale;
    const float dot = 5.f * m_dotScale;

    std::vector<PLModMenu_DrawCommand> cmds;

    // border + background
    const std::uint32_t border = rgba(m_opacity, 1, 1, 1);
    cmds.push_back(rectCmd(x0 - 3, y0 - 3, W + 6, W + 6, border));
    cmds.push_back(rectCmd(x0, y0, W, W, rgba(m_opacity, 0.04f, 0.06f, 0.04f)));
    // cross hairs
    cmds.push_back(rectCmd(cx - 0.5f, y0, 1.f, W, rgba(m_opacity * 0.25f, 1, 1, 1)));
    cmds.push_back(rectCmd(x0, cy - 0.5f, W, 1.f, rgba(m_opacity * 0.25f, 1, 1, 1)));

    // world offset -> map pixels. Rotating mode: your facing direction is always "up".
    const float yawR = yaw * 3.14159265f / 180.f;
    const float cs = std::cos(yawR), sn = std::sin(yawR);
    auto project = [&](float dx, float dz, float& px, float& py) {
        if (m_rotateWithView) {
            const float right = dx * -cs + dz * -sn;
            const float fwd = dx * -sn + dz * cs;
            px = right * pxPerBlock;
            py = -fwd * pxPerBlock;
        } else {  // north up
            px = dx * pxPerBlock;
            py = dz * pxPerBlock;
        }
    };

    // north marker
    {
        float px, py;
        project(0.f, -1000.f, px, py);
        const float len = std::sqrt(px * px + py * py);
        if (len > 0.f) {
            px = px / len * (half - 12.f);
            py = py / len * (half - 12.f);
            PLModMenu_DrawCommand t = {};
            t.type = PL_DRAW_TEXT;
            t.x = cx + px; t.y = cy + py - 10.f; t.w = 1.f; t.h = 1.f;
            t.color = rgba(1, 1, 0.3f, 0.3f);
            t.size = 22.f;
            t.text = "N";
            cmds.push_back(t);
        }
    }

    // waypoints (clamped to the edge like a compass)
    if (m_showWaypoints) {
        static const std::uint32_t pal[4] = {
            rgba(1, 0.3f, 0.6f, 1.f), rgba(1, 1.f, 0.4f, 0.8f), rgba(1, 0.6f, 1.f, 0.9f), rgba(1, 1.f, 0.8f, 0.3f)};
        for (size_t i = 0; i < wps.size(); ++i) {
            float px, py;
            project(wps[i].x - sx, wps[i].z - sz, px, py);
            const bool inside = std::fabs(px) <= half && std::fabs(py) <= half;
            px = std::clamp(px, -half, half);
            py = std::clamp(py, -half, half);
            cmds.push_back(dotCmd(cx + px, cy + py, (inside ? 1.2f : 0.9f) * dot, pal[i % 4]));
        }
    }

    // entities
    for (const auto& e : ents) {
        if (e.kind == 0 && !m_showPlayers) continue;
        if (e.kind != 0 && !m_showMobs) continue;
        float px, py;
        project(e.x - sx, e.z - sz, px, py);
        if (std::fabs(px) > half || std::fabs(py) > half) continue;
        const float ex = cx + px, ey = cy + py;
        if (e.kind == 0) {
            if (e.sneaking && m_highlightCrouch) {   // crouched: orange dot with white ring
                cmds.push_back(dotCmd(ex, ey, 1.6f * dot, rgba(1, 1, 1, 1)));
                cmds.push_back(dotCmd(ex, ey, 1.2f * dot, rgba(1, 1.f, 0.5f, 0.f)));
            } else {
                cmds.push_back(dotCmd(ex, ey, 1.1f * dot, rgba(1, 1.f, 1.f, 0.f)));
            }
        } else if (e.kind == 1) {
            cmds.push_back(dotCmd(ex, ey, 0.8f * dot, rgba(1, 1.f, 0.15f, 0.15f)));
        } else {
            cmds.push_back(dotCmd(ex, ey, 0.8f * dot, rgba(1, 0.3f, 1.f, 0.3f)));
        }
    }

    // you: white dot + arrow showing facing direction
    {
        float fx = 0.f, fy = -1.f;
        if (!m_rotateWithView) { fx = -sn; fy = cs; }
        const float tip = 16.f * m_dotScale, base = 7.f * m_dotScale;
        PLModMenu_DrawCommand tri = {};
        tri.type = PL_DRAW_TRIANGLE_FILLED;
        tri.x = cx + fx * tip;             tri.y = cy + fy * tip;
        tri.w = cx - fy * base - fx * 3.f; tri.h = cy + fx * base - fy * 3.f;
        tri.x3 = cx + fy * base - fx * 3.f; tri.y3 = cy - fx * base - fy * 3.f;
        tri.color = rgba(1, 1, 1, 1);
        cmds.push_back(tri);
        cmds.push_back(dotCmd(cx, cy, 0.9f * dot, rgba(1, 1, 1, 1)));
    }

    submitDrawCommands(moduleId, cmds);
}

void MiniMapModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    auto f = [&](const char* k, float& v) { if (j.contains(k) && j[k].is_number()) v = j[k].get<float>(); };
    auto b = [&](const char* k, bool& v) { if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>(); };
    f("hudPosX", hudPosX); f("hudPosY", hudPosY);
    b("isHudModule", isHudModule);
    f("m_mapWidth", m_mapWidth); f("m_zoomScale", m_zoomScale); f("m_dotScale", m_dotScale);
    f("m_opacity", m_opacity);
    b("m_rotateWithView", m_rotateWithView); b("m_showPlayers", m_showPlayers);
    b("m_showMobs", m_showMobs); b("m_showWaypoints", m_showWaypoints);
    b("m_highlightCrouch", m_highlightCrouch);
    if (j.contains("waypoints") && j["waypoints"].is_array()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waypoints.clear();
        for (const auto& w : j["waypoints"])
            if (w.is_array() && w.size() == 3)
                m_waypoints.push_back({w[0].get<float>(), w[1].get<float>(), w[2].get<float>()});
    }
}

void MiniMapModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX; j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_mapWidth"] = m_mapWidth; j["m_zoomScale"] = m_zoomScale; j["m_dotScale"] = m_dotScale;
    j["m_opacity"] = m_opacity;
    j["m_rotateWithView"] = m_rotateWithView; j["m_showPlayers"] = m_showPlayers;
    j["m_showMobs"] = m_showMobs; j["m_showWaypoints"] = m_showWaypoints;
    j["m_highlightCrouch"] = m_highlightCrouch;
    j["m_addWaypointButton"] = false;
    j["m_clearWaypointsButton"] = false;
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& w : m_waypoints) arr.push_back({w.x, w.y, w.z});
    }
    j["waypoints"] = arr;
}
