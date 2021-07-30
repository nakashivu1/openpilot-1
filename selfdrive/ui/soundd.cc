#include <sys/resource.h>

#include <map>

#include <QApplication>
#include <QString>
#include <QSoundEffect>
#include <string>  //opkr

#include "cereal/messaging/messaging.h"
#include "selfdrive/common/util.h"
#include "selfdrive/hardware/hw.h"
#include "selfdrive/ui/ui.h"
#include "selfdrive/common/params.h"

// TODO: detect when we can't play sounds
// TODO: detect when we can't display the UI

class Sound : public QObject {
public:
  explicit Sound(QObject *parent = 0) {
    // TODO: merge again and add EQ in the amp config
    const QString sound_asset_path = Hardware::TICI ? "../assets/sounds_tici/" : "../assets/sounds/";
    std::tuple<AudibleAlert, QString, bool> sound_list[] = {
      {AudibleAlert::CHIME_DISENGAGE, sound_asset_path + "disengaged.wav", false},
      {AudibleAlert::CHIME_ENGAGE, sound_asset_path + "engaged.wav", false},
      {AudibleAlert::CHIME_WARNING1, sound_asset_path + "warning_1.wav", false},
      {AudibleAlert::CHIME_WARNING2, sound_asset_path + "warning_2.wav", false},
      {AudibleAlert::CHIME_WARNING2_REPEAT, sound_asset_path + "warning_2.wav", false},
      {AudibleAlert::CHIME_WARNING_REPEAT, sound_asset_path + "warning_repeat.wav", false},
      {AudibleAlert::CHIME_ERROR, sound_asset_path + "error.wav", false},
      {AudibleAlert::CHIME_PROMPT, sound_asset_path + "error.wav", false},
      {AudibleAlert::CHIME_MODE_OPENPILOT, "../assets/sounds/modeopenpilot.wav", false},
      {AudibleAlert::CHIME_MODE_DISTCURV, "../assets/sounds/modedistcurv.wav", false},
      {AudibleAlert::CHIME_MODE_DISTANCE, "../assets/sounds/modedistance.wav", false},
      {AudibleAlert::CHIME_MODE_ONEWAY, "../assets/sounds/modeoneway.wav", false},
      {AudibleAlert::CHIME_MODE_MAPONLY, "../assets/sounds/modemaponly.wav", false}
    };
    for (auto &[alert, fn, loops] : sound_list) {
      sounds[alert].first.setSource(QUrl::fromLocalFile(fn));
      sounds[alert].second = loops ? QSoundEffect::Infinite : 0;
      QObject::connect(&sounds[alert].first, &QSoundEffect::statusChanged, this, &Sound::checkStatus);
    }

    sm = new SubMaster({"carState", "controlsState"});

    QTimer *timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, this, &Sound::update);
    timer->start();
  };
  ~Sound() {
    delete sm;
  };

private slots:
  void checkStatus() {
    for (auto &[alert, kv] : sounds) {
      assert(kv.first.status() != QSoundEffect::Error);
    }
  }

  void update() {
    sm->update(100);
    if (sm->updated("carState")) {
      // scale volume with speed
      volume = util::map_val((*sm)["carState"].getCarState().getVEgo(), 0.f, 20.f,
                             Hardware::MIN_VOLUME, Hardware::MAX_VOLUME);
    }
    if (sm->updated("controlsState")) {
      const cereal::ControlsState::Reader &cs = (*sm)["controlsState"].getControlsState();
      setAlert({QString::fromStdString(cs.getAlertText1()),
                QString::fromStdString(cs.getAlertText2()),
                QString::fromStdString(cs.getAlertType()),
                cs.getAlertSize(), cs.getAlertSound()});
    } else if (sm->rcv_frame("controlsState") > 0 && (*sm)["controlsState"].getControlsState().getEnabled() &&
               ((nanos_since_boot() - sm->rcv_time("controlsState")) / 1e9 > CONTROLS_TIMEOUT)) {
      setAlert(CONTROLS_UNRESPONSIVE_ALERT);
    }
  }

  void setAlert(Alert a) {
    if (!alert.equal(a)) {
      alert = a;
      // stop sounds
      for (auto &kv : sounds) {
        // Only stop repeating sounds
        auto &[sound, loops] = kv.second;
        if (sound.loopsRemaining() == QSoundEffect::Infinite) {
          sound.stop();
        }
      }

      // play sound
      if (alert.sound != AudibleAlert::NONE) {
        auto &[sound, loops] = sounds[alert.sound];
        sound.setLoopCount(loops);
        if ((std::stof(Params().get("OpkrUIVolumeBoost")) * 0.01) < -0.03) {
          sound.setVolume(0.0);
        } else if ((std::stof(Params().get("OpkrUIVolumeBoost")) * 0.01) > 0.03) {
          sound.setVolume(std::stof(Params().get("OpkrUIVolumeBoost")) * 0.01);
        } else {
          sound.setVolume(volume);
        }
        sound.play();
      }
    }
  }

private:
  Alert alert;
  float volume = Hardware::MIN_VOLUME;
  std::map<AudibleAlert, std::pair<QSoundEffect, int>> sounds;
  SubMaster *sm;
};

int main(int argc, char **argv) {
  setpriority(PRIO_PROCESS, 0, -20);

  QApplication a(argc, argv);
  Sound sound;
  return a.exec();
}