#include "Playlist.h"
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

ItemType Playlist::DetectType(const std::string& path)
{
    auto ext = path.rfind('.');
    if (ext == std::string::npos) return ItemType::Video;
    std::string e = ToLower(path.substr(ext + 1));
    if (e == "jpg" || e == "jpeg" || e == "png" || e == "bmp" ||
        e == "gif" || e == "webp" || e == "tiff")
        return ItemType::Image;
    if (e == "html" || e == "htm")
        return ItemType::HTML;
    return ItemType::Video;
}

bool Playlist::LoadFromFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;

    json doc;
    try { f >> doc; } catch (...) { return false; }

    m_loop = doc.value("loop", true);
    if (doc.contains("transition")) {
        auto& t = doc["transition"];
        m_transitionType      = t.value("type", "crossfade");
        m_transitionDurationMs = t.value("duration_ms", 500.0);
    }

    m_items.clear();
    for (auto& it : doc.value("items", json::array())) {
        PlaylistItem item;
        item.path       = it.value("path", "");
        item.durationMs = it.value("duration_ms", 0.0);

        std::string typeStr = it.value("type", "");
        if (typeStr == "image")     item.type = ItemType::Image;
        else if (typeStr == "html") item.type = ItemType::HTML;
        else if (typeStr == "video") item.type = ItemType::Video;
        else                        item.type = DetectType(item.path);

        if (!item.path.empty()) m_items.push_back(std::move(item));
    }
    return true;
}

void Playlist::SaveToFile(const std::string& path) const
{
    json doc;
    doc["loop"] = m_loop;
    doc["transition"]["type"]        = m_transitionType;
    doc["transition"]["duration_ms"] = m_transitionDurationMs;

    json items = json::array();
    for (auto& it : m_items) {
        json j;
        j["path"]        = it.path;
        j["duration_ms"] = it.durationMs;
        j["type"] = (it.type == ItemType::Image) ? "image" :
                    (it.type == ItemType::HTML)  ? "html"  : "video";
        items.push_back(std::move(j));
    }
    doc["items"] = std::move(items);

    std::ofstream f(path);
    f << doc.dump(2);
}

void Playlist::AddItem(PlaylistItem item)
{
    m_items.push_back(std::move(item));
}

void Playlist::RemoveItem(int idx)
{
    if (idx >= 0 && idx < (int)m_items.size())
        m_items.erase(m_items.begin() + idx);
}

void Playlist::MoveItem(int from, int to)
{
    if (from < 0 || from >= (int)m_items.size()) return;
    to = std::clamp(to, 0, (int)m_items.size() - 1);
    auto item = m_items[from];
    m_items.erase(m_items.begin() + from);
    m_items.insert(m_items.begin() + to, std::move(item));
}
