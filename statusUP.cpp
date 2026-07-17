// statusUP - ランキング貼り付けテキストを取り込み、戦闘力・順位の変動を集計する
//
//   statusUP                     クリップボードから取り込み → HTML レポートを開く
//   statusUP add [rawfile] [--time "YYYY-MM-DD HH:MM"]
//   statusUP report [--no-open]
//   statusUP list
//   statusUP diff [A] [B] [--top N]
//   statusUP history <name>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;
namespace fs = std::filesystem;

static const char *DATA_DIR = "data";

// ---------------------------------------------------------------- データ構造

struct Entry
{
    int rank = 0;
    string name;
    long long power = 0;
};

struct Snapshot
{
    string stamp; // "YYYY-MM-DD HH:MM"
    string file;
    vector<Entry> entries;
};

// -------------------------------------------------- クリップボード / ブラウザ

// クリップボードのテキストを UTF-8 で取得する。取れなければ空文字列
static string readClipboard()
{
#ifdef _WIN32
    if (!OpenClipboard(nullptr))
        return "";
    string out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h)
    {
        LPCWSTR w = (LPCWSTR)GlobalLock(h);
        if (w)
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1)
            {
                out.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
#else
    return "";
#endif
}

static void openInBrowser(const string &path)
{
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    (void)!system(cmd.c_str());
#endif
}

// ------------------------------------------------------------ 文字列ユーティリティ

static string trim(const string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n\v\f");
    if (b == string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n\v\f");
    return s.substr(b, e - b + 1);
}

// UTF-8 の表示幅。全角(CJK・仮名・記号)は 2 桁として数える
static int dispWidth(const string &s)
{
    int w = 0;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i];
        int len = 1;
        unsigned int cp = c;
        if (c >= 0xF0)
        {
            len = 4;
            cp = c & 0x07;
        }
        else if (c >= 0xE0)
        {
            len = 3;
            cp = c & 0x0F;
        }
        else if (c >= 0xC0)
        {
            len = 2;
            cp = c & 0x1F;
        }
        for (int k = 1; k < len && i + k < s.size(); k++)
            cp = (cp << 6) | (s[i + k] & 0x3F);
        i += len;

        bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                    (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                    (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                    (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
        w += wide ? 2 : 1;
    }
    return w;
}

// 表示幅を揃えてパディング(必要なら末尾を切り詰める)
static string padName(const string &s, int width)
{
    if (dispWidth(s) <= width)
        return s + string(width - dispWidth(s), ' ');

    string out;
    int w = 0;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i];
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        string ch = s.substr(i, len);
        int cw = dispWidth(ch);
        if (w + cw > width - 1)
            break;
        out += ch;
        w += cw;
        i += len;
    }
    out += "…";
    w += 1;
    if (w < width)
        out += string(width - w, ' ');
    return out;
}

// 12345 -> "12,345"
static string comma(long long v)
{
    bool neg = v < 0;
    string d = to_string(neg ? -v : v);
    string out;
    for (size_t i = 0; i < d.size(); i++)
    {
        if (i && (d.size() - i) % 3 == 0)
            out += ',';
        out += d[i];
    }
    return (neg ? "-" : "") + out;
}

static string signedComma(long long v)
{
    if (v > 0)
        return "+" + comma(v);
    return comma(v); // 0 と負はそのまま
}

// --------------------------------------------------------------- パーサ

// 貼り付けテキストに混ざる装飾トークン
static bool isNoise(const string &t)
{
    static const set<string> noise = {"フレーム", "▼", "▲", "#", "プレイヤー", "戦闘力", "順位"};
    return noise.count(t) > 0;
}

// "745,769" / "745769" -> 745769
static bool parsePower(const string &t, long long &out)
{
    if (t.empty())
        return false;
    string d;
    for (char c : t)
    {
        if (c == ',')
            continue;
        if (!isdigit((unsigned char)c))
            return false;
        d += c;
    }
    if (d.empty() || d.size() > 18)
        return false;
    out = stoll(d);
    return true;
}

// "1位" / "12" -> 順位。カンマを含むものは戦闘力なので順位とみなさない
static bool parseRank(const string &t, int &out)
{
    string body = t;
    const string kurai = "位";
    if (body.size() > kurai.size() && body.compare(body.size() - kurai.size(), kurai.size(), kurai) == 0)
        body = body.substr(0, body.size() - kurai.size());
    if (body.empty() || body.size() > 4)
        return false;
    for (char c : body)
        if (!isdigit((unsigned char)c))
            return false;
    int v = stoi(body);
    if (v < 1 || v > 5000)
        return false;
    out = v;
    return true;
}

// 改行・タブで区切って空でないトークンを取り出す
static vector<string> tokenize(const string &text)
{
    vector<string> out;
    string cur;
    for (char c : text)
    {
        if (c == '\n' || c == '\r' || c == '\t')
        {
            string t = trim(cur);
            if (!t.empty())
                out.push_back(t);
            cur.clear();
        }
        else
            cur += c;
    }
    string t = trim(cur);
    if (!t.empty())
        out.push_back(t);
    return out;
}

// 順位 → 名前 → 戦闘力 の順で現れることだけを頼りに読む状態機械。
// 上位3件が 2位/1位/3位 の順で並ぶレイアウトも、順位を読み取っているのでそのまま扱える。
static vector<Entry> parseRaw(const string &text, vector<string> &warnings)
{
    enum State
    {
        RANK,
        NAME,
        POWER
    };
    State st = RANK;
    Entry cur;
    vector<Entry> out;

    for (const string &tok : tokenize(text))
    {
        if (isNoise(tok))
            continue;

        if (st == RANK)
        {
            int r;
            if (parseRank(tok, r) && tok.find(',') == string::npos)
            {
                cur = Entry{r, "", 0};
                st = NAME;
            }
            continue; // 順位が来るまで読み飛ばす(見出し等)
        }
        if (st == NAME)
        {
            cur.name = tok;
            st = POWER;
            continue;
        }
        // POWER
        long long p;
        if (parsePower(tok, p))
        {
            cur.power = p;
            out.push_back(cur);
            st = RANK;
        }
        else
        {
            // 戦闘力が来るはずの位置に別のものが来た → 名前を取り違えた可能性
            warnings.push_back("順位 " + to_string(cur.rank) + " (" + cur.name +
                               ") の戦闘力を読めませんでした: \"" + tok + "\"");
            st = RANK;
        }
    }
    if (st != RANK)
        warnings.push_back("末尾のデータが途中で切れています (順位 " + to_string(cur.rank) + ")");

    sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) { return a.rank < b.rank; });
    return out;
}

// ------------------------------------------------------------ 保存 / 読込

static string csvQuote(const string &s)
{
    string out = "\"";
    for (char c : s)
    {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }
    return out + "\"";
}

static vector<string> csvSplit(const string &line)
{
    vector<string> out;
    string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); i++)
    {
        char c = line[i];
        if (q)
        {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"')
            {
                cur += '"';
                i++;
            }
            else if (c == '"')
                q = false;
            else
                cur += c;
        }
        else if (c == '"')
            q = true;
        else if (c == ',')
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    out.push_back(cur);
    return out;
}

static string nowStamp()
{
    time_t t = time(nullptr);
    tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
    return buf;
}

// "YYYY-MM-DD HH:MM" -> "YYYY-MM-DD_HHMM.csv"
static string stampToFile(const string &stamp)
{
    string s;
    for (size_t i = 0; i < stamp.size(); i++)
    {
        char c = stamp[i];
        if (c == ' ')
            s += '_';
        else if (c == ':')
            continue;
        else
            s += c;
    }
    return s + ".csv";
}

// 時刻差(時間)。解釈できなければ負値を返す
static double hoursBetween(const string &a, const string &b)
{
    auto toTime = [](const string &s, time_t &out) -> bool {
        tm t{};
        if (sscanf(s.c_str(), "%d-%d-%d %d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour,
                   &t.tm_min) != 5)
            return false;
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        t.tm_isdst = -1;
        out = mktime(&t);
        return out != (time_t)-1;
    };
    time_t ta, tb;
    if (!toTime(a, ta) || !toTime(b, tb))
        return -1;
    return difftime(tb, ta) / 3600.0;
}

static void saveSnapshot(const Snapshot &s)
{
    fs::create_directories(DATA_DIR);
    fs::path p = fs::path(DATA_DIR) / stampToFile(s.stamp);
    ofstream f(p, ios::binary);
    if (!f)
        throw runtime_error("書き込めません: " + p.string());
    f << "# " << s.stamp << "\n";
    f << "rank,name,power\n";
    for (const Entry &e : s.entries)
        f << e.rank << ',' << csvQuote(e.name) << ',' << e.power << '\n';
    cout << "保存しました: " << p.string() << " (" << s.entries.size() << " 件)\n";
}

static Snapshot loadSnapshot(const fs::path &p)
{
    ifstream f(p, ios::binary);
    if (!f)
        throw runtime_error("読み込めません: " + p.string());
    Snapshot s;
    s.file = p.filename().string();
    string line;
    while (getline(f, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            s.stamp = trim(line.substr(1));
            continue;
        }
        if (line.rfind("rank,", 0) == 0)
            continue;
        vector<string> c = csvSplit(line);
        if (c.size() < 3)
            continue;
        Entry e;
        e.rank = stoi(c[0]);
        e.name = c[1];
        e.power = stoll(c[2]);
        s.entries.push_back(e);
    }
    if (s.stamp.empty())
        s.stamp = p.stem().string();
    return s;
}

static vector<fs::path> snapshotFiles()
{
    vector<fs::path> out;
    if (!fs::exists(DATA_DIR))
        return out;
    for (const auto &e : fs::directory_iterator(DATA_DIR))
        if (e.is_regular_file() && e.path().extension() == ".csv")
            out.push_back(e.path());
    sort(out.begin(), out.end()); // ファイル名が時系列順
    return out;
}

// 引数(1始まりの番号 / ファイル名 / パス)からスナップショットを解決する
static Snapshot resolveSnapshot(const string &spec)
{
    vector<fs::path> files = snapshotFiles();
    bool numeric = !spec.empty() && all_of(spec.begin(), spec.end(),
                                           [](char c) { return isdigit((unsigned char)c); });
    if (numeric)
    {
        int i = stoi(spec);
        if (i < 1 || i >(int) files.size())
            throw runtime_error("番号 " + spec + " のスナップショットはありません");
        return loadSnapshot(files[i - 1]);
    }
    for (const fs::path &p : files)
        if (p.filename().string() == spec || p.stem().string() == spec)
            return loadSnapshot(p);
    if (fs::exists(spec))
        return loadSnapshot(spec);
    throw runtime_error("スナップショットが見つかりません: " + spec);
}

// ------------------------------------------------------------------ 集計

struct Row
{
    string name;
    bool inA = false, inB = false;
    int rankA = 0, rankB = 0;
    long long powerA = 0, powerB = 0;

    long long gain() const { return powerB - powerA; }
    double rate() const { return powerA > 0 ? (double)gain() / powerA * 100.0 : 0.0; }
    int rankMove() const { return rankA - rankB; } // 正 = 順位が上がった
};

static vector<Row> buildRows(const Snapshot &a, const Snapshot &b)
{
    map<string, Row> m;
    for (const Entry &e : a.entries)
    {
        Row &r = m[e.name];
        r.name = e.name;
        r.inA = true;
        r.rankA = e.rank;
        r.powerA = e.power;
    }
    for (const Entry &e : b.entries)
    {
        Row &r = m[e.name];
        r.name = e.name;
        r.inB = true;
        r.rankB = e.rank;
        r.powerB = e.power;
    }
    vector<Row> out;
    for (auto &kv : m)
        out.push_back(kv.second);
    return out;
}

static const int NAMEW = 24;

static string rankMoveStr(const Row &r)
{
    if (!r.inA)
        return "NEW";
    if (!r.inB)
        return "OUT";
    int m = r.rankMove();
    if (m > 0)
        return "↑" + to_string(m);
    if (m < 0)
        return "↓" + to_string(-m);
    return "→0";
}

static void printGainRanking(const vector<Row> &rows, double hours, int top)
{
    vector<Row> v;
    for (const Row &r : rows)
        if (r.inA && r.inB)
            v.push_back(r);
    sort(v.begin(), v.end(), [](const Row &a, const Row &b) { return a.gain() > b.gain(); });

    cout << "\n■ 戦力上昇値ランキング\n";
    cout << "  #  " << padName("プレイヤー", NAMEW) << "     上昇値      上昇率";
    if (hours > 0)
        cout << "     時間あたり";
    cout << "\n";
    int n = top > 0 ? min<int>(top, (int)v.size()) : (int)v.size();
    for (int i = 0; i < n; i++)
    {
        const Row &r = v[i];
        ostringstream pct;
        pct << fixed << setprecision(2) << r.rate() << "%";
        cout << setw(3) << (i + 1) << "  " << padName(r.name, NAMEW) << setw(11)
             << signedComma(r.gain()) << setw(11) << pct.str();
        if (hours > 0)
            cout << setw(13) << (signedComma((long long)(r.gain() / hours)) + "/h");
        cout << "\n";
    }
}

static void printRateRanking(const vector<Row> &rows, int top)
{
    vector<Row> v;
    for (const Row &r : rows)
        if (r.inA && r.inB && r.powerA > 0)
            v.push_back(r);
    sort(v.begin(), v.end(), [](const Row &a, const Row &b) { return a.rate() > b.rate(); });

    cout << "\n■ 上昇率ランキング\n";
    cout << "  #  " << padName("プレイヤー", NAMEW) << "     上昇率        上昇値\n";
    int n = top > 0 ? min<int>(top, (int)v.size()) : (int)v.size();
    for (int i = 0; i < n; i++)
    {
        const Row &r = v[i];
        ostringstream pct;
        pct << fixed << setprecision(2) << r.rate() << "%";
        cout << setw(3) << (i + 1) << "  " << padName(r.name, NAMEW) << setw(10) << pct.str()
             << setw(14) << signedComma(r.gain()) << "\n";
    }
}

static void printRankMoves(const vector<Row> &rows)
{
    vector<Row> v;
    for (const Row &r : rows)
        if (r.inA && r.inB && r.rankMove() != 0)
            v.push_back(r);
    sort(v.begin(), v.end(), [](const Row &a, const Row &b) { return a.rankMove() > b.rankMove(); });

    cout << "\n■ ランキング遷移 (変動があったプレイヤーのみ)\n";
    cout << "     " << padName("プレイヤー", NAMEW) << "   前回 → 今回   増減\n";
    for (const Row &r : v)
        cout << "     " << padName(r.name, NAMEW) << setw(5) << r.rankA << " → " << setw(3)
             << r.rankB << setw(8) << rankMoveStr(r) << "\n";
    if (v.empty())
        cout << "     (順位の変動なし)\n";
}

static void printAll(const vector<Row> &rows)
{
    vector<Row> v = rows;
    sort(v.begin(), v.end(), [](const Row &a, const Row &b) {
        int ra = a.inB ? a.rankB : 100000 + a.rankA;
        int rb = b.inB ? b.rankB : 100000 + b.rankA;
        return ra < rb;
    });
    cout << "\n■ 戦闘力変動 (全プレイヤー)\n";
    cout << " 順位  " << padName("プレイヤー", NAMEW) << "        前回        今回        変動   順位\n";
    for (const Row &r : v)
    {
        cout << setw(4) << (r.inB ? to_string(r.rankB) : "-") << "  " << padName(r.name, NAMEW)
             << setw(12) << (r.inA ? comma(r.powerA) : "-") << setw(12)
             << (r.inB ? comma(r.powerB) : "-") << setw(12)
             << (r.inA && r.inB ? signedComma(r.gain()) : "-") << setw(7) << rankMoveStr(r) << "\n";
    }
}

static void printSummary(const Snapshot &a, const Snapshot &b, const vector<Row> &rows, double hours)
{
    long long totalGain = 0;
    int moved = 0, up = 0, down = 0, newcomers = 0, gone = 0;
    for (const Row &r : rows)
    {
        if (r.inA && r.inB)
        {
            totalGain += r.gain();
            if (r.rankMove() > 0)
                up++;
            else if (r.rankMove() < 0)
                down++;
            if (r.gain() != 0)
                moved++;
        }
        else if (r.inB)
            newcomers++;
        else
            gone++;
    }
    cout << "========================================================================\n";
    cout << " 期間 : " << a.stamp << "  →  " << b.stamp;
    if (hours > 0)
    {
        ostringstream h;
        h << fixed << setprecision(1) << hours;
        cout << "   (" << h.str() << " 時間)";
    }
    cout << "\n";
    cout << " 人数 : " << a.entries.size() << " → " << b.entries.size() << "   新規 " << newcomers
         << " / 圏外 " << gone << "\n";
    cout << " 戦力 : 合計上昇 " << signedComma(totalGain) << "   上昇したプレイヤー " << moved
         << " 人\n";
    cout << " 順位 : 上昇 " << up << " 人 / 下降 " << down << " 人\n";
    cout << "========================================================================\n";
}

// ------------------------------------------------------------ HTML レポート

static string jsonEscape(const string &s)
{
    string out;
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += (char)c;
        }
    }
    return out;
}

// 全スナップショットを 1 つの JSON にまとめる(集計はブラウザ側で行う)
static string buildJson(const vector<Snapshot> &snaps)
{
    ostringstream j;
    j << "{\"generated\":\"" << jsonEscape(nowStamp()) << "\",\"snapshots\":[";
    for (size_t i = 0; i < snaps.size(); i++)
    {
        if (i)
            j << ',';
        j << "{\"stamp\":\"" << jsonEscape(snaps[i].stamp) << "\",\"entries\":[";
        for (size_t k = 0; k < snaps[i].entries.size(); k++)
        {
            const Entry &e = snaps[i].entries[k];
            if (k)
                j << ',';
            j << "{\"r\":" << e.rank << ",\"n\":\"" << jsonEscape(e.name) << "\",\"p\":" << e.power
              << '}';
        }
        j << "]}";
    }
    j << "]}";
    return j.str();
}

static const char *HTML_TEMPLATE = R"HTML(<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>戦闘力ランキング推移</title>
<style>
  :root {
    --bg: #12141a; --panel: #1b1e27; --panel2: #232734; --border: #2e3342;
    --text: #e6e9f0; --muted: #8b91a3; --accent: #4f8cff;
    --up: #4caf7d; --down: #e05c5c; --gold: #e8b84b;
  }
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { background: var(--bg); color: var(--text);
         font-family: "Segoe UI","Hiragino Sans","Yu Gothic UI",sans-serif; min-height: 100vh; }
  header { padding: 14px 24px; border-bottom: 1px solid var(--border);
           display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 8px; }
  header h1 { font-size: 1.15rem; font-weight: 600; }
  header h1 span { color: var(--accent); margin-right: 8px; }
  header .gen { color: var(--muted); font-size: 0.8rem; }
  .wrap { max-width: 1180px; margin: 0 auto; padding: 16px 24px 60px; }
  .panel { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; }

  .controls { display: flex; gap: 14px; align-items: center; flex-wrap: wrap; padding: 12px 14px; margin-bottom: 14px; }
  .controls label { font-size: 0.8rem; color: var(--muted); display: flex; align-items: center; gap: 6px; }
  select, input[type=text] {
    background: var(--panel2); color: var(--text); border: 1px solid var(--border);
    border-radius: 6px; padding: 6px 8px; font-size: 0.85rem; font-family: inherit; }
  input[type=text] { min-width: 200px; }
  .arrow { color: var(--muted); }
  .swap { background: var(--panel2); border: 1px solid var(--border); color: var(--muted);
          border-radius: 6px; padding: 6px 10px; cursor: pointer; font-size: 0.8rem; }
  .swap:hover { color: var(--text); border-color: var(--accent); }

  .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 12px; margin-bottom: 14px; }
  .card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 12px 14px; }
  .card .k { font-size: 0.75rem; color: var(--muted); margin-bottom: 6px; }
  .card .v { font-size: 1.35rem; font-weight: 600; }
  .card .s { font-size: 0.75rem; color: var(--muted); margin-top: 4px; }

  .tabs { display: flex; gap: 6px; margin-bottom: 12px; flex-wrap: wrap; }
  .tab { background: var(--panel); border: 1px solid var(--border); color: var(--muted);
         padding: 7px 14px; border-radius: 8px; cursor: pointer; font-size: 0.85rem; }
  .tab:hover { color: var(--text); }
  .tab.on { background: var(--accent); border-color: var(--accent); color: #fff; }

  .tablewrap { overflow-x: auto; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th, td { padding: 8px 10px; text-align: right; white-space: nowrap; }
  th { color: var(--muted); font-weight: 500; font-size: 0.78rem; border-bottom: 1px solid var(--border);
       cursor: pointer; user-select: none; position: sticky; top: 0; background: var(--panel); }
  th:hover { color: var(--text); }
  th.sorted { color: var(--accent); }
  td { border-bottom: 1px solid rgba(46,51,66,0.5); }
  tbody tr { cursor: pointer; }
  tbody tr:hover { background: var(--panel2); }
  .name { text-align: left; max-width: 260px; overflow: hidden; text-overflow: ellipsis; }
  .rk { color: var(--muted); }
  .rk.top { color: var(--gold); font-weight: 600; }
  .up { color: var(--up); }
  .down { color: var(--down); }
  .zero { color: var(--muted); }
  .tag { font-size: 0.7rem; padding: 1px 6px; border-radius: 4px; border: 1px solid; }
  .tag.new { color: var(--accent); border-color: var(--accent); }
  .tag.out { color: var(--down); border-color: var(--down); }
  .empty { padding: 30px; text-align: center; color: var(--muted); }
  .note { padding: 10px 14px; margin-bottom: 14px; font-size: 0.82rem; color: var(--muted);
          border-left: 3px solid var(--accent); background: var(--panel); border-radius: 0 8px 8px 0; }

  .modal { position: fixed; inset: 0; background: rgba(0,0,0,0.6); display: flex;
           align-items: center; justify-content: center; padding: 20px; z-index: 10; }
  .modal.hidden { display: none; }
  .modal .panel { max-width: 620px; width: 100%; max-height: 84vh; overflow: auto; }
  .mhead { display: flex; justify-content: space-between; align-items: center;
           padding: 12px 14px; border-bottom: 1px solid var(--border); }
  .mhead h2 { font-size: 1rem; }
  .close { background: none; border: none; color: var(--muted); font-size: 1.3rem; cursor: pointer; }
  .close:hover { color: var(--text); }
  .mbody { padding: 14px; }
  .spark { width: 100%; height: 90px; margin-bottom: 12px; }
</style>
</head>
<body>
<header>
  <h1><span>▲</span>戦闘力ランキング推移</h1>
  <div class="gen" id="gen"></div>
</header>

<div class="wrap">
  <div class="panel controls">
    <label>比較元 <select id="selA"></select></label>
    <span class="arrow">→</span>
    <label>比較先 <select id="selB"></select></label>
    <button class="swap" id="swap">入れ替え</button>
    <label style="margin-left:auto">絞り込み <input type="text" id="q" placeholder="プレイヤー名"></label>
  </div>

  <div id="note"></div>
  <div class="cards" id="cards"></div>

  <div class="tabs" id="tabs"></div>

  <div class="panel tablewrap">
    <table>
      <thead><tr id="head"></tr></thead>
      <tbody id="body"></tbody>
    </table>
    <div class="empty" id="empty" style="display:none">該当するプレイヤーがいません</div>
  </div>
</div>

<div class="modal hidden" id="modal">
  <div class="panel">
    <div class="mhead"><h2 id="mname"></h2><button class="close" id="mclose">×</button></div>
    <div class="mbody">
      <svg class="spark" id="spark" preserveAspectRatio="none"></svg>
      <div class="tablewrap"><table>
        <thead><tr><th class="name">日時</th><th>順位</th><th>戦闘力</th><th>変動</th></tr></thead>
        <tbody id="mbody"></tbody>
      </table></div>
    </div>
  </div>
</div>

<script>
const DATA = __DATA__;
const S = DATA.snapshots;

const fmt = n => n.toLocaleString('en-US');
const sfmt = n => (n > 0 ? '+' : '') + fmt(n);
const cls = n => n > 0 ? 'up' : n < 0 ? 'down' : 'zero';

// "YYYY-MM-DD HH:MM" -> Date
function pd(s) {
  const m = s.match(/(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2})/);
  return m ? new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5]) : null;
}
function hoursBetween(a, b) {
  const x = pd(a), y = pd(b);
  return x && y ? (y - x) / 3600000 : 0;
}

let iA = Math.max(0, S.length - 2), iB = S.length - 1;
let tab = 'gain', sortKey = 'gain', sortDir = -1, query = '';

// 2 時点を突き合わせて 1 行にまとめる
function buildRows() {
  const A = S[iA], B = S[iB], m = new Map();
  A.entries.forEach(e => m.set(e.n, { n: e.n, inA: true, rA: e.r, pA: e.p }));
  B.entries.forEach(e => {
    const r = m.get(e.n) || { n: e.n };
    r.inB = true; r.rB = e.r; r.pB = e.p; m.set(e.n, r);
  });
  const hrs = hoursBetween(A.stamp, B.stamp);
  return [...m.values()].map(r => {
    const both = r.inA && r.inB;
    r.gain = both ? r.pB - r.pA : null;
    r.rate = both && r.pA > 0 ? (r.pB - r.pA) / r.pA * 100 : null;
    r.perh = both && hrs > 0 ? r.gain / hrs : null;
    r.move = both ? r.rA - r.rB : null;       // 正 = 順位が上がった
    r.cur = r.inB ? r.rB : 100000 + r.rA;     // 圏外は末尾へ
    return r;
  });
}

const TABS = [
  { id: 'gain', label: '戦力上昇値ランキング', key: 'gain', dir: -1, filter: r => r.inA && r.inB },
  { id: 'rate', label: '上昇率ランキング', key: 'rate', dir: -1, filter: r => r.inA && r.inB && r.pA > 0 },
  { id: 'move', label: 'ランキング遷移', key: 'move', dir: -1, filter: r => r.inA && r.inB && r.move !== 0 },
  { id: 'all',  label: '全プレイヤー', key: 'cur', dir: 1, filter: () => true },
];

const COLS = [
  { key: 'cur',  label: '順位',   render: r => r.inB
      ? `<span class="rk${r.rB <= 3 ? ' top' : ''}">${r.rB}</span>` : '<span class="rk">-</span>' },
  { key: 'n',    label: 'プレイヤー', cls: 'name', render: r => {
      const tag = !r.inA ? ' <span class="tag new">NEW</span>'
                : !r.inB ? ' <span class="tag out">圏外</span>' : '';
      return esc(r.n) + tag; } },
  { key: 'pA',   label: '前回',   render: r => r.inA ? fmt(r.pA) : '-' },
  { key: 'pB',   label: '今回',   render: r => r.inB ? fmt(r.pB) : '-' },
  { key: 'gain', label: '上昇値', render: r => r.gain === null ? '-'
      : `<span class="${cls(r.gain)}">${sfmt(r.gain)}</span>` },
  { key: 'rate', label: '上昇率', render: r => r.rate === null ? '-'
      : `<span class="${cls(r.rate)}">${r.rate > 0 ? '+' : ''}${r.rate.toFixed(2)}%</span>` },
  { key: 'perh', label: '時間あたり', render: r => r.perh === null ? '-'
      : `<span class="${cls(r.perh)}">${sfmt(Math.round(r.perh))}/h</span>` },
  { key: 'move', label: '順位変動', render: r => {
      if (!r.inA) return '<span class="tag new">NEW</span>';
      if (!r.inB) return '<span class="tag out">圏外</span>';
      if (r.move === 0) return '<span class="zero">→ 0</span>';
      return `<span class="${cls(r.move)}">${r.move > 0 ? '↑' : '↓'}${Math.abs(r.move)}</span>`
           + ` <span class="rk">(${r.rA}→${r.rB})</span>`; } },
];

function esc(s) {
  return s.replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

function renderCards(rows) {
  const A = S[iA], B = S[iB];
  const hrs = hoursBetween(A.stamp, B.stamp);
  let total = 0, up = 0, down = 0, nw = 0, out = 0, gained = 0;
  rows.forEach(r => {
    if (r.inA && r.inB) {
      total += r.gain;
      if (r.gain !== 0) gained++;
      if (r.move > 0) up++; else if (r.move < 0) down++;
    } else if (r.inB) nw++; else out++;
  });
  const same = iA === iB;
  document.getElementById('cards').innerHTML = [
    { k: '期間', v: same ? '—' : (hrs > 0 ? hrs.toFixed(1) + ' 時間' : '—'),
      s: `${A.stamp} → ${B.stamp}` },
    { k: '合計上昇', v: `<span class="${cls(total)}">${sfmt(total)}</span>`,
      s: `上昇したプレイヤー ${gained} 人` },
    { k: '人数', v: `${A.entries.length} → ${B.entries.length}`,
      s: `新規 ${nw} / 圏外 ${out}` },
    { k: '順位変動', v: `<span class="up">↑${up}</span> / <span class="down">↓${down}</span>`,
      s: '上昇 / 下降' },
  ].map(c => `<div class="card"><div class="k">${c.k}</div><div class="v">${c.v}</div><div class="s">${esc(c.s)}</div></div>`).join('');
}

function render() {
  const all = buildRows();
  renderCards(all);

  const t = TABS.find(x => x.id === tab);
  let rows = all.filter(t.filter);
  if (query) {
    const q = query.toLowerCase();
    rows = rows.filter(r => r.n.toLowerCase().includes(q));
  }
  rows.sort((a, b) => {
    const x = a[sortKey], y = b[sortKey];
    if (x === null || x === undefined) return 1;
    if (y === null || y === undefined) return -1;
    if (typeof x === 'string') return x.localeCompare(y, 'ja') * sortDir;
    return (x - y) * sortDir;
  });

  document.getElementById('head').innerHTML = COLS.map(c =>
    `<th data-k="${c.key}" class="${c.cls || ''}${sortKey === c.key ? ' sorted' : ''}">${c.label}${
      sortKey === c.key ? (sortDir < 0 ? ' ▼' : ' ▲') : ''}</th>`).join('');

  document.getElementById('body').innerHTML = rows.map(r =>
    `<tr data-n="${esc(r.n)}">` + COLS.map(c =>
      `<td class="${c.cls || ''}">${c.render(r)}</td>`).join('') + '</tr>').join('');

  document.getElementById('empty').style.display = rows.length ? 'none' : 'block';
}

// --- 個人の推移 ---
function showPlayer(name) {
  const hist = S.map(s => ({ stamp: s.stamp, e: s.entries.find(e => e.n === name) }));
  document.getElementById('mname').textContent = name;

  let prev = null;
  document.getElementById('mbody').innerHTML = hist.map(h => {
    if (!h.e) return `<tr><td class="name">${esc(h.stamp)}</td><td colspan="3" class="zero">圏外</td></tr>`;
    const d = prev === null ? null : h.e.p - prev;
    prev = h.e.p;
    return `<tr><td class="name">${esc(h.stamp)}</td><td class="rk">${h.e.r}</td>`
         + `<td>${fmt(h.e.p)}</td><td>${d === null ? '-' : `<span class="${cls(d)}">${sfmt(d)}</span>`}</td></tr>`;
  }).join('');

  // 戦闘力の推移をスパークラインで描く
  const pts = hist.filter(h => h.e).map(h => h.e.p);
  const svg = document.getElementById('spark');
  if (pts.length >= 2) {
    const w = 560, h = 90, pad = 6;
    const lo = Math.min(...pts), hi = Math.max(...pts), span = hi - lo || 1;
    const xy = pts.map((p, i) => [
      pad + i * (w - pad * 2) / (pts.length - 1),
      h - pad - (p - lo) / span * (h - pad * 2)
    ]);
    svg.setAttribute('viewBox', `0 0 ${w} ${h}`);
    svg.innerHTML =
      `<polyline fill="none" stroke="#4f8cff" stroke-width="2" points="${xy.map(p => p.join(',')).join(' ')}"/>`
      + xy.map(p => `<circle cx="${p[0]}" cy="${p[1]}" r="3" fill="#4f8cff"/>`).join('');
    svg.style.display = 'block';
  } else {
    svg.style.display = 'none';
  }
  document.getElementById('modal').classList.remove('hidden');
}

// --- 初期化 ---
document.getElementById('gen').textContent = '生成 ' + DATA.generated + ' / ' + S.length + ' スナップショット';

const selA = document.getElementById('selA'), selB = document.getElementById('selB');
S.forEach((s, i) => {
  selA.add(new Option(s.stamp, i));
  selB.add(new Option(s.stamp, i));
});
selA.value = iA; selB.value = iB;
selA.onchange = () => { iA = +selA.value; render(); };
selB.onchange = () => { iB = +selB.value; render(); };
document.getElementById('swap').onclick = () => {
  [iA, iB] = [iB, iA]; selA.value = iA; selB.value = iB; render();
};
document.getElementById('q').oninput = e => { query = e.target.value.trim(); render(); };

document.getElementById('tabs').innerHTML = TABS.map(t =>
  `<div class="tab${t.id === tab ? ' on' : ''}" data-id="${t.id}">${t.label}</div>`).join('');
document.getElementById('tabs').onclick = e => {
  const el = e.target.closest('.tab');
  if (!el) return;
  const t = TABS.find(x => x.id === el.dataset.id);
  tab = t.id; sortKey = t.key; sortDir = t.dir;
  document.querySelectorAll('.tab').forEach(x => x.classList.toggle('on', x.dataset.id === tab));
  render();
};
document.getElementById('head').onclick = e => {
  const k = e.target.closest('th')?.dataset.k;
  if (!k) return;
  if (sortKey === k) sortDir = -sortDir; else { sortKey = k; sortDir = k === 'n' || k === 'cur' ? 1 : -1; }
  render();
};
document.getElementById('body').onclick = e => {
  const n = e.target.closest('tr')?.dataset.n;
  if (n) showPlayer(S.flatMap(s => s.entries).find(x => esc(x.n) === n)?.n || n);
};
const modal = document.getElementById('modal');
document.getElementById('mclose').onclick = () => modal.classList.add('hidden');
modal.onclick = e => { if (e.target === modal) modal.classList.add('hidden'); };
document.onkeydown = e => { if (e.key === 'Escape') modal.classList.add('hidden'); };

if (S.length < 2) {
  document.getElementById('note').innerHTML =
    '<div class="note">スナップショットが 1 件だけです。時間をおいてもう一度ランキングをコピーし、statusUP を実行すると変動が見られます。</div>';
}
render();
</script>
</body>
</html>
)HTML";

// data/report.html を書き出してパスを返す
static string writeReport(const vector<Snapshot> &snaps)
{
    string html = HTML_TEMPLATE;
    const string ph = "__DATA__";
    size_t at = html.find(ph);
    if (at == string::npos)
        throw runtime_error("テンプレートが壊れています");
    html.replace(at, ph.size(), buildJson(snaps));

    fs::create_directories(DATA_DIR);
    fs::path p = fs::absolute(fs::path(DATA_DIR) / "report.html");
    ofstream f(p, ios::binary);
    if (!f)
        throw runtime_error("書き込めません: " + p.string());
    f << html;
    return p.string();
}

// ----------------------------------------------------------------- コマンド

// 直近のスナップショットと中身が同じか(同じものを二重に取り込まないため)
static bool sameAsLatest(const vector<Entry> &entries)
{
    vector<fs::path> files = snapshotFiles();
    if (files.empty())
        return false;
    Snapshot last = loadSnapshot(files.back());
    if (last.entries.size() != entries.size())
        return false;
    for (size_t i = 0; i < entries.size(); i++)
        if (last.entries[i].name != entries[i].name || last.entries[i].power != entries[i].power)
            return false;
    return true;
}

// 取り込みの共通処理。取り込めなければ false
static bool importText(const string &text, const string &stamp, bool quiet)
{
    vector<string> warnings;
    Snapshot s;
    s.stamp = stamp.empty() ? nowStamp() : stamp;
    s.entries = parseRaw(text, warnings);

    if (s.entries.empty())
    {
        if (!quiet)
            cerr << "1 件も読み取れませんでした。形式を確認してください。\n";
        return false;
    }
    for (const string &w : warnings)
        cerr << "警告: " << w << "\n";

    if (sameAsLatest(s.entries))
    {
        cout << "直近のスナップショットと同じ内容のため、保存しませんでした。\n";
        return true;
    }
    saveSnapshot(s);
    cout << "  " << s.stamp << "  1位 " << s.entries.front().name << " ("
         << comma(s.entries.front().power) << ")  最下位 " << s.entries.back().name << " ("
         << comma(s.entries.back().power) << ")\n";
    return true;
}

static int cmdAdd(vector<string> args)
{
    string file, stamp;
    for (size_t i = 0; i < args.size(); i++)
    {
        if (args[i] == "--time" && i + 1 < args.size())
            stamp = args[++i];
        else if (file.empty())
            file = args[i];
    }

    string text;
    if (file.empty())
    {
        text = readClipboard(); // ファイル指定なし → クリップボードから
        if (trim(text).empty())
        {
            cerr << "クリップボードにテキストがありません。ランキング画面をコピーしてから実行してください。\n";
            return 1;
        }
        cout << "クリップボードから取り込みます。\n";
    }
    else
    {
        ifstream f(file, ios::binary);
        if (!f)
        {
            cerr << "読み込めません: " << file << "\n";
            return 1;
        }
        stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    return importText(text, stamp, false) ? 0 : 1;
}

static int cmdReport(vector<string> args)
{
    bool open = find(args.begin(), args.end(), "--no-open") == args.end();
    vector<fs::path> files = snapshotFiles();
    if (files.empty())
    {
        cerr << "スナップショットがありません。先にランキングをコピーして statusUP を実行してください。\n";
        return 1;
    }
    vector<Snapshot> snaps;
    for (const fs::path &p : files)
        snaps.push_back(loadSnapshot(p));

    string path = writeReport(snaps);
    cout << "レポート: " << path << "\n";
    if (open)
        openInBrowser(path);
    return 0;
}

// 引数なしで実行したとき: クリップボードを取り込んでレポートを開く
static int cmdQuick()
{
    string text = readClipboard();
    if (!trim(text).empty())
    {
        vector<string> warnings;
        vector<Entry> e = parseRaw(text, warnings);
        if (!e.empty())
            importText(text, "", true);
        else
            cout << "クリップボードにランキングが見当たらないため、取り込みは行いません。\n";
    }
    else
        cout << "クリップボードが空のため、取り込みは行いません。\n";

    if (snapshotFiles().empty())
    {
        cerr << "\nランキング画面をコピーしてから、もう一度実行してください。\n";
        return 1;
    }
    return cmdReport({});
}

static int cmdList()
{
    vector<fs::path> files = snapshotFiles();
    if (files.empty())
    {
        cout << "スナップショットがありません。まず statusUP add <rawfile> を実行してください。\n";
        return 0;
    }
    cout << "  #  日時                件数   1位\n";
    for (size_t i = 0; i < files.size(); i++)
    {
        Snapshot s = loadSnapshot(files[i]);
        cout << setw(3) << (i + 1) << "  " << padName(s.stamp, 18) << setw(5) << s.entries.size()
             << "   " << (s.entries.empty() ? "-" : s.entries.front().name) << "\n";
    }
    return 0;
}

static int cmdDiff(vector<string> args)
{
    int top = 20;
    vector<string> specs;
    for (size_t i = 0; i < args.size(); i++)
    {
        if (args[i] == "--top" && i + 1 < args.size())
            top = stoi(args[++i]);
        else if (args[i] == "--all")
            top = 0;
        else
            specs.push_back(args[i]);
    }

    Snapshot a, b;
    if (specs.size() >= 2)
    {
        a = resolveSnapshot(specs[0]);
        b = resolveSnapshot(specs[1]);
    }
    else
    {
        vector<fs::path> files = snapshotFiles();
        if (files.size() < 2)
        {
            cerr << "比較には 2 つ以上のスナップショットが必要です。\n";
            return 1;
        }
        if (specs.size() == 1)
        {
            b = resolveSnapshot(specs[0]);
            a = loadSnapshot(files[files.size() - 2]);
        }
        else
        {
            a = loadSnapshot(files[files.size() - 2]);
            b = loadSnapshot(files.back());
        }
    }

    vector<Row> rows = buildRows(a, b);
    double hours = hoursBetween(a.stamp, b.stamp);

    printSummary(a, b, rows, hours);
    printGainRanking(rows, hours, top);
    printRateRanking(rows, top);
    printRankMoves(rows);
    printAll(rows);
    return 0;
}

static int cmdHistory(vector<string> args)
{
    if (args.empty())
    {
        cerr << "使い方: statusUP history <name>\n";
        return 1;
    }
    const string &name = args[0];
    vector<fs::path> files = snapshotFiles();
    cout << "■ " << name << " の推移\n";
    cout << "  日時                順位        戦闘力        変動\n";
    bool found = false;
    long long prev = -1;
    for (const fs::path &p : files)
    {
        Snapshot s = loadSnapshot(p);
        auto it = find_if(s.entries.begin(), s.entries.end(),
                          [&](const Entry &e) { return e.name == name; });
        if (it == s.entries.end())
        {
            cout << "  " << padName(s.stamp, 18) << "  (圏外)\n";
            continue;
        }
        found = true;
        cout << "  " << padName(s.stamp, 18) << setw(4) << it->rank << setw(14) << comma(it->power)
             << setw(12) << (prev < 0 ? "-" : signedComma(it->power - prev)) << "\n";
        prev = it->power;
    }
    if (!found)
        cout << "  該当するプレイヤーが見つかりませんでした。\n";
    return 0;
}

static void usage()
{
    cout << "statusUP - ランキングの戦闘力・順位変動を集計する\n\n"
         << "  statusUP                              ランキングをコピーして実行するだけ。\n"
         << "                                        クリップボードから取り込み、レポートを開く\n\n"
         << "  statusUP add [rawfile] [--time \"YYYY-MM-DD HH:MM\"]  取り込みのみ (既定: クリップボード)\n"
         << "  statusUP report [--no-open]                          HTML レポートを作り直す\n"
         << "  statusUP list                                        取り込み済み一覧\n"
         << "  statusUP diff [A] [B] [--top N|--all]                2 時点を比較 (コンソール)\n"
         << "  statusUP history <name>                              1 人の推移を追う\n\n"
         << "  A / B は list の番号かファイル名で指定します。\n";
}

#ifdef _WIN32
// argv は既定で ANSI(CP932) のため、UTF-8 のデータと名前を突き合わせられない。
// コマンドラインをワイド文字で取り直して UTF-8 に変換する。
static vector<string> utf8Args()
{
    vector<string> out;
    int n = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wargv)
        return out;
    for (int i = 1; i < n; i++)
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        string s(len > 0 ? len - 1 : 0, '\0');
        if (len > 0)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(wargv);
    return out;
}
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    vector<string> args = utf8Args();
#else
    vector<string> args(argv + 1, argv + argc);
#endif
    try
    {
        if (args.empty())
            return cmdQuick(); // ダブルクリック実行を想定した一発モード
    }
    catch (const exception &e)
    {
        cerr << "エラー: " << e.what() << "\n";
        return 1;
    }
    string cmd = args[0];
    args.erase(args.begin());
    try
    {
        if (cmd == "add")
            return cmdAdd(args);
        if (cmd == "report")
            return cmdReport(args);
        if (cmd == "list")
            return cmdList();
        if (cmd == "diff")
            return cmdDiff(args);
        if (cmd == "history")
            return cmdHistory(args);
        usage();
        return 1;
    }
    catch (const exception &e)
    {
        cerr << "エラー: " << e.what() << "\n";
        return 1;
    }
}
