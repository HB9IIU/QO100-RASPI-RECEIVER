'use strict';

const demod_state_lookup = {
  0: "Initialising",
  1: "Hunting",
  2: "Header..",
  3: "Lock DVB-S",
  4: "Lock DVB-S2"
};

const modcod_lookup_dvbs = {
  4: "QPSK 1/2",
  5: "QPSK 3/5",
  6: "QPSK 2/3",
  7: "QPSK 3/4",
  9: "QPSK 5/6",
  10: "QPSK 6/7",
  11: "QPSK 7/8"
}

const modcod_lookup_dvbs2 = {
  0: "DummyPL",
  1: "QPSK 1/4",
  2: "QPSK 1/3",
  3: "QPSK 2/5",
  4: "QPSK 1/2",
  5: "QPSK 3/5",
  6: "QPSK 2/3",
  7: "QPSK 3/4",
  8: "QPSK 4/5",
  9: "QPSK 5/6",
  10: "QPSK 8/9",
  11: "QPSK 9/10",
  12: "8PSK 3/5",
  13: "8PSK 2/3",
  14: "8PSK 3/4",
  15: "8PSK 5/6",
  16: "8PSK 8/9",
  17: "8PSK 9/10",
  18: "16APSK 2/3",
  19: "16APSK 3/4",
  20: "16APSK 4/5",
  21: "16APSK 5/6",
  22: "16APSK 8/9",
  23: "16APSK 9/10",
  24: "32APSK 3/4",
  25: "32APSK 4/5",
  26: "32APSK 5/6",
  27: "32APSK 8/9",
  28: "32APSK 9/10"
}

const mpeg_type_lookup = {
  1: "MPEG1 Video",
  3: "MPEG1 Audio",
  15: "AAC Audio",
  16: "H.263 Video",
  27: "H.264 Video",
  33: "JPEG2K Video",
  36: "H.265 Video",
  129: "AC3 Audio"
}

// Source: https://wiki.batc.org.uk/MiniTiouner_Power_Level_Indication
const agc1_lookup = [
  [-39, 10],
  [-38, 21800],
  [-37, 25100],
  [-36, 27100],
  [-35, 28100],
  [-34, 28900],
  [-33, 29600],
  [-32, 30100],
  [-31, 30550],
  [-30, 31000],
  [-29, 31350],
  [-28, 31700],
  [-27, 32050],
  [-26, 32400],
  [-25, 32700],
  [-24, 33000],
  [-23, 33301],
  [-22, 33600],
  [-21, 33900],
  [-20, 34200],
  [-19, 34500],
  [-18, 34750],
  [-17, 35000],
  [-16, 35250],
  [-15, 35500],
  [-14, 35750],
  [-13, 36000],
  [-12, 36200],
  [-11, 36400],
  [-10, 36600],
  [-9, 36800],
  [-8, 37000],
  [-7, 37200],
  [-6, 37400],
  [-5, 37600],
  [-4, 37700]
];

// Source: https://wiki.batc.org.uk/MiniTiouner_Power_Level_Indication
const agc2_lookup = [
  [-67, 3200],
  [-66, 2740],
  [-65, 2560],
  [-64, 2380],
  [-63, 2200],
  [-62, 2020],
  [-61, 1840],
  [-60, 1660],
  [-59, 1480],
  [-58, 1300],
  [-57, 1140],
  [-56, 1000],
  [-55, 880],
  [-54, 780],
  [-53, 700],
  [-52, 625],
  [-51, 560],
  [-50, 500],
  [-49, 450],
  [-48, 400],
  [-47, 360],
  [-46, 325],
  [-45, 290],
  [-44, 255],
  [-43, 225],
  [-42, 200],
  [-41, 182],
  [-40, 164],
  [-39, 149],
  [-38, 148]
];

const rflevel_lookupfn = function(agc1_value, agc2_value)
{
  // Simple nearest-value selection.
  if(agc1_value > 1000)
  {
    let agc1_lookup_closest = agc1_lookup.reduce(function(prev, curr) {
      return (Math.abs(curr[1] - agc1_value) < Math.abs(prev[1] - agc1_value) ? curr : prev);
    });
    return agc1_lookup_closest[0];
  }
  else if(agc2_value > 150)
  {
    let agc2_lookup_closest = agc2_lookup.reduce(function(prev, curr) {
      return (Math.abs(curr[1] - agc2_value) < Math.abs(prev[1] - agc2_value) ? curr : prev);
    });
    return agc2_lookup_closest[0];
  }
  else
  {
    return -38;
  }
}
