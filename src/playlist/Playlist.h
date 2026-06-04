#pragma once
#include <string>
#include <vector>

enum class ItemType { Video, Image, HTML };

struct PlaylistItem {
    std::string path;
    ItemType    type{ItemType::Video};
    double      durationMs{0};  // 0 = play to end (video)
};

class Playlist {
public:
    bool LoadFromFile(const std::string& jsonPath);
    void SaveToFile(const std::string& jsonPath) const;

    void AddItem(PlaylistItem item);
    void RemoveItem(int idx);
    void MoveItem(int from, int to);

    int               Size()                    const { return (int)m_items.size(); }
    const PlaylistItem& GetItem(int idx)        const { return m_items[idx]; }
    PlaylistItem&       GetItem(int idx)              { return m_items[idx]; }
    bool              IsLoop()                  const { return m_loop; }
    double            TransitionDurationSec()   const { return m_transitionDurationMs / 1000.0; }
    const std::string& TransitionType()         const { return m_transitionType; }

    void SetLoop(bool v)                              { m_loop = v; }
    void SetTransitionDuration(double ms)             { m_transitionDurationMs = ms; }

private:
    static ItemType DetectType(const std::string& path);

    std::vector<PlaylistItem> m_items;
    bool        m_loop{true};
    double      m_transitionDurationMs{500.0};
    std::string m_transitionType{"crossfade"};
};
