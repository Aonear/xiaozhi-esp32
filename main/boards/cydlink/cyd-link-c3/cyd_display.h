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
    // Emosi CYD: 0 netral, 1 senang, 2 sedih, 3 marah, 4 kaget, 5 berpikir, 6 mengantuk
    static uint8_t MapEmotion(const char* e) {
        if (!e) return 0;
        static const char* happy[] = {"happy", "laughing", "funny", "loving", "winking", "cool",
                                      "relaxed", "delicious", "kissy", "confident", "silly"};
        for (auto h : happy) if (strcmp(e, h) == 0) return 1;
        if (strcmp(e, "sad") == 0 || strcmp(e, "crying") == 0) return 2;
        if (strcmp(e, "angry") == 0) return 3;
        if (strcmp(e, "surprised") == 0 || strcmp(e, "shocked") == 0 || strcmp(e, "embarrassed") == 0) return 4;
        if (strcmp(e, "thinking") == 0 || strcmp(e, "confused") == 0) return 5;
        if (strcmp(e, "sleepy") == 0) return 6;
        return 0;
    }
};

#endif
