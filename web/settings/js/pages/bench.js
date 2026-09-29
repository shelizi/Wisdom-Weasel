import { h, section, card, checkbox, button, buttons, textarea, note } from '../ui.js';
import { get, int, profileLabel } from '../state.js';

// 題目：預測「前文|期望的詞」；校正「前文|注音|初稿|正確的句子」（注音每個音節以空格或聲調斷開）
const kDefaultPredict = [
  '# 聊天與客套話',
  '謝謝你的|幫忙',
  '不好意思，打擾|一下',
  '祝你生日|快樂',
  '新年|快樂',
  '路上小|心',
  '早安，今天也要加|油',
  '明天見，晚|安',
  '有空的話回我一下，謝|謝',
  '你今天辛苦|了',
  '不好意思，讓你久|等',
  '期待下次再|見',
  '祝你一路順|風',
  '我們下次再約時間吃|飯',
  '# 生活',
  '請問這個多少|錢',
  '天氣預報說明天會下|雨',
  '下雨了，出門記得帶雨|傘',
  '我先去吃|飯',
  '我的手機沒電了，可以借我充|電',
  '這家餐廳的牛肉麵很好|吃',
  '他每天早上都會去公園跑|步',
  '請問最近的捷運|站',
  '我感冒了，要去看醫|生',
  '# 工作與學校',
  '會議開始前請先把手機調成|靜音',
  '不好意思，我今天身體不舒服，想請|假',
  '這個問題我需要再想一|下',
  '報告寫好了，我等一下寄給|你',
  '老師說明天要考|試',
  '# 常識與成語',
  '一石二|鳥',
  '台灣最高的山是玉|山',
].join('\n');
const kDefaultCorrect = [
  '# 聊天',
  '|ㄐㄧㄣ ㄊㄧㄢ ㄊㄧㄢ ㄑㄧˋ ㄏㄣˇ ㄏㄠˇ|今天天汽很好|今天天氣很好',
  '|ㄒㄧㄝˋ ㄒㄧㄝ˙ ㄋㄧˇ ㄉㄜ˙ ㄅㄤ ㄇㄤˊ|謝謝你的邦忙|謝謝你的幫忙',
  '|ㄨㄛˇ ㄇㄣ˙ ㄒㄧㄚˋ ㄘˋ ㄗㄞˋ ㄐㄧㄢˋ|我們下次在見|我們下次再見',
  '明天早上有空嗎？|ㄨㄛˇ ㄗㄞˋ ㄒㄧㄤˇ ㄒㄧㄤˇ ㄎㄢˋ|我在想想看|我再想想看',
  '你到了嗎？|ㄨㄛˇ ㄧˇ ㄐㄧㄥ ㄗㄞˋ ㄇㄣˊ ㄎㄡˇ ㄌㄜ˙|我已經在門口了|我已經在門口了',
  '晚餐要吃什麼？|ㄨㄛˇ ㄒㄧㄤˇ ㄔ ㄋㄧㄡˊ ㄖㄡˋ ㄌㄚ ㄇㄧㄢˋ|我想吃牛肉拉面|我想吃牛肉拉麵',
  '他怎麼還沒來？|ㄊㄚ ㄕㄨㄛ ㄙㄞ ㄔㄜ ㄏㄨㄟˋ ㄨㄢˇ ㄧ ㄉㄧㄢˇ ㄉㄠˋ|他說塞車會晚一點到|他說塞車會晚一點到',
  '你週末要做什麼？|ㄨㄛˇ ㄧㄠˋ ㄏㄨㄟˊ ㄐㄧㄚ ㄎㄢˋ ㄅㄚˋ ㄇㄚ|我要回家看爸媽|我要回家看爸媽',
  '你在哪裡？|ㄨㄛˇ ㄗㄞˋ ㄍㄨㄥ ㄙ ㄐㄧㄚ ㄅㄢ|我再公司加班|我在公司加班',
  '|ㄋㄧˇ ㄐㄧㄣ ㄊㄧㄢ ㄧㄡˇ ㄎㄨㄥˋ ㄇㄚ˙|你今天有空嗎|你今天有空嗎',
  '|ㄊㄚ ㄕˋ ㄨㄛˇ ㄉㄜ˙ ㄏㄠˇ ㄆㄥˊ ㄧㄡˇ|他是我的好朋有|他是我的好朋友',
  '|ㄨㄛˇ ㄉㄥˇ ㄧ ㄒㄧㄚˋ ㄗㄞˋ ㄉㄚˇ ㄍㄟˇ ㄋㄧˇ|我等一下在打給你|我等一下再打給你',
  '# 工作',
  '|ㄑㄧㄥˇ ㄉㄚˋ ㄐㄧㄚ ㄓㄨㄣˇ ㄕˊ ㄔㄨ ㄒㄧˊ ㄏㄨㄟˋ ㄧˋ|請大家準時出席惠議|請大家準時出席會議',
  '會議紀錄如附件，|ㄖㄨˊ ㄧㄡˇ ㄨㄣˋ ㄊㄧˊ ㄑㄧㄥˇ ㄙㄨㄟˊ ㄕˊ ㄌㄧㄢˊ ㄌㄨㄛˋ|如有問題請隨時聯洛|如有問題請隨時聯絡',
  '主管說|ㄓㄜˋ ㄍㄜ˙ ㄢˋ ㄗ˙ ㄧㄠˋ ㄐㄧㄣˇ ㄎㄨㄞˋ ㄔㄨˇ ㄌㄧˇ|這個案子要盡快處裡|這個案子要盡快處理',
  '請確認|ㄈㄨˋ ㄐㄧㄢˋ ㄉㄜ˙ ㄉㄤˇ ㄢˋ ㄕˋ ㄈㄡˇ ㄓㄥˋ ㄑㄩㄝˋ|附件的檔案是否正卻|附件的檔案是否正確',
  '麻煩你|ㄅㄤ ㄨㄛˇ ㄩˋ ㄉㄧㄥˋ ㄏㄨㄟˋ ㄧˋ ㄕˋ|幫我預定會議是|幫我預定會議室',
  '關於下週的專案，|ㄑㄧㄥˇ ㄉㄚˋ ㄐㄧㄚ ㄗㄞˋ ㄒㄧㄥ ㄑㄧˊ ㄨˇ ㄑㄧㄢˊ ㄐㄧㄠ ㄅㄠˋ ㄍㄠˋ|請大家在星期五前交報告|請大家在星期五前交報告',
  '客戶那邊回覆了，|ㄊㄚ ㄇㄣ˙ ㄒㄧ ㄨㄤˋ ㄊㄧˊ ㄗㄠˇ ㄐㄧㄠ ㄏㄨㄛˋ|他們希望提早交貨|他們希望提早交貨',
  '明天下午兩點|ㄗㄞˋ ㄙㄢ ㄌㄡˊ ㄏㄨㄟˋ ㄧˋ ㄕˋ ㄎㄞ ㄏㄨㄟˋ|在三樓會議室開會|在三樓會議室開會',
  '|ㄓㄜˋ ㄈㄣˋ ㄅㄠˋ ㄍㄠˋ ㄒㄩ ㄧㄠˋ ㄗㄞˋ ㄒㄧㄡ ㄍㄞˇ ㄧ ㄒㄧㄚˋ|這份報告需要再修改一下|這份報告需要再修改一下',
  '|ㄏㄨㄟˋ ㄧˋ ㄍㄞˇ ㄉㄠˋ ㄒㄧㄚˋ ㄌㄧˇ ㄅㄞˋ ㄙㄢ|會議改到下禮拜三|會議改到下禮拜三',
  '|ㄑㄧㄥˇ ㄅㄚˇ ㄉㄤˇ ㄢˋ ㄐㄧˋ ㄍㄟˇ ㄨㄛˇ|請把檔案寄給我|請把檔案寄給我',
  '|ㄐㄧㄣ ㄊㄧㄢ ㄉㄜ˙ ㄍㄨㄥ ㄗㄨㄛˋ ㄏㄣˇ ㄉㄨㄛ|今天的工做很多|今天的工作很多',
  '|ㄑㄧㄥˇ ㄗㄞˋ ㄓㄜˋ ㄌㄧˇ ㄑㄧㄢ ㄇㄧㄥˊ|請在這裡簽明|請在這裡簽名',
  '# 生活與購物',
  '|ㄨㄛˇ ㄧˇ ㄐㄧㄥ ㄔ ㄍㄨㄛˋ ㄨˇ ㄘㄢ ㄌㄜ˙|我已經吃過午參了|我已經吃過午餐了',
  '|ㄨㄛˇ ㄒㄧㄤˇ ㄏㄜ ㄧ ㄅㄟ ㄍㄚ ㄈㄟ|我想喝一杯咖非|我想喝一杯咖啡',
  '|ㄨㄛˇ ㄒㄧㄤˇ ㄇㄞˇ ㄧ ㄓ ㄒㄧㄣ ㄕㄡˇ ㄐㄧ|我想買一隻新手機|我想買一支新手機',
  '他的身體狀況很好，|ㄅㄨˋ ㄩㄥˋ ㄉㄢ ㄒㄧㄣ|不用擔新|不用擔心',
  '藥局在哪裡？|ㄗㄞˋ ㄐㄧㄝˊ ㄩㄣˋ ㄓㄢˋ ㄆㄤˊ ㄅㄧㄢ|在捷運站旁邊|在捷運站旁邊',
  '我感冒了，|ㄧㄠˋ ㄑㄩˋ ㄎㄢˋ ㄧ ㄕㄥ ㄋㄚˊ ㄧㄠˋ|要去看醫生拿藥|要去看醫生拿藥',
  '最近天氣變冷了，|ㄐㄧˋ ㄉㄜ˙ ㄉㄨㄛ ㄔㄨㄢ ㄧ ㄐㄧㄢˋ ㄨㄞˋ ㄊㄠˋ|記得多穿一件外套|記得多穿一件外套',
  '結帳的時候|ㄎㄜˇ ㄧˇ ㄩㄥˋ ㄒㄧㄣˋ ㄩㄥˋ ㄎㄚˇ ㄇㄚ˙|可以用信用卡嗎|可以用信用卡嗎',
  '|ㄓㄜˋ ㄐㄧㄚ ㄘㄢ ㄊㄧㄥ ㄉㄜ˙ ㄉㄨㄥ ㄒㄧ ㄏㄣˇ ㄏㄠˇ ㄔ|這家餐廳的東西很好吃|這家餐廳的東西很好吃',
  '|ㄒㄧㄠˇ ㄒㄧㄣ ㄅㄨˊ ㄧㄠˋ ㄍㄢˇ ㄇㄠˋ ㄌㄜ˙|小心不要感冒了|小心不要感冒了',
  '# 電腦與技術',
  '程式跑不起來，|ㄎㄜˇ ㄋㄥˊ ㄕˋ ㄕㄜˋ ㄉㄧㄥˋ ㄉㄤˇ ㄒㄧㄝˇ ㄘㄨㄛˋ ㄌㄜ˙|可能是設定黨寫錯了|可能是設定檔寫錯了',
  '網路很慢，|ㄎㄜˇ ㄋㄥˊ ㄕˋ ㄌㄨˋ ㄧㄡˊ ㄑㄧˋ ㄉㄜ˙ ㄨㄣˋ ㄊㄧˊ|可能是路由器的問提|可能是路由器的問題',
  '他的電腦壞了，|ㄧㄠˋ ㄋㄚˊ ㄑㄩˋ ㄒㄧㄡ ㄌㄧˇ|要拿去修里|要拿去修理',
  '|ㄑㄧㄥˇ ㄒㄧㄢ ㄔㄨㄥˊ ㄒㄧㄣ ㄑㄧˇ ㄉㄨㄥˋ ㄉㄧㄢˋ ㄋㄠˇ ㄗㄞˋ ㄕˋ ㄕˋ ㄎㄢˋ|請先重新啟動電腦再試試看|請先重新啟動電腦再試試看',
  '|ㄐㄧˋ ㄉㄜ˙ ㄉㄧㄥˋ ㄑㄧˊ ㄅㄟˋ ㄈㄣˋ ㄗ ㄌㄧㄠˋ|記得定期備份資料|記得定期備份資料',
  '|ㄓㄜˋ ㄧˊ ㄉㄧㄢˋ ㄋㄠˇ ㄉㄜ˙ ㄙㄨˋ ㄉㄨˋ ㄏㄣˇ ㄇㄢˋ|這台電腦的速度很慢|這台電腦的速度很慢',
  '|ㄓㄜˋ ㄍㄜ˙ ㄨㄣˋ ㄊㄧˊ ㄏㄣˇ ㄐㄧㄢˇ ㄉㄢ|這個問題很簡丹|這個問題很簡單',
  '# 學校與家庭',
  '老師說|ㄇㄧㄥˊ ㄊㄧㄢ ㄧㄠˋ ㄐㄧㄠ ㄗㄨㄛˋ ㄧㄝˋ|明天要交做業|明天要交作業',
  '|ㄇㄧㄥˊ ㄊㄧㄢ ㄧㄠˋ ㄎㄠˇ ㄕˋ|明天要考式|明天要考試',
  '|ㄊㄚ ㄉㄜ˙ ㄔㄥˊ ㄐㄧ ㄐㄧㄣˋ ㄅㄨˋ ㄏㄣˇ ㄉㄨㄛ|他的成積進步很多|他的成績進步很多',
  '|ㄒㄧㄠˇ ㄏㄞˊ ㄗㄞˋ ㄎㄜˋ ㄊㄧㄥ ㄒㄧㄝˇ ㄍㄨㄥ ㄎㄜˋ|小孩在客廳寫功課|小孩在客廳寫功課',
  '媽媽問|ㄋㄧˇ ㄐㄧㄣ ㄊㄧㄢ ㄐㄧˇ ㄉㄧㄢˇ ㄏㄨㄟˊ ㄐㄧㄚ ㄔ ㄈㄢˋ|你今天幾點回家吃飯|你今天幾點回家吃飯',
  '這件事情很重要，|ㄑㄧㄥˇ ㄅㄨˊ ㄧㄠˋ ㄨㄤˋ ㄐㄧˋ|請不要忘計|請不要忘記',
  '|ㄨㄛˇ ㄗㄞˋ ㄊㄨˊ ㄕㄨ ㄍㄨㄢˇ ㄎㄢˋ ㄕㄨ|我在圖書館看書|我在圖書館看書',
  '# 的／得、在／再',
  '|ㄊㄚ ㄆㄠˇ ㄉㄜ˙ ㄏㄣˇ ㄎㄨㄞˋ|他跑的很快|他跑得很快',
  '|ㄓㄜˋ ㄕˋ ㄨㄛˇ ㄉㄜ˙ ㄕㄨ|這是我的書|這是我的書',
  '|ㄨㄛˇ ㄇㄧㄥˊ ㄊㄧㄢ ㄗㄞˋ ㄌㄞˊ ㄓㄠˇ ㄋㄧˇ|我明天在來找你|我明天再來找你',
  '|ㄑㄧㄥˇ ㄗㄞˋ ㄕㄨㄛ ㄧ ㄘˋ|請再說一次|請再說一次',
  '|ㄊㄚ ㄕㄨㄛ ㄉㄜ˙ ㄏㄨㄚˋ ㄏㄣˇ ㄧㄡˇ ㄉㄠˋ ㄌㄧˇ|他說的話很有到理|他說的話很有道理',
  '|ㄨㄛˇ ㄐㄩㄝˊ ㄉㄜ˙ ㄓㄜˋ ㄍㄜ˙ ㄓㄨˇ ㄧˋ ㄅㄨˋ ㄘㄨㄛˋ|我覺得這個主義不錯|我覺得這個主意不錯',
  '# 地名與專有名詞（不該改）',
  '|ㄨㄛˇ ㄓㄨˋ ㄗㄞˋ ㄒㄧㄣ ㄅㄟˇ ㄕˋ ㄅㄢˇ ㄑㄧㄠˊ ㄑㄩ|我住在新北市板橋區|我住在新北市板橋區',
  '|ㄨㄛˇ ㄇㄣ˙ ㄩㄝ ㄗㄞˋ ㄧˊ ㄅㄟˇ ㄔㄜ ㄓㄢˋ ㄐㄧㄢˋ ㄇㄧㄢˋ|我們約在台北車站見面|我們約在台北車站見面',
  '|ㄍㄠ ㄒㄩㄥˊ ㄉㄜ˙ ㄊㄧㄢ ㄑㄧˋ ㄅㄧˇ ㄧˊ ㄅㄟˇ ㄖㄜˋ|高雄的天氣比台北熱|高雄的天氣比台北熱',
  '|ㄨㄛˇ ㄇㄣ˙ ㄍㄨㄥ ㄙ ㄗㄞˋ ㄒㄧㄣˋ ㄧˋ ㄑㄩ|我們公司在信義區|我們公司在信義區',
  '|ㄨㄛˇ ㄇㄧㄥˊ ㄊㄧㄢ ㄧㄠˋ ㄑㄩˋ ㄧˊ ㄅㄟˇ ㄎㄞ ㄏㄨㄟˋ|我明天要去台北開會|我明天要去台北開會',
  '# 聲調打錯（注音是使用者實際打的）',
  '|ㄨㄛˇ ㄇㄣˊ ㄇㄧㄥˊ ㄊㄧㄢ ㄐㄧㄢˋ|我門明天見|我們明天見',
  '謝謝你昨天的幫忙！|ㄅㄨˋ ㄎㄜˋ ㄑㄧˋ|不客器|不客氣',
  '|ㄨㄛˇ ㄇㄣ˙ ㄧ ㄑㄧˇ ㄑㄩˋ ㄎㄢˋ ㄉㄧㄢˋ ㄧㄥˇ ㄅㄚˇ|我們一起去看電影把|我們一起去看電影吧',
  '|ㄐㄧˋ ㄉㄜ˙ ㄉㄞˋ ㄩˇ ㄙㄢˋ|記得帶雨散|記得帶雨傘',
  '|ㄨㄛˇ ㄇㄣˊ ㄐㄧㄣ ㄊㄧㄢ ㄨㄢˇ ㄕㄤ˙ ㄑㄩˋ ㄧㄝˋ ㄕˋ|我門今天晚上去夜是|我們今天晚上去夜市',
  '# 按錯鍵（注音本身打錯）',
  '|ㄨㄛˇ ㄐㄧㄣ ㄊㄧㄢ ㄏㄣˇ ㄈㄤˊ|我今天很房|我今天很忙',
  '|ㄐㄧㄣ ㄊㄧㄢ ㄊㄧㄢ ㄑㄧˋ ㄏㄣˇ ㄎㄠˇ|今天天氣很考|今天天氣很好',
  '|ㄒㄧㄝˇ ㄒㄧㄝ˙ ㄋㄧˇ ㄉㄜ˙ ㄅㄤ ㄇㄤˊ|寫謝你的幫忙|謝謝你的幫忙',
  '|ㄨㄛˇ ㄉㄜ˙ ㄉㄢˋ ㄋㄠˇ ㄏㄨㄞˋ ㄌㄜ˙|我的蛋腦壞了|我的電腦壞了',
].join('\n');

// 題目存在這台電腦的瀏覽器儲存空間（改過的題目下次還在）；讀不到時用預設
function loadText(key, fallback) {
  try {
    const v = localStorage.getItem(key);
    return v === null ? fallback : v;
  } catch {
    return fallback;
  }
}
function saveText(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // 存不了就算了，只影響下次開啟時的預設
  }
}

let predictText = loadText('bench.predict', kDefaultPredict);
let correctText = loadText('bench.correct', kDefaultCorrect);
let doPredict = true;
let doCorrect = true;
let chosen = null;     // 勾選的模型設定（索引）
let running = false;
let statusText = '';
let run = null;        // 這次測試：{profiles, predict, correct, load[], predictResults[][], correctResults[][]}
let subscribed = false;

// 題目的一行 → 欄位（與輸入法端的解析相同：| 或全形｜分隔，# 開頭與空行略過）
function parseLines(text) {
  return text.split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('#'))
    .map((l) => l.split(/[|｜]/).map((f) => f.trim()));
}

const avg = (list) => {
  const ms = list.filter(Boolean).map((r) => r.ms);
  return ms.length ? Math.round(ms.reduce((a, b) => a + b, 0) / ms.length) + ' ms' : '—';
};
const rate = (list, total, key = 'ok') => {
  const done = list.filter(Boolean);
  if (!total) return '—';
  const hit = done.filter((r) => r[key]).length;
  return hit + ' / ' + total + (done.length < total ? '（進行中）' : '') + '　' + Math.round((hit * 100) / total) + '%';
};

export default {
  id: 'bench',
  nav: '模型測試',
  icon: '',
  title: '模型測試',
  desc: '用一組題目測試各個模型的預測與整句校正：命中率與反應時間。',

  onShow(ctx) {
    if (!subscribed) {
      subscribed = true;
      ctx.on('bench', (e) => this.onEvent(ctx, e));
    }
  },

  onEvent(ctx, e) {
    if (!run) return;
    if (e.state === 'pause') {
      statusText = e.message;
    } else if (e.state === 'load') {
      run.load[e.profile] = e;
      const name = run.profiles[e.profile].name;
      statusText = e.ok ? '測試中：' + name + '（載入 ' + e.ms + ' ms）' : name + '：' + e.error;
    } else if (e.state === 'predict') {
      run.predictResults[e.profile][e.index] = e;
    } else if (e.state === 'correct') {
      run.correctResults[e.profile][e.index] = e;
    } else {
      running = false;
      statusText = e.state === 'done' ? '測試完成。' : e.state === 'cancelled' ? '已取消。' : '測試失敗：' + e.message;
      if (ctx.isCurrent(this)) ctx.rerender();
      return;
    }
    if (ctx.isCurrent(this) && this.update) this.update();
  },

  async start(ctx) {
    const { state } = ctx;
    const profiles = [...chosen].sort((a, b) => a - b).map((i) => state.profiles[i]).filter(Boolean);
    const predict = doPredict ? parseLines(predictText) : [];
    const correct = doCorrect ? parseLines(correctText) : [];
    const f = state.form;
    const options = {
      prompt: f.predict.prompt,
      typo_prompt: f.typo.prompt,
      n_ctx: f.local.n_ctx,
      n_gpu_layers: int(f.local.gpu_layers, 0),
      n_threads: int(f.local.threads, 4),
      temperature: parseFloat(get(state.llm, 'llamacpp/temperature')) || 0.8,
    };
    try {
      await ctx.call('bench.start', {
        profiles, options,
        predict: doPredict ? predictText : '', correct: doCorrect ? correctText : '',
      });
    } catch (e) {
      ctx.status(e.message, 'error');
      return;
    }
    run = {
      profiles, predict, correct, load: [],
      predictResults: profiles.map(() => []), correctResults: profiles.map(() => []),
    };
    running = true;
    statusText = '載入模型中…';
    ctx.rerender();
  },

  // 結果：總表與每一題的輸出
  renderResults() {
    if (!run) return [];
    const { profiles, predict, correct } = run;
    const summary = h('div', { class: 'table-wrap' }, h('table', { class: 'table' },
      h('thead', {}, h('tr', {}, ...['模型', '載入', '預測 第一個對', '預測 前五個有', '預測 平均', '校正 正確', '校正 平均'].map((t) => h('th', {}, t)))),
      h('tbody', {}, ...profiles.map((p, i) => {
        const load = run.load[i];
        const failed = load && !load.ok;  // 沒載入：沒有結果
        const or = (text) => (failed ? '—' : text);
        return h('tr', {},
          h('td', {}, profileLabel(p)),
          h('td', { class: load && !load.ok ? 'wrap' : '' }, !load ? '…' : load.ok ? load.ms + ' ms' : '✗ ' + load.error),
          h('td', {}, or(rate(run.predictResults[i], predict.length, 'top1'))),
          h('td', {}, or(rate(run.predictResults[i], predict.length))),
          h('td', {}, or(avg(run.predictResults[i]))),
          h('td', {}, or(rate(run.correctResults[i], correct.length))),
          h('td', {}, or(avg(run.correctResults[i]))));
      }))));
    const failed = (i) => run.load[i] && !run.load[i].ok;
    const mark = (r) => (!r ? '…' : r.ok ? '✓ ' : '✗ ');
    const detail = (title, headers, rows) => rows.length ? [
      h('div', { class: 'spacer' }),
      h('div', { class: 'field-label' }, title),
      h('div', { class: 'table-wrap' }, h('table', { class: 'table' },
        h('thead', {}, h('tr', {}, ...headers.map((t) => h('th', {}, t)), ...profiles.map((p) => h('th', {}, p.name)))),
        h('tbody', {}, ...rows))),
    ] : [];
    const predictRows = predict.map(([context, expected], j) => h('tr', {},
      h('td', { class: 'wrap' }, context), h('td', {}, expected),
      ...profiles.map((_, i) => {
        const r = run.predictResults[i][j];
        if (failed(i)) return h('td', {}, '—');
        return h('td', { class: 'wrap' }, mark(r) + (r ? r.candidates.join('、') || '（沒有候選）' : '') + (r ? '（' + r.ms + ' ms）' : ''));
      })));
    const correctRows = correct.map(([, , draft, expected], j) => h('tr', {},
      h('td', {}, draft), h('td', {}, expected),
      ...profiles.map((_, i) => {
        const r = run.correctResults[i][j];
        if (failed(i)) return h('td', {}, '—');
        return h('td', {}, mark(r) + (r ? (r.changed ? r.output : '（不改）') + '（' + r.ms + ' ms）' : ''));
      })));
    return [
      summary,
      ...detail('預測（✓ = 前五個候選有期望的詞）', ['前文', '期望'], predictRows),
      ...detail('整句校正', ['初稿', '正確'], correctRows),
    ];
  },

  render(ctx) {
    const { profiles, use } = ctx.state;
    // 預設勾選智慧預測與注音校正正在用的模型（render 比 onShow 先呼叫）
    if (chosen === null)
      chosen = new Set([use.predict, use.typo].filter((i) => i >= 0 && i < profiles.length));
    const statusEl = note(statusText);
    const results = h('div', {}, ...this.renderResults());
    this.update = () => {
      statusEl.set(statusText);
      results.replaceChildren(...this.renderResults());
    };
    const startButton = button(running ? '測試中…' : '開始測試', () => this.start(ctx), { primary: true, disabled: running });
    const cancelButton = button('取消', () => ctx.call('bench.cancel'), { disabled: !running });

    return [
      section('要測試的模型',
        card({
          desc: '用「語言模型」頁的設定與目前表單的提示詞、本機模型執行設定（不必先套用）。設定程式會自己載入模型，測完就釋放。測試期間輸入法會先放掉它的模型（預測、校正與推薦暫時不出現，精煉也暫停），測完自動重新載入。API 模型每一題都會送出請求。',
          below: profiles.length
            ? h('div', {}, ...profiles.map((p, i) => h('div', {}, checkbox(chosen.has(i), profileLabel(p), (v) => {
              if (v) chosen.add(i);
              else chosen.delete(i);
            }, { disabled: running }))))
            : h('div', { class: 'muted' }, '還沒有模型設定，請先到「語言模型」頁新增。'),
        })),
      section('題目',
        card({
          title: checkbox(doPredict, '預測：前文|期望的詞', (v) => { doPredict = v; }, { disabled: running }),
          desc: '期望的詞出現在前五個候選裡算對（候選多接了字也算）；另外統計第一個候選就對的比例。',
          below: textarea(predictText, (v) => { predictText = v; saveText('bench.predict', v); }, { rows: 8, disabled: running }),
        }),
        card({
          title: checkbox(doCorrect, '整句校正：前文|注音|初稿|正確的句子', (v) => { doCorrect = v; }, { disabled: running }),
          desc: '前文可以空白；注音的每個音節以空格或聲調斷開（和打字時送給模型的一樣）。初稿和正確的句子相同時，模型不改才算對。# 開頭的行是註解。',
          below: textarea(correctText, (v) => { correctText = v; saveText('bench.correct', v); }, { rows: 8, disabled: running }),
        }),
        buttons(button('還原預設題目', () => {
          predictText = kDefaultPredict;
          correctText = kDefaultCorrect;
          saveText('bench.predict', predictText);
          saveText('bench.correct', correctText);
          ctx.rerender();
        }, { disabled: running }))),
      section('結果',
        card({
          below: [buttons(startButton, cancelButton), statusEl, h('div', { class: 'spacer' }), results],
        })),
    ];
  },
};
