#pragma once

class Configurator {
 public:
  explicit Configurator();

  void Initialize();
  // 安裝時要不要開設定視窗：輸入方案與外觀都還沒設定過（第一次安裝）
  bool IsFirstRun();
  int UpdateWorkspace(bool report_errors = false);
  int SyncUserData();
  // 這次執行有沒有重新部署過（安裝時沒部署就補一次）
  bool deployed() const { return deployed_; }

 private:
  bool deployed_ = false;
};
