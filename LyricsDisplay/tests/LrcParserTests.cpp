#include "../src/LrcParser.h"
#include "../src/LyricsJson.h"
#include <cstdio>
int main() {
  const auto lines = ParseLrc("[00:01]Seconds\n[00:02.5]Tenths\n[00:03.125]Millis\n[00:04.50][00:05.50]Repeat\n");
  if(lines.size()!=5 || lines[0].timestamp.count()!=1000 || lines[1].timestamp.count()!=2500 || lines[2].timestamp.count()!=3125) { std::puts("FAIL common LRC timestamps were discarded or timed incorrectly"); return 1; }
  std::puts("PASS seconds, tenths, centiseconds, milliseconds and repeated timestamps");
  const auto json = R"([{"syncedLyrics": null}, {"syncedLyrics" : "[00:01.250]Caf\u00e9 \uD83C\uDFB5\n[00:02]Say \"hi\"\\"}])";
  if (ExtractSyncedLyrics(json) != "[00:01.250]Caf\xC3\xA9 \xF0\x9F\x8E\xB5\n[00:02]Say \"hi\"\\") {
    std::puts("FAIL online JSON whitespace, Unicode or escaping"); return 1;
  }
  if(!ExtractSyncedLyrics(R"({"syncedLyrics":"\uD800"})").empty()) return 1;
  if(!ParseLrc("[00:60]Invalid\n[-1:02]Invalid\n[00:01.1234]Invalid\n").empty()) return 1;
  const auto bom=ParseLrc("\xEF\xBB\xBF[00:00.05]First\r\n[00:01.00]\r\n");
  if(bom.size()!=2 || bom[0].timestamp.count()!=50 || !bom[1].text.empty()) return 1;
  std::puts("PASS online JSON whitespace, Unicode, escaped text, malformed cues, BOM and blank cues");
}
