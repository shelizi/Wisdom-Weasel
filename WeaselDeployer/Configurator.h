#pragma once

class UIStyleSettings;

class Configurator {
 public:
  explicit Configurator();

  void Initialize();
  int Run(bool installing, int start_page = 0);
  int UpdateWorkspace(bool report_errors = false);
  int DictManagement();
  int LegacyDictManagement();
  int SyncUserData();
};
