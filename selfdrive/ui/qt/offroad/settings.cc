#include "selfdrive/ui/qt/offroad/settings.h"

#include <cassert>
#include <string>

#include <QDebug>
#include <QProcess> // opkr
#include <QDateTime> // opkr
#include <QTimer> // opkr

#ifndef QCOM
#include "selfdrive/ui/qt/offroad/networking.h"
#endif

#ifdef ENABLE_MAPS
#include "selfdrive/ui/qt/maps/map_settings.h"
#endif

#include "selfdrive/common/params.h"
#include "selfdrive/common/util.h"
#include "selfdrive/hardware/hw.h"
#include "selfdrive/ui/qt/widgets/controls.h"
#include "selfdrive/ui/qt/widgets/input.h"
#include "selfdrive/ui/qt/widgets/scrollview.h"
#include "selfdrive/ui/qt/widgets/ssh_keys.h"
#include "selfdrive/ui/qt/widgets/toggle.h"
#include "selfdrive/ui/ui.h"
#include "selfdrive/ui/qt/util.h"

TogglesPanel::TogglesPanel(QWidget *parent) : QWidget(parent) {
  QVBoxLayout *main_layout = new QVBoxLayout(this);

  QList<ParamControl*> toggles;

  toggles.append(new ParamControl("OpenpilotEnabledToggle",
                                  "Enable openpilot",
                                  "Use the openpilot system for adaptive cruise control and lane keep driver assistance. Your attention is required at all times to use this feature. Changing this setting takes effect when the car is powered off.",
                                  "../assets/offroad/icon_openpilot.png",
                                  this));
  toggles.append(new ParamControl("IsLdwEnabled",
                                  "Enable Lane Departure Warnings",
                                  "Receive alerts to steer back into the lane when your vehicle drifts over a detected lane line without a turn signal activated while driving over 31mph (50kph).",
                                  "../assets/offroad/icon_warning.png",
                                  this));
  toggles.append(new ParamControl("IsRHD",
                                  "Enable Right-Hand Drive",
                                  "Allow openpilot to obey left-hand traffic conventions and perform driver monitoring on right driver seat.",
                                  "../assets/offroad/icon_openpilot_mirrored.png",
                                  this));
  toggles.append(new ParamControl("IsMetric",
                                  "Use Metric System",
                                  "Display speed in km/h instead of mp/h.",
                                  "../assets/offroad/icon_metric.png",
                                  this));
  toggles.append(new ParamControl("CommunityFeaturesToggle",
                                  "Enable Community Features",
                                  "Use features from the open source community that are not maintained or supported by comma.ai and have not been confirmed to meet the standard safety model. These features include community supported cars and community supported hardware. Be extra cautious when using these features.",
                                  "../assets/offroad/icon_shell.png",
                                  this));

  toggles.append(new ParamControl("UploadRaw",
                                 "Upload Raw Logs",
                                 "Upload full logs at my.comma.ai/useradmin (only works while on WiFi).",
                                 "../assets/offroad/icon_network.png",
                                 this));

  ParamControl *record_toggle = new ParamControl("RecordFront",
                                                 "Record and Upload Driver Camera",
                                                 "Upload data from the driver facing camera and help improve the driver monitoring algorithm.",
                                                 "../assets/offroad/icon_network.png",
                                                 this);
  toggles.append(record_toggle);
  toggles.append(new ParamControl("EndToEndToggle",
                                   "\U0001f96c Disable use of lanelines (Alpha) \U0001f96c",
                                   "In this mode openpilot will ignore lanelines and just drive how it thinks a human would.",
                                   "../assets/offroad/icon_road.png",
                                   this));

  if (Hardware::TICI()) {
    toggles.append(new ParamControl("EnableWideCamera",
                                    "Enable use of Wide Angle Camera",
                                    "Use wide angle camera for driving and ui.",
                                    "../assets/offroad/icon_openpilot.png",
                                    this));
    QObject::connect(toggles.back(), &ToggleControl::toggleFlipped, [=](bool state) {
      Params().remove("CalibrationParams");
    });
  }

#ifdef ENABLE_MAPS
  toggles.append(new ParamControl("NavSettingTime24h",
                                  "Show ETA in 24h format",
                                  "Use 24h format instead of am/pm",
                                  "../assets/offroad/icon_metric.png",
                                  this));
#endif

  toggles.append(new ParamControl("OpkrEnableDriverMonitoring",
                                  "Enable Driver Monitoring",
                                  "Use driver supervision monitoring.",
                                  "../assets/offroad/icon_shell.png",
                                  this));
  toggles.append(new ParamControl("OpkrEnableLogger",
                                  "Enable Logger",
                                  "Record driving logs for data analysis locally. Only the logger is active and not uploaded to the server.",
                                  "../assets/offroad/icon_shell.png",
                                  this));
  toggles.append(new ParamControl("OpkrEnableUploader",
                                  "Enable Uploader",
                                  "Activates the upload process to send system logs and other driving data to the server. Upload only in off-road conditions.",
                                  "../assets/offroad/icon_shell.png",
                                  this));
  toggles.append(new ParamControl("CommaStockUI",
                                  "Enable Comma Stock UI",
                                  "Use the stock UI of comma for the driving screen. You can also switch in real time by clicking the box on the top left of the driving screen.",
                                  "../assets/offroad/icon_shell.png",
                                  this));

  bool record_lock = Params().getBool("RecordFrontLock");
  record_toggle->setEnabled(!record_lock);

  for(ParamControl *toggle : toggles) {
    if(main_layout->count() != 0) {
      main_layout->addWidget(horizontal_line());
    }
    main_layout->addWidget(toggle);
  }
}

DevicePanel::DevicePanel(QWidget* parent) : QWidget(parent) {
  QVBoxLayout *main_layout = new QVBoxLayout(this);
  Params params = Params();

  QString dongle = QString::fromStdString(params.get("DongleId", false));
  main_layout->addWidget(new LabelControl("Dongle ID", dongle));
  main_layout->addWidget(horizontal_line());

  QString serial = QString::fromStdString(params.get("HardwareSerial", false));
  main_layout->addWidget(new LabelControl("Serial", serial));

  // offroad-only buttons

  auto dcamBtn = new ButtonControl("Driver Camera", "PREVIEW",
                                        "Preview the driver facing camera to help optimize device mounting position for best driver monitoring experience. (vehicle must be off)");
  connect(dcamBtn, &ButtonControl::released, [=]() { emit showDriverView(); });

  QString resetCalibDesc = "openpilot requires the device to be mounted within 4° left or right and within 5° up or down. openpilot is continuously calibrating, resetting is rarely required.";
  auto resetCalibBtn = new ButtonControl("Reset Calibration", "RESET", resetCalibDesc);
  connect(resetCalibBtn, &ButtonControl::released, [=]() {
    QString desc = "[Reference value: within L/R 4° and UP/DN 5°]";
    std::string calib_bytes = Params().get("CalibrationParams");
    if (!calib_bytes.empty()) {
      try {
        AlignedBuffer aligned_buf;
        capnp::FlatArrayMessageReader cmsg(aligned_buf.align(calib_bytes.data(), calib_bytes.size()));
        auto calib = cmsg.getRoot<cereal::Event>().getLiveCalibration();
        if (calib.getCalStatus() != 0) {
          double pitch = calib.getRpyCalib()[1] * (180 / M_PI);
          double yaw = calib.getRpyCalib()[2] * (180 / M_PI);
          desc += QString(" Your device is pointed %1° %2 and %3° %4.")
                                .arg(QString::number(std::abs(pitch), 'g', 1), pitch > 0 ? "↑" : "↓",
                                     QString::number(std::abs(yaw), 'g', 1), yaw > 0 ? "→" : "←");
        }
      } catch (kj::Exception) {
        qInfo() << "invalid CalibrationParams";
      }
    }
    if (ConfirmationDialog::alert(desc, this)) {
      //Params().remove("CalibrationParams");
    }
  });
  connect(resetCalibBtn, &ButtonControl::showDescription, [=]() {
    QString desc = resetCalibDesc;
    std::string calib_bytes = Params().get("CalibrationParams");
    if (!calib_bytes.empty()) {
      try {
        AlignedBuffer aligned_buf;
        capnp::FlatArrayMessageReader cmsg(aligned_buf.align(calib_bytes.data(), calib_bytes.size()));
        auto calib = cmsg.getRoot<cereal::Event>().getLiveCalibration();
        if (calib.getCalStatus() != 0) {
          double pitch = calib.getRpyCalib()[1] * (180 / M_PI);
          double yaw = calib.getRpyCalib()[2] * (180 / M_PI);
          desc += QString(" Your device is pointed %1° %2 and %3° %4.")
                                .arg(QString::number(std::abs(pitch), 'g', 1), pitch > 0 ? "↑" : "↓",
                                     QString::number(std::abs(yaw), 'g', 1), yaw > 0 ? "→" : "←");
        }
      } catch (kj::Exception) {
        qInfo() << "invalid CalibrationParams";
      }
    }
    resetCalibBtn->setDescription(desc);
  });

  ButtonControl *retrainingBtn = nullptr;
  if (!params.getBool("Passive")) {
    retrainingBtn = new ButtonControl("Review Training Guide", "REVIEW", "Review the rules, features, and limitations of openpilot.");
    connect(retrainingBtn, &ButtonControl::released, [=]() {
      if (ConfirmationDialog::confirm("Are you sure you want to review the training guide?", this)) {
        Params().remove("CompletedTrainingVersion");
        emit reviewTrainingGuide();
      }
    });
  }

  auto uninstallBtn = new ButtonControl(getBrand() + " Dashcam", "openpilot");
  connect(uninstallBtn, &ButtonControl::released, [=]() {
    if (ConfirmationDialog::confirm("Are you sure you want to uninstall?", this)) {
      Params().putBool("DoUninstall", true);
    }
  });

  for (auto btn : {dcamBtn, resetCalibBtn, retrainingBtn, uninstallBtn}) {
    if (btn) {
      main_layout->addWidget(horizontal_line());
      connect(parent, SIGNAL(offroadTransition(bool)), btn, SLOT(setEnabled(bool)));
      main_layout->addWidget(btn);
    }
  }

  main_layout->addWidget(horizontal_line());

  // cal reset and param init buttons
  QHBoxLayout *cal_param_init_layout = new QHBoxLayout();
  cal_param_init_layout->setSpacing(50);

  QPushButton *calinit_btn = new QPushButton("Calibration Reset");
  calinit_btn->setStyleSheet("height: 120px;border-radius: 15px;background-color: #393939;");
  cal_param_init_layout->addWidget(calinit_btn);
  QObject::connect(calinit_btn, &QPushButton::released, [=]() {
    if (ConfirmationDialog::confirm("Are you sure you want to reset calibration? The device will automatically reboot.", this)) {
      Params().remove("CalibrationParams");
      Params().remove("LiveParameters");
      QTimer::singleShot(1000, []() {
        Hardware::reboot();
      });
    }
  });

  QPushButton *paraminit_btn = new QPushButton("Parameter Initialization");
  paraminit_btn->setStyleSheet("height: 120px;border-radius: 15px;background-color: #393939;");
  cal_param_init_layout->addWidget(paraminit_btn);
  QObject::connect(paraminit_btn, &QPushButton::released, [=]() {
    if (ConfirmationDialog::confirm("Reset the parameters to their initial state. Are you sure you want to proceed?", this)) {
      QProcess::execute("/data/openpilot/init_param.sh");
    }
  });

  // preset1 buttons
  QHBoxLayout *presetone_layout = new QHBoxLayout();
  presetone_layout->setSpacing(50);

  QPushButton *presetoneload_btn = new QPushButton("Load Preset 1");
  presetoneload_btn->setStyleSheet("height: 120px;border-radius: 15px;background-color: #393939;");
  presetone_layout->addWidget(presetoneload_btn);
  QObject::connect(presetoneload_btn, &QPushButton::released, [=]() {
    if (ConfirmationDialog::confirm("Are you sure you want to load Preset 1?", this)) {
      QProcess::execute("/data/openpilot/load_preset1.sh");
    }
  });

  QPushButton *presetonesave_btn = new QPushButton("Save Preset 1");
  presetonesave_btn->setStyleSheet("height: 120px;border-radius: 15px;background-color: #393939;");
  presetone_layout->addWidget(presetonesave_btn);
  QObject::connect(presetonesave_btn, &QPushButton::released, [=]() {
    if (ConfirmationDialog::confirm("Are you sure you want to save Preset 1?", this)) {
      QProcess::execute("/data/openpilot/save_preset1.sh");
    }
  });

  // preset2 buttons
  QHBoxLayout *presettwo_layout = new QHBoxLayout();
  presettwo_layout->setSpacing(50);

  QPushButton *presettwoload_btn = new QPushButton("Load Preset 2");
  presettwoload_btn->setStyleSheet("height: 120px;border-radius: 15px;background-color: #393939;");
  presettwo_layout->addWidget(presettwoload_btn);
  QObject::connect(presettwoload_btn, &QPushButton::released, [=]() {
    if (ConfirmationDialog::confirm("Are you sure you want to save Preset 2?", this)) {
      QProcess::execute("/data/openpilot/load_preset2.sh");
    }
  });

  QPushButton *presettwosave_btn = new QPushButton("Save Preset 2");
  presettwosave_btn->setStyleSheet("height: 120px;border-radius: 15px;background-color: #393939;");
  presettwo_layout->addWidget(presettwosave_btn);
  QObject::connect(presettwosave_btn, &QPushButton::released, [=]() {
    if (ConfirmationDialog::confirm("Are you sure you want to save Preset 2?", this)) {
      QProcess::execute("/data/openpilot/save_preset2.sh");
    }
  });

  // power buttons
  QHBoxLayout *power_layout = new QHBoxLayout();
  power_layout->setSpacing(50);

  QPushButton *reboot_btn = new QPushButton("Reboot");
  reboot_btn->setStyleSheet("height: 120px;border-radius: 15px;background-color: #393939;");
  power_layout->addWidget(reboot_btn);
  QObject::connect(reboot_btn, &QPushButton::released, [=]() {
    if (ConfirmationDialog::confirm("Are you sure you want to reboot?", this)) {
      Hardware::reboot();
    }
  });

  QPushButton *poweroff_btn = new QPushButton("Power Off");
  poweroff_btn->setStyleSheet("height: 120px;border-radius: 15px;background-color: #E22C2C;");
  power_layout->addWidget(poweroff_btn);
  QObject::connect(poweroff_btn, &QPushButton::released, [=]() {
    if (ConfirmationDialog::confirm("Are you sure you want to power off?", this)) {
      Hardware::poweroff();
    }
  });

  main_layout->addLayout(cal_param_init_layout);

  main_layout->addWidget(horizontal_line());

  main_layout->addLayout(presetone_layout);
  main_layout->addLayout(presettwo_layout);

  main_layout->addWidget(horizontal_line());

  main_layout->addLayout(power_layout);
}

SoftwarePanel::SoftwarePanel(QWidget* parent) : QWidget(parent) {
  gitRemoteLbl = new LabelControl("Git Remote");
  gitBranchLbl = new LabelControl("Git Branch");
  gitCommitLbl = new LabelControl("Git Commit");
  osVersionLbl = new LabelControl("OS Version");
  versionLbl = new LabelControl("Version");
  lastUpdateLbl = new LabelControl("Check for Update", "", "");
  updateBtn = new ButtonControl("Check and Apply Update", "");
  connect(updateBtn, &ButtonControl::released, [=]() {
    if (params.getBool("IsOffroad")) {
      const QString paramsPath = QString::fromStdString(params.getParamsPath());
      fs_watch->addPath(paramsPath + "/d/LastUpdateTime");
      fs_watch->addPath(paramsPath + "/d/UpdateFailedCount");
    }
    std::system("/data/openpilot/gitcommit.sh");
    std::system("date '+%F %T' > /data/params/d/LastUpdateTime");
    QString desc = "";
    QString commit_local = QString::fromStdString(Params().get("GitCommit").substr(0, 10));
    QString commit_remote = QString::fromStdString(Params().get("GitCommitRemote").substr(0, 10));
    QString empty = "";
    desc += QString("Local: %1\nRemote: %2%3%4\n").arg(commit_local, commit_remote, empty, empty);
    if (commit_local == commit_remote) {
      desc += QString("Local and Remote match. No update required.");
    } else {
      desc += QString("An update is available. Click OK to apply.");
    }
    if (ConfirmationDialog::confirm(desc, this)) {
      std::system("/data/openpilot/gitpull.sh");
    }
  });

  QVBoxLayout *main_layout = new QVBoxLayout(this);
  QWidget *widgets[] = {versionLbl, gitRemoteLbl, gitBranchLbl, lastUpdateLbl, updateBtn};
  for (int i = 0; i < std::size(widgets); ++i) {
    main_layout->addWidget(widgets[i]);
    if (i < std::size(widgets) - 1) {
      main_layout->addWidget(horizontal_line());
    }
  }

  main_layout->addWidget(new GitHash());

  main_layout->addWidget(horizontal_line());

  const char* git_reset = "/data/openpilot/git_reset.sh ''";
  auto gitresetbtn = new ButtonControl("Git Reset", "EXECUTE");
  QObject::connect(gitresetbtn, &ButtonControl::released, [=]() {
    if (ConfirmationDialog::confirm("After forcibly initializing local changes, the latest commit history of Remote Git is applied. Are you sure you want to proceed??", this)){
      std::system(git_reset);
    }
  });
  main_layout->addWidget(gitresetbtn);

  main_layout->addWidget(horizontal_line());

  const char* gitpull_cancel = "/data/openpilot/gitpull_cancel.sh ''";
  auto gitpullcanceltbtn = new ButtonControl("Git Pull Cancel", "EXECUTE");
  QObject::connect(gitpullcanceltbtn, &ButtonControl::released, [=]() {
    if (ConfirmationDialog::confirm("GitPull will be reverted to the previous state. Are you sure you want to proceed?", this)){
      std::system(gitpull_cancel);
    }
  });
  main_layout->addWidget(gitpullcanceltbtn);

  main_layout->addWidget(horizontal_line());

  const char* panda_flashing = "/data/openpilot/panda_flashing.sh ''";
  auto pandaflashingtbtn = new ButtonControl("Flash Panda", "CONFIRM");
  QObject::connect(pandaflashingtbtn, &ButtonControl::released, [=]() {
    if (ConfirmationDialog::confirm("When the panda flashing is in progress, the green LED of the panda blinks quickly and automatically reboots when completed. Never turn off the power of the device or disconnect it arbitrarily. Are  you sure you want to proceed?", this)) {
      std::system(panda_flashing);
    }
  });
  main_layout->addWidget(pandaflashingtbtn);

  setStyleSheet(R"(QLabel {font-size: 50px;})");

  fs_watch = new QFileSystemWatcher(this);
  QObject::connect(fs_watch, &QFileSystemWatcher::fileChanged, [=](const QString path) {
    int update_failed_count = params.get<int>("UpdateFailedCount").value_or(0);
    if (path.contains("UpdateFailedCount") && update_failed_count > 0) {
      lastUpdateLbl->setText("failed to fetch update");
      updateBtn->setText("CHECK");
      updateBtn->setEnabled(true);
    } else if (path.contains("LastUpdateTime")) {
      updateLabels();
    }
  });
}

void SoftwarePanel::showEvent(QShowEvent *event) {
  updateLabels();
}

void SoftwarePanel::updateLabels() {
  QString lastUpdate = "";
  QString tm = QString::fromStdString(params.get("LastUpdateTime").substr(0, 19));
  if (tm != "") {
    lastUpdate = timeAgo(QDateTime::fromString(tm, "yyyy-MM-dd HH:mm:ss"));
  }

  versionLbl->setText(getBrandVersion());
  lastUpdateLbl->setText(lastUpdate);
  updateBtn->setText("CHECK");
  updateBtn->setEnabled(true);
  gitRemoteLbl->setText(QString::fromStdString(params.get("GitRemote").substr(19)));
  gitBranchLbl->setText(QString::fromStdString(params.get("GitBranch")));
  gitCommitLbl->setText(QString::fromStdString(params.get("GitCommit")).left(10));
  osVersionLbl->setText(QString::fromStdString(Hardware::get_os_version()).trimmed());
}

QWidget * network_panel(QWidget * parent) {
#ifdef QCOM
  QWidget *w = new QWidget(parent);
  QVBoxLayout *layout = new QVBoxLayout(w);
  layout->setSpacing(30);

  layout->addWidget(new OpenpilotView());
  layout->addWidget(horizontal_line());
  // wifi + tethering buttons
  auto wifiBtn = new ButtonControl("WiFi Settings", "OPEN");
  QObject::connect(wifiBtn, &ButtonControl::released, [=]() { HardwareEon::launch_wifi(); });
  layout->addWidget(wifiBtn);
  layout->addWidget(horizontal_line());

  auto tetheringBtn = new ButtonControl("Tethering Settings", "OPEN");
  QObject::connect(tetheringBtn, &ButtonControl::released, [=]() { HardwareEon::launch_tethering(); });
  layout->addWidget(tetheringBtn);
  layout->addWidget(horizontal_line());

  layout->addWidget(new HotspotOnBootToggle());

  layout->addWidget(horizontal_line());

  // SSH key management
  layout->addWidget(new SshToggle());
  layout->addWidget(horizontal_line());
  layout->addWidget(new SshControl());
  layout->addWidget(horizontal_line());
  layout->addWidget(new SshLegacyToggle());

  layout->addStretch(1);
#else
  Networking *w = new Networking(parent);
#endif
  return w;
}

UserPanel::UserPanel(QWidget* parent) : QWidget(parent) {
  QVBoxLayout *layout = new QVBoxLayout(this);

  // OPKR
  layout->addWidget(new LabelControl("UI Settings", ""));
  layout->addWidget(new AutoShutdown());
  layout->addWidget(new ForceShutdown());
  //layout->addWidget(new AutoScreenDimmingToggle());
  layout->addWidget(new AutoScreenOff());
  layout->addWidget(new VolumeControl());
  layout->addWidget(new BrightnessControl());
  layout->addWidget(new GetoffAlertToggle());
  layout->addWidget(new BatteryChargingControlToggle());
  layout->addWidget(new ChargingMin());
  layout->addWidget(new ChargingMax());
  layout->addWidget(new FanSpeedGain());
  layout->addWidget(new DrivingRecordToggle());
  layout->addWidget(new RecordCount());
  layout->addWidget(new RecordQuality());
  const char* record_del = "rm -f /storage/emulated/0/videos/*";
  auto recorddelbtn = new ButtonControl("Delete All Recordings", "EXECUTE");
  QObject::connect(recorddelbtn, &ButtonControl::released, [=]() {
    if (ConfirmationDialog::confirm("Deletes all saved recorded files. Are you sure you want to proceed?", this)){
      std::system(record_del);
    }
  });
  layout->addWidget(recorddelbtn);
  const char* realdata_del = "rm -rf /storage/emulated/0/realdata/*";
  auto realdatadelbtn = new ButtonControl("Delete All Driving Logs", "EXECUTE");
  QObject::connect(realdatadelbtn, &ButtonControl::released, [=]() {
    if (ConfirmationDialog::confirm("Deletes all saved driving logs. Are you sure you want to proceed?", this)){
      std::system(realdata_del);
    }
  });
  layout->addWidget(realdatadelbtn);
  layout->addWidget(new MonitoringMode());
  layout->addWidget(new MonitorEyesThreshold());
  layout->addWidget(new NormalEyesThreshold());
  layout->addWidget(new BlinkThreshold());
  layout->addWidget(new ApksEnableToggle());
  layout->addWidget(new RunNaviOnBootToggle());

  layout->addWidget(horizontal_line());
  layout->addWidget(new LabelControl("Driving Settings", ""));
  layout->addWidget(new AutoResumeToggle());
  layout->addWidget(new VariableCruiseToggle());
  layout->addWidget(new VariableCruiseProfile());
  layout->addWidget(new CruisemodeSelInit());
  layout->addWidget(new LaneChangeSpeed());
  layout->addWidget(new LaneChangeDelay());
  layout->addWidget(new LCTimingFactorUD());
  layout->addWidget(new LCTimingFactor());
  layout->addWidget(new LeftCurvOffset());
  layout->addWidget(new RightCurvOffset());
  layout->addWidget(new BlindSpotDetectToggle());
  layout->addWidget(new MaxAngleLimit());
  layout->addWidget(new SteerAngleCorrection());
  layout->addWidget(new TurnSteeringDisableToggle());
  layout->addWidget(new CruiseOverMaxSpeedToggle());
  layout->addWidget(new SpeedLimitOffset());
  layout->addWidget(new CruiseGapAdjustToggle());
  layout->addWidget(new AutoEnabledToggle());
  layout->addWidget(new CruiseAutoResToggle());
  layout->addWidget(new RESChoice());
  layout->addWidget(new SteerWindDownToggle());
  layout->addWidget(new MadModeEnabledToggle());

  layout->addWidget(horizontal_line());
  layout->addWidget(new LabelControl("Developer", ""));
  layout->addWidget(new DebugUiOneToggle());
  layout->addWidget(new DebugUiTwoToggle());
  layout->addWidget(new LongLogToggle());
  layout->addWidget(new PrebuiltToggle());
  layout->addWidget(new FPTwoToggle());
  layout->addWidget(new LDWSToggle());
  layout->addWidget(new GearDToggle());
  layout->addWidget(new ComIssueToggle());
  layout->addWidget(new WhitePandaSupportToggle());
  layout->addWidget(new SteerWarningFixToggle());
  layout->addWidget(new BattLessToggle());
  const char* cal_ok = "cp -f /data/openpilot/selfdrive/assets/addon/param/CalibrationParams /data/params/d/";
  auto calokbtn = new ButtonControl("Enable Force Calibration", "EXECUTE");
  QObject::connect(calokbtn, &ButtonControl::released, [=]() {
    if (ConfirmationDialog::confirm("Force calibration. It is for checking engagement, so please initialize it during actual driving.", this)){
      std::system(cal_ok);
    }
  });
  layout->addWidget(calokbtn);
  layout->addWidget(horizontal_line());
  layout->addWidget(new CarRecognition());
  //layout->addWidget(new CarForceSet());
  //QString car_model = QString::fromStdString(Params().get("CarModel", false));
  //layout->addWidget(new LabelControl("현재차량모델", ""));
  //layout->addWidget(new LabelControl(car_model, ""));

  layout->addWidget(horizontal_line());
  layout->addWidget(new LabelControl("Panda Values", "CAUTION"));
  layout->addWidget(new MaxSteer());
  layout->addWidget(new MaxRTDelta());
  layout->addWidget(new MaxRateUp());
  layout->addWidget(new MaxRateDown());
  const char* p_edit_go = "/data/openpilot/p_edit.sh ''";
  auto peditbtn = new ButtonControl("Apply Panda Value Change", "EXECUTE");
  QObject::connect(peditbtn, &ButtonControl::released, [=]() {
    if (ConfirmationDialog::confirm("Apply the changed pandas value. Are you sure you want to proceed? The device will automatically reboot.", this)){
      std::system(p_edit_go);
    }
  });
  layout->addWidget(peditbtn);
}

TuningPanel::TuningPanel(QWidget* parent) : QWidget(parent) {
  QVBoxLayout *layout = new QVBoxLayout(this);

  // OPKR
  layout->addWidget(new LabelControl("Tuning Menu", ""));
  layout->addWidget(new CameraOffset());
  layout->addWidget(new LiveSteerRatioToggle());
  layout->addWidget(new SRBaseControl());
  layout->addWidget(new SRMaxControl());
  layout->addWidget(new SteerActuatorDelay());
  layout->addWidget(new SteerRateCost());
  layout->addWidget(new SteerLimitTimer());
  layout->addWidget(new TireStiffnessFactor());
  layout->addWidget(new SteerMaxBase());
  layout->addWidget(new SteerMaxMax());
  layout->addWidget(new SteerMaxv());
  layout->addWidget(new VariableSteerMaxToggle());
  layout->addWidget(new SteerDeltaUpBase());
  layout->addWidget(new SteerDeltaUpMax());
  layout->addWidget(new SteerDeltaDownBase());
  layout->addWidget(new SteerDeltaDownMax());
  layout->addWidget(new VariableSteerDeltaToggle());
  layout->addWidget(new SteerThreshold());

  layout->addWidget(horizontal_line());

  layout->addWidget(new LabelControl("Control Menu", ""));
  layout->addWidget(new LateralControl());
  layout->addWidget(new LiveTuneToggle());
  QString lat_control = QString::fromStdString(Params().get("LateralControlMethod", false));
  if (lat_control == "0") {
    layout->addWidget(new PidKp());
    layout->addWidget(new PidKi());
    layout->addWidget(new PidKd());
    layout->addWidget(new PidKf());
    layout->addWidget(new IgnoreZone());
    layout->addWidget(new ShaneFeedForward());
  } else if (lat_control == "1") {
    layout->addWidget(new InnerLoopGain());
    layout->addWidget(new OuterLoopGain());
    layout->addWidget(new TimeConstant());
    layout->addWidget(new ActuatorEffectiveness());
  } else if (lat_control == "2") {
    layout->addWidget(new Scale());
    layout->addWidget(new LqrKi());
    layout->addWidget(new DcGain());
  }

  layout->addWidget(horizontal_line());

  layout->addWidget(new LabelControl("Longitudinal Tuning Menu", ""));
  layout->addWidget(new DynamicTR());
  layout->addWidget(new CruiseGapTR());
}

void SettingsWindow::showEvent(QShowEvent *event) {
  panel_widget->setCurrentIndex(0);
  nav_btns->buttons()[0]->setChecked(true);
}

SettingsWindow::SettingsWindow(QWidget *parent) : QFrame(parent) {

  // setup two main layouts
  sidebar_widget = new QWidget;
  QVBoxLayout *sidebar_layout = new QVBoxLayout(sidebar_widget);
  sidebar_layout->setMargin(0);
  panel_widget = new QStackedWidget();
  panel_widget->setStyleSheet(R"(
    border-radius: 30px;
    background-color: #292929;
  )");

  // close button
  QPushButton *close_btn = new QPushButton("◀");
  close_btn->setStyleSheet(R"(
    font-size: 60px;
    font-weight: bold;
    border 1px grey solid;
    border-radius: 100px;
    background-color: #292929;
  )");
  close_btn->setFixedSize(200, 200);
  sidebar_layout->addSpacing(45);
  sidebar_layout->addWidget(close_btn, 0, Qt::AlignCenter);
  QObject::connect(close_btn, &QPushButton::released, this, &SettingsWindow::closeSettings);

  // setup panels
  DevicePanel *device = new DevicePanel(this);
  QObject::connect(device, &DevicePanel::reviewTrainingGuide, this, &SettingsWindow::reviewTrainingGuide);
  QObject::connect(device, &DevicePanel::showDriverView, this, &SettingsWindow::showDriverView);

  QList<QPair<QString, QWidget *>> panels = {
    {"Device", device},
    {"Network", network_panel(this)},
    {"Toggles", new TogglesPanel(this)},
    {"Software", new SoftwarePanel(this)},
    {"Developer", new UserPanel(this)},
    {"Tuning", new TuningPanel(this)},
  };

  sidebar_layout->addSpacing(45);

#ifdef ENABLE_MAPS
  if (!Params().get("MapboxToken").empty()) {
    auto map_panel = new MapPanel(this);
    panels.push_back({"Navigation", map_panel});
    QObject::connect(map_panel, &MapPanel::closeSettings, this, &SettingsWindow::closeSettings);
  }
#endif
  const int padding = panels.size() > 3 ? 18 : 28;

  nav_btns = new QButtonGroup();
  for (auto &[name, panel] : panels) {
    QPushButton *btn = new QPushButton(name);
    btn->setCheckable(true);
    btn->setChecked(nav_btns->buttons().size() == 0);
    btn->setStyleSheet(QString(R"(
      QPushButton {
        color: grey;
        border: none;
        background: none;
        font-size: 65px;
        font-weight: 500;
        padding-top: %1px;
        padding-bottom: %1px;
      }
      QPushButton:checked {
        color: white;
      }
    )").arg(padding));

    nav_btns->addButton(btn);
    sidebar_layout->addWidget(btn, 0, Qt::AlignRight);

    panel->setContentsMargins(50, 25, 50, 25);

    ScrollView *panel_frame = new ScrollView(panel, this);
    panel_widget->addWidget(panel_frame);

    QObject::connect(btn, &QPushButton::released, [=, w = panel_frame]() {
      btn->setChecked(true);
      panel_widget->setCurrentWidget(w);
    });
  }
  sidebar_layout->setContentsMargins(50, 50, 100, 50);

  // main settings layout, sidebar + main panel
  QHBoxLayout *main_layout = new QHBoxLayout(this);

  sidebar_widget->setFixedWidth(500);
  main_layout->addWidget(sidebar_widget);
  main_layout->addWidget(panel_widget);

  setStyleSheet(R"(
    * {
      color: white;
      font-size: 50px;
    }
    SettingsWindow {
      background-color: black;
    }
  )");
}

void SettingsWindow::hideEvent(QHideEvent *event) {
#ifdef QCOM
  HardwareEon::close_activities();
#endif
}
