// Isolated real-librime test: dictionary packs must work with plain and
// personal dictionaries.
#include <rime_api.h>
#include <cstdio>
#include <string>
#include "../../core/settings/taiwan_lexicon.h"

int main(int argc, char** argv) {
  if (argc != 3)
    return 2;
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
    if (!ok) {
      ++failures;
      std::printf("FAIL %s (%s)\n", name, error.c_str());
    }
  };
  const auto personal = tw::Read(user / "terra_pinyin.personal.dict.yaml");
  const auto custom = tw::Read(user / "bopomofo_express.custom.yaml");
  bool changed = false;
  check(tw::Prepare(api, shared, user, &error, &changed),
        "native first deployment");
  check(changed, "first deployment requests maintenance");
  if (!error.empty())
    std::printf("prepare error: %s\n", error.c_str());
  check(tw::Read(user / "terra_pinyin.personal.dict.yaml") == personal,
        "personal dictionary preserved");
  check(tw::Read(user / "bopomofo_express.custom.yaml") == custom,
        "child settings preserved");
  const auto base = tw::Read(user / "bopomofo.custom.yaml");
  check(tw::Prepare(api, shared, user, &error, &changed),
        "idempotent deployment");
  check(!changed, "unchanged startup does not force maintenance");
  check(tw::Read(user / "bopomofo.custom.yaml") == base, "idempotent patch");
  const std::string original =
      "\xef\xbb\xbf# custom\r\npatch:\r\n  menu/page_size: 7\r\n";
  const auto patched = tw::Patch(api, original, true, false);
  check(tw::Patch(api, patched, false, false) == original,
        "BOM/comments/CRLF preserved");
  // The opt-out must survive an upgrade/redeployment, without editing personal
  // data.
  tw::Write(user / "wisdom_taiwan.user.yaml", "enabled: false\n");
  check(tw::Prepare(api, shared, user, &error, &changed), "native opt-out");
  check(changed, "opt-out requests maintenance");
  check(tw::Read(user / "bopomofo.custom.yaml").find(tw::kBegin) ==
            std::string::npos,
        "pack detached");
  check(tw::Prepare(api, shared, user, &error), "opt-out retained on upgrade");
  check(tw::Read(user / "bopomofo.custom.yaml").find(tw::kBegin) ==
            std::string::npos,
        "upgrade did not re-enable");
  tw::Write(user / "wisdom_taiwan.user.yaml", "enabled: true\n");
  tw::Write(user / "wisdom_taiwan.dict.yaml",
            "# obsolete packaged vocabulary\n");
  check(tw::Prepare(api, shared, user, &error, &changed), "pack update");
  check(changed, "vocabulary update requests maintenance");
  check(tw::Read(user / "wisdom_taiwan.dict.yaml") ==
            tw::Read(shared / "wisdom_taiwan.dict.yaml"),
        "updated source");
  check(tw::Read(user / "terra_pinyin.personal.dict.yaml") == personal,
        "update preserved personal dictionary");
  const auto conflict = user / "conflict";
  fs::create_directory(conflict);
  tw::Write(conflict / "bopomofo.custom.yaml", "patch: {menu/page_size: 7}\n");
  check(!tw::Prepare(api, shared, conflict, &error, &changed),
        "conflicting inline patch rejected");
  check(!changed, "failed preparation does not report changes");
  check(!fs::exists(conflict / "wisdom_taiwan.dict.yaml"),
        "failed preflight did not write files");
  check(tw::Read(conflict / "bopomofo.custom.yaml") ==
            "patch: {menu/page_size: 7}\n",
        "conflict preserved");
  tw::Write(conflict / "bopomofo.custom.yaml",
            "\"patch\": {menu/page_size: 7}\n");
  check(!tw::Prepare(api, shared, conflict, &error),
        "quoted inline patch preserved");
  if (failures)
    return 1;
  api->initialize(nullptr);
  if (api->start_maintenance(True))
    api->join_maintenance_thread();
  const auto session = api->create_session();
  const char* schemas[] = {"bopomofo", "bopomofo_express", "bopomofo_tw"};
  struct Case {
    const char* text;
    const char* keys;
    int max_rank = 1;
  };
  const Case cases[] = {
      {u8"台積電", "w96ru 2u04"},      {u8"悠遊卡", "u. u.6d83"},
      {u8"健保卡", "ru041l3d83"},      {u8"台灣高鐵", "w96j0 el wu,3"},
      {u8"電子發票", "2u04y3z8 qul4"}, {u8"行動支付", "vu/62j/45 zj4"},
      {u8"設籍", "gk4ru6", 5},         {u8"爆表", "1l41ul3", 5},
      {u8"發想", "z8 vu;3", 5},        {u8"新檔", "vup 2;3", 5},
  };
  for (const auto schema : schemas) {
    if (!api->select_schema(session, schema)) {
      ++failures;
      continue;
    }
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
          if (iter.candidate.text &&
              std::string(iter.candidate.text) == item.text) {
            found = true;
            break;
          }
        }
        api->candidate_list_end(&iter);
      }
      std::printf("%s %s rank=%d %s\n", schema, item.text, rank,
                  found ? "OK" : "FAIL");
      if (!found || rank > item.max_rank)
        ++failures;
    }
  }
  api->destroy_session(session);

  // --- Test reading validation, personal edits, migration backup, conflicts &
  // import/export ---
  tw::SyllableTable syl_table;
  check(tw::LoadSyllableTable(shared, shared, &syl_table),
        "load syllable table");
  {
    std::string w, c, zh, err;
    check(tw::ParseAndValidateReading(u8"發想", u8"ㄈㄚ ㄒㄧㄤˇ", syl_table, &w,
                                      &c, &zh, &err) &&
              c == "fa1 xiang3" && zh == u8"ㄈㄚ ㄒㄧㄤˇ",
          "validate spaced zhuyin");
    check(tw::ParseAndValidateReading(u8"發想", u8"ㄈㄚㄒㄧㄤˇ", syl_table, &w,
                                      &c, &zh, &err) &&
              c == "fa1 xiang3",
          "validate unspaced zhuyin");
    check(tw::ParseAndValidateReading(u8"發想", "fa1 xiang3", syl_table, &w, &c,
                                      &zh, &err) &&
              zh == u8"ㄈㄚ ㄒㄧㄤˇ",
          "validate pinyin tone code");
    check(!tw::ParseAndValidateReading(u8"發想", u8"ㄈㄚ", syl_table, &w, &c,
                                       &zh, &err),
          "reject mismatched syllable count");
    check(!tw::ParseAndValidateReading(u8"發想\t注入", u8"ㄈㄚ ㄒㄧㄤˇ",
                                       syl_table, &w, &c, &zh, &err),
          "reject tab injection in word");
    check(!tw::ParseAndValidateReading(u8"發想", u8"ㄈㄚ\nㄒㄧㄤˇ", syl_table,
                                       &w, &c, &zh, &err),
          "reject newline injection in reading");
    check(!tw::ParseAndValidateReading(u8"一二三四五六七八九十甲乙丙", u8"ㄧ",
                                       syl_table, &w, &c, &zh, &err),
          "reject word exceeding max length 12");
  }

  // Test v0 -> v1 migration backup
  {
    tw::BaseLexicon base_lex;
    check(tw::ParseDictText(tw::Read(shared / "wisdom_taiwan.dict.yaml"),
                            &base_lex, &error),
          "parse base lexicon");

    const std::string v0_text =
        "# Bundled Taiwan vocabulary; set false to disable.\nenabled: true\n";
    tw::Write(user / "wisdom_taiwan.user.yaml", v0_text);
    tw::UserConfig cfg;
    cfg.version = 1;
    cfg.enabled = true;
    cfg.edits_revision = 1;
    cfg.deployed_base_version = base_lex.version;
    cfg.deployed_edits_revision = 1;
    // 1) User adds custom word 智鼬 (zhi4 you4)
    cfg.edits.push_back(
        {u8"智鼬", "zhi4 you4", false, "high", false, 1000, "", ""});
    // 2) User hides built-in 設籍 (she4 ji2)
    cfg.edits.push_back(
        {u8"設籍", "she4 ji2", true, "normal", true, 1000, "", ""});
    // 3) Upstream-removed item (origin_base=true, not in base): must NOT be
    // silently re-added
    cfg.edits.push_back({u8"已移除舊詞", "yi3 yi2 chu2 jiu4 ci2", false,
                         "normal", true, 1000, "", ""});
    // 4) User-added item that also exists in upstream (發想 fa1 xiang3):
    // deduplicated, not summed
    cfg.edits.push_back(
        {u8"發想", "fa1 xiang3", false, "high", false, 1000, "", ""});

    check(tw::SaveUserConfigWithMigrationBackup(api, user, cfg, &error),
          "save v1 config with migration backup");
    check(fs::exists(user / "wisdom_taiwan.user.yaml.v0.bak") &&
              tw::Read(user / "wisdom_taiwan.user.yaml.v0.bak") == v0_text,
          "v0 migration backup created and matches original");

    bool edit_changed = false;
    check(tw::Prepare(api, shared, user, &error, &edit_changed),
          "prepare with personal edits");
    check(edit_changed, "personal edits trigger update");
    const std::string eff_dict = tw::Read(user / "wisdom_taiwan.dict.yaml");
    check(eff_dict.find("version: '" + base_lex.version + ".u1'") !=
              std::string::npos,
          "effective dict version includes edit revision");
    check(eff_dict.find(u8"智鼬\tzhi4 you4\t10000\n") != std::string::npos,
          "added word present with high weight");
    check(eff_dict.find(u8"設籍\tshe4 ji2\t") == std::string::npos,
          "hidden word excluded from effective dict");
    check(eff_dict.find(u8"已移除舊詞") == std::string::npos,
          "upstream-removed modified item not silently re-added");

    // Verify deduplication of 發想\tfa1 xiang3
    size_t fa_count = 0;
    size_t pos = 0;
    while ((pos = eff_dict.find(u8"\n發想\tfa1 xiang3\t", pos)) !=
           std::string::npos) {
      ++fa_count;
      pos += 10;
    }
    check(fa_count == 1,
          "duplicate user-added + upstream entry appears exactly once");

    // Test export and import preview
    const std::string exported =
        tw::ExportEditsJson(base_lex.version, cfg.edits, "2026/10/17 20:00");
    tw::json preview;
    check(tw::PreviewImportEdits(exported, base_lex, syl_table, {}, &preview,
                                 &error),
          "preview import edits");
    check(preview["conflicts"].size() == 1 &&
              preview["conflicts"][0]["word"].get<std::string>() ==
                  u8"已移除舊詞",
          "preview flags upstream-removed item as conflict");
    check(!tw::PreviewImportEdits("# Rime userdb snapshot\n", base_lex,
                                  syl_table, {}, &preview, &error),
          "preview rejects non-json / userdb snapshot");
  }

  api->finalize();
  std::printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
