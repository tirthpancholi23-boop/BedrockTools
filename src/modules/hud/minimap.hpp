#pragma once

// MiniMap module: terrain-coloured mini map with underground slice mode, player skin heads
// (crouched players ringed in orange), mob heads and waypoints.

#include "../Module.hpp"
#include <mutex>
#include <string>
#include <vector>

struct MiniMapEntity {
    float x, z;
    int kind;            // 0 = player, 1 = mob
    int mob;             // MobKind (see minimap.cpp) when kind == 1
    bool sneaking;       // crouched player
    bool hostile;
    std::string image;   // registered image id (player skin head), empty = fallback dot
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
    float m_opacity = 0.85f;
    bool m_rotateWithView = false;   // terrain is refreshed ~3x/s, north-up looks smoothest
    bool m_showTerrain = true;
    bool m_undergroundMode = false;  // slice view: see caves/tunnels at your height
    float m_sliceOffset = 2.f;       // underground slice height relative to you (+ above, - below), set by Y buttons
    bool m_showPlayers = true;
    bool m_showMobs = true;
    bool m_showWaypoints = true;
    bool m_highlightCrouch = true;
    bool m_showCoords = true;
    bool m_useFixedY = false;        // cave view at an exact Y level instead of relative to you
    int m_fixedY = 30;               // that exact Y level (type it in settings, or use the Y buttons)
    bool m_mapOpen = true;           // map opened/closed by the Map button; extra buttons only show when open
    bool m_showButtons = true;       // on-screen touch buttons (waypoint, Y level, cave, zoom)
    bool m_addWaypointButton = false;     // menu button
    bool m_clearWaypointsButton = false;  // menu button

private:
    void setFixedYFromText(std::string_view text);
    void updateButtons();
    void saveNow();
    void buildTerrain(void* player, float px, float py, float pz, float yawDeg);

    std::mutex m_mutex;
    std::vector<MiniMapEntity> m_entities;
    std::vector<MiniMapWaypoint> m_waypoints;
    float m_selfX = 0.f, m_selfY = 0.f, m_selfZ = 0.f, m_yaw = 0.f;
    bool m_hasSelf = false;
    bool m_terrainReady = false;
    int m_tick = 0;
};
