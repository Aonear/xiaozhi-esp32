#ifndef CYD_DISPLAY_H
#define CYD_DISPLAY_H

#include "display/display.h"
#include "cyd_link.h"

#include <cstring>
#include <string>

// "Layar" yang sebenarnya tidak menggambar apa pun: emosi dan teks percakapan diteruskan ke CYD.
class CydDisplay : public NoDisplay {
public:
    virtual void SetStatus(const char* status) override { (void)status; }
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override {
        (void)notification; (void)duration_ms;
    }
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000) override {
        (void)notification; (void)duration_ms;
    }
    virtual void UpdateStatusBar(bool update_all = false) override { (void)update_all; }
    virtual void SetPowerSaveMode(bool on) override { (void)on; }
    virtual void ClearChatMessages() override {}

    virtual void SetEmotion(const char* emotion) override {
        CydLink::GetInstance().emotion = MapEmotion(emotion);
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        if (!role || !content) return;
        auto& link = CydLink::GetInstance();
        if (strcmp(role, "user") == 0) {
            link.thinking = true;      // pengguna selesai bicara, menunggu jawaban
            link.SendText(0, content);
        } else if (strcmp(role, "assistant") == 0) {
            link.SendText(1, content);
        }
    }

private:
    // Emosi CYD: 0 netral, 1 senang, 2 sedih, 3 marah, 4 kaget, 5 berpikir, 6 ngantuk,
    //            7 tertawa, 8 bingung, 9 jatuh cinta, 10 ngambek/malu, 11 kedip genit
    static uint8_t MapEmotion(const char* e) {
        if (!e) return 0;
        static const struct { const char* name; uint8_t id; } table[] = {
            {"happy", 1}, {"relaxed", 1}, {"cool", 1}, {"delicious", 1}, {"confident", 1}, {"silly", 1},
            {"laughing", 7}, {"funny", 7},
            {"loving", 9}, {"kissy", 9},
            {"winking", 11},
            {"sad", 2}, {"crying", 2},
            {"angry", 3},
            {"surprised", 4}, {"shocked", 4},
            {"thinking", 5},
            {"confused", 8},
            {"embarrassed", 10},
            {"sleepy", 6},
        };
        for (auto& t : table) if (strcmp(e, t.name) == 0) return t.id;
        return 0;
    }
};

#endif
