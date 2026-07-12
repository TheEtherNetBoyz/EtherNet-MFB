#include "tpcm/InstrumentMap.h"

#include <cassert>
#include <string>

int main() {
    // Two zones on bank 11 prog 0, split at key 64
    // One zone on bank 11 prog 1 covering full range
    // One zone on bank 52 prog 5 covering only keys 40-80
    const std::string json = R"({
  "schema": 1,
  "zones": [
    {"bank": 11, "program": 0, "loKey": 0,  "hiKey": 63,  "waveId": 100},
    {"bank": 11, "program": 0, "loKey": 64, "hiKey": 127, "waveId": 101},
    {"bank": 11, "program": 1, "loKey": 0,  "hiKey": 127, "waveId": 200},
    {"bank": 52, "program": 5, "loKey": 40, "hiKey": 80,  "waveId": 300}
  ]
})";

    const tpcm::InstrumentMap map = tpcm::InstrumentMap::loadFromJson(json);

    assert(!map.empty());
    assert(map.zoneCount() == 4);

    // bank 11, prog 0 — lower zone
    assert(map.findWaveId(11, 0, 0)   == 100);
    assert(map.findWaveId(11, 0, 63)  == 100);
    // bank 11, prog 0 — upper zone
    assert(map.findWaveId(11, 0, 64)  == 101);
    assert(map.findWaveId(11, 0, 127) == 101);
    // bank 11, prog 1
    assert(map.findWaveId(11, 1, 0)   == 200);
    assert(map.findWaveId(11, 1, 60)  == 200);
    assert(map.findWaveId(11, 1, 127) == 200);
    // bank 52, prog 5 — within range
    assert(map.findWaveId(52, 5, 40)  == 300);
    assert(map.findWaveId(52, 5, 60)  == 300);
    assert(map.findWaveId(52, 5, 80)  == 300);
    // bank 52, prog 5 — outside range
    assert(map.findWaveId(52, 5, 39)  == 0xFFFF);
    assert(map.findWaveId(52, 5, 81)  == 0xFFFF);
    // Unknown bank
    assert(map.findWaveId(99, 0, 60)  == 0xFFFF);
    // Unknown program
    assert(map.findWaveId(11, 99, 60) == 0xFFFF);

    // zonesFor
    const auto z0 = map.zonesFor(11, 0);
    assert(z0.size() == 2);
    assert(z0[0].waveId == 100);
    assert(z0[1].waveId == 101);

    const auto z1 = map.zonesFor(11, 1);
    assert(z1.size() == 1);
    assert(z1[0].waveId == 200);

    const auto zNone = map.zonesFor(99, 0);
    assert(zNone.empty());

    // Game bank 11 prog 128-255 are stored as bank=11, program=128..255
    // (the export script maps SF2 bank 101 prog P -> game bank 11 prog P+128)
    // Verify the index key encoding handles prog > 127 correctly.
    const std::string jsonHigh = R"({
  "schema": 1,
  "zones": [
    {"bank": 11, "program": 200, "loKey": 0, "hiKey": 127, "waveId": 999}
  ]
})";
    const tpcm::InstrumentMap mapHigh = tpcm::InstrumentMap::loadFromJson(jsonHigh);
    assert(mapHigh.findWaveId(11, 200, 60) == 999);
    assert(mapHigh.findWaveId(11, 199, 60) == 0xFFFF);

    return 0;
}
