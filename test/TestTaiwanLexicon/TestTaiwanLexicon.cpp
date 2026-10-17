// Isolated real-librime test: dictionary packs must work with plain and personal dictionaries.
#include <rime_api.h>
#include <cstdio>
#include <string>
#include "../../core/settings/taiwan_lexicon.h"

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  auto api = rime_get_api();
  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = argv[1];
  traits.prebuilt_data_dir = argv[1];
  traits.user_data_dir = argv[2];
  traits.app_name = "rime.test_taiwan_lexicon";
  api->setup(&traits);
  namespace tw = settings::taiwan_lexicon;
  namespace fs = std::filesystem;
  const fs::path user = argv[2], shared = argv[1];
  int failures = 0;
  std::string error;
  const auto check = [&](bool ok, const char* name) {
    if (!ok) { ++failures; std::printf("FAIL %s (%s)\n", name, error.c_str()); }
  };
  const auto personal = tw::Read(user / "terra_pinyin.personal.dict.yaml");
  const auto custom = tw::Read(user / "bopomofo_express.custom.yaml");
  bool changed = false;
  check(tw::Prepare(api, shared, user, &error, &changed), "native first deployment");
  check(changed, "first deployment requests maintenance");
  if (!error.empty()) std::printf("prepare error: %s\n", error.c_str());
  check(tw::Read(user / "terra_pinyin.personal.dict.yaml") == personal, "personal dictionary preserved");
  check(tw::Read(user / "bopomofo_express.custom.yaml") == custom, "child settings preserved");
  const auto base = tw::Read(user / "bopomofo.custom.yaml");
  check(tw::Prepare(api, shared, user, &error, &changed), "idempotent deployment");
  check(!changed, "unchanged startup does not force maintenance");
  check(tw::Read(user / "bopomofo.custom.yaml") == base, "idempotent patch");
  const std::string original = "\xef\xbb\xbf# custom\r\npatch:\r\n  menu/page_size: 7\r\n";
  const auto patched = tw::Patch(api, original, true, false);
  check(tw::Patch(api, patched, false, false) == original, "BOM/comments/CRLF preserved");
  // The opt-out must survive an upgrade/redeployment, without editing personal data.
  tw::Write(user / "wisdom_taiwan.user.yaml", "enabled: false\n");
  check(tw::Prepare(api, shared, user, &error, &changed), "native opt-out");
  check(changed, "opt-out requests maintenance");
  check(tw::Read(user / "bopomofo.custom.yaml").find(tw::kBegin) == std::string::npos, "pack detached");
  check(tw::Prepare(api, shared, user, &error), "opt-out retained on upgrade");
  check(tw::Read(user / "bopomofo.custom.yaml").find(tw::kBegin) == std::string::npos, "upgrade did not re-enable");
  tw::Write(user / "wisdom_taiwan.user.yaml", "enabled: true\n");
  tw::Write(user / "wisdom_taiwan.dict.yaml", "# obsolete packaged vocabulary\n");
  check(tw::Prepare(api, shared, user, &error, &changed), "pack update");
  check(changed, "vocabulary update requests maintenance");
  check(tw::Read(user / "wisdom_taiwan.dict.yaml") == tw::Read(shared / "wisdom_taiwan.dict.yaml"), "updated source");
  check(tw::Read(user / "terra_pinyin.personal.dict.yaml") == personal, "update preserved personal dictionary");
  const auto conflict = user / "conflict";
  fs::create_directory(conflict);
  tw::Write(conflict / "bopomofo.custom.yaml", "patch: {menu/page_size: 7}\n");
  check(!tw::Prepare(api, shared, conflict, &error, &changed), "conflicting inline patch rejected");
  check(!changed, "failed preparation does not report changes");
  check(!fs::exists(conflict / "wisdom_taiwan.dict.yaml"), "failed preflight did not write files");
  check(tw::Read(conflict / "bopomofo.custom.yaml") == "patch: {menu/page_size: 7}\n", "conflict preserved");
  tw::Write(conflict / "bopomofo.custom.yaml", "\"patch\": {menu/page_size: 7}\n");
  check(!tw::Prepare(api, shared, conflict, &error), "quoted inline patch preserved");
  if (failures) return 1;
  api->initialize(nullptr);
  if (api->start_maintenance(True)) api->join_maintenance_thread();
  const auto session = api->create_session();
  const char* schemas[] = {"bopomofo", "bopomofo_express", "bopomofo_tw"};
  struct Case { const char* text; const char* keys; };
  const Case cases[] = {
    {u8"台積電", "w96ru 2u04"}, {u8"悠遊卡", "u. u.6d83"},
    {u8"健保卡", "ru041l3d83"}, {u8"台灣高鐵", "w96j0 el wu,3"},
    {u8"電子發票", "2u04y3z8 qul4"}, {u8"行動支付", "vu/62j/45 zj4"},
  };
  for (const auto schema : schemas) {
    if (!api->select_schema(session, schema)) { ++failures; continue; }
    RimeConfig config = {0};
    if (!api->schema_open(schema, &config)) {
      ++failures;
    } else {
      if (api->config_list_size(&config, "translator/packs") != 1) {
        std::printf("%s duplicated/missing pack FAIL\n", schema);
        ++failures;
      }
      api->config_close(&config);
    }
    api->set_option(session, "ascii_mode", False);
    api->set_option(session, "zh_hant", True);
    api->set_option(session, "zh_hant_tw", False);
    for (const auto& item : cases) {
      api->clear_composition(session);
      api->set_input(session, item.keys);
      RimeCandidateListIterator iter = {0};
      bool found = false;
      int rank = 0;
      if (api->candidate_list_begin(session, &iter)) {
        while (rank < 200 && api->candidate_list_next(&iter)) {
          ++rank;
          if (iter.candidate.text && std::string(iter.candidate.text) == item.text) {
            found = true;
            break;
          }
        }
        api->candidate_list_end(&iter);
      }
      std::printf("%s %s rank=%d %s\n", schema, item.text, rank, found ? "OK" : "FAIL");
      if (!found) ++failures;
    }
  }
  api->destroy_session(session);
  api->finalize();
  std::printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
