#pragma once

// MiniMap module: toggleable radar-style mini map with players (crouch indicator),
// mobs and waypoints. Draws through the LeviLauncher mod-menu draw API like Compass.

#include "../Module.hpp"
#include <mutex>
#include <vector>

struct MiniMapEntity {
    float x, z;
    int kind;        // 0 = player, 1 = hostile mob, 2 = other mob
    bool sneaking;   // crouched player
};

struct MiniMapWaypoint { float x, y, z; };

class MiniMapModule : public Module {
public:
    MiniMapModule();
    ~MiniMapModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    bool showInLegacyMenu(std::string_view key) const override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void onLocalPlayerTick(void* localPlayer);
    void addWaypointHere();

    float hudPosX = 1500.f;
    float hudPosY = 20.f;
    bool isHudModule = true;

    float m_mapWidth = 300.f;        // map box size in HUD pixels
    float m_zoomScale = 1.0f;        // 1.0 = 6 px per block
    float m_dotScale = 1.0f;
    float m_opacity = 0.7f;
    bool m_rotateWithView = true;
    bool m_showPlayers = true;
    bool m_showMobs = true;
    bool m_showWaypoints = true;
    bool m_highlightCrouch = true;
    bool m_addWaypointButton = false;     // menu button
    bool m_clearWaypointsButton = false;  // menu button

private:
    std::mutex m_mutex;
    std::vector<MiniMapEntity> m_entities;
    std::vector<MiniMapWaypoint> m_waypoints;
    float m_selfX = 0.f, m_selfY = 0.f, m_selfZ = 0.f, m_yaw = 0.f;
    bool m_hasSelf = false;
    int m_tick = 0;
};
