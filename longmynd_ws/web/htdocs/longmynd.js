const ws_longmynd_url = window.location.protocol == "https:" ? "wss:" : "ws:" + "//" + window.location.hostname + ":" + window.location.port + "/";

var ws_monitor_buffer = [];
var ws_control_buffer = [];

var ws_monitor = null;
var ws_control = null;

var render_busy = false;
var render_interval = 100;

var vlc_control_autoreset = true;
var vlc_control_autoreset_ip = '127.0.0.1';

var lo_frequency = 9750000;

var tuning_mode_scan = false;
var scan_signals_index = 0;
var scan_last_lock_timestamp = 0;

var demod_state_lookup = {
  0: "Initialising",
  1: "Hunting",
  2: "Header..",
  3: "Lock DVB-S",
  4: "Lock DVB-S2"
};

var modcod_lookup_dvbs = {
  4: "QPSK 1/2",
  5: "QPSK 3/5",
  6: "QPSK 2/3",
  7: "QPSK 3/4",
  9: "QPSK 5/6",
  10: "QPSK 6/7",
  11: "QPSK 7/8"
}

var modcod_lookup_dvbs2 = {
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

var mpeg_type_lookup = {
  1: "MPEG1 Video",
  3: "MPEG1 Audio",
  15: "AAC Audio",
  16: "H.263 Video",
  27: "H.264 Video",
  33: "JPEG2K Video",
  36: "H.265 Video",
  129: "AC3 Audio"
}

function signal_tune(_frequency, _symbolrate)
{
  longmynd_tune(_frequency, _symbolrate);
}

function longmynd_tune(_frequency, _symbolrate)
{
  _frequency = (_frequency * 1000) - lo_frequency;

  ws_control.sendMessage("C"+_frequency+","+_symbolrate);

  if(vlc_control_autoreset)
  {
    /* Reset locally-running VLC */
    /* Requires configuration in VLC:
     - 'Tools' -> 'Preferences'
     - Select 'Show Settings' -> 'All', in bottom left.
     - Navigate to 'Interface' -> 'Main Interfaces'
     - Tick 'Extra Interface Modules' -> 'Web'
     - Click 'Save' to exit
    */
    $.ajax({
      url: "http://"+vlc_control_autoreset_ip+":8080/requests/status.xml?command=pl_next"
    });
  }
}

function longmynd_lnbv(_enabled, _horizontal)
{
  ws_control.sendMessage("V"+(_enabled ? 1 : 0)+","+(_horizontal ? 1 : 0));

  if(_enabled && vlc_control_autoreset)
  {
    /* Reset locally-running VLC */
    /* Requires configuration in VLC:
     - 'Tools' -> 'Preferences'
     - Select 'Show Settings' -> 'All', in bottom left.
     - Navigate to 'Interface' -> 'Main Interfaces'
     - Tick 'Extra Interface Modules' -> 'Web'
     - Click 'Save' to exit
    */
    $.ajax({
      url: "http://"+vlc_control_autoreset_ip+":8080/requests/status.xml?command=pl_next"
    });
  }
}

function longmynd_rfport(_rfport_index)
{
  ws_control.sendMessage("T"+_rfport_index);

  if(vlc_control_autoreset)
  {
    /* Reset locally-running VLC */
    /* Requires configuration in VLC:
     - 'Tools' -> 'Preferences'
     - Select 'Show Settings' -> 'All', in bottom left.
     - Navigate to 'Interface' -> 'Main Interfaces'
     - Tick 'Extra Interface Modules' -> 'Web'
     - Click 'Save' to exit
    */
    $.ajax({
      url: "http://"+vlc_control_autoreset_ip+":8080/requests/status.xml?command=pl_next"
    });
  }
}

function longmynd_udpts(_udp_host, _udp_port)
{
  ws_control.sendMessage("U"+_udp_host+':'+_udp_port);

  if(vlc_control_autoreset)
  {
    /* Reset locally-running VLC */
    /* Requires configuration in VLC:
     - 'Tools' -> 'Preferences'
     - Select 'Show Settings' -> 'All', in bottom left.
     - Navigate to 'Interface' -> 'Main Interfaces'
     - Tick 'Extra Interface Modules' -> 'Web'
     - Click 'Save' to exit
    */
    $.ajax({
      url: "http://"+vlc_control_autoreset_ip+":8080/requests/status.xml?command=pl_next"
    });
  }
}

function load_settings()
{
  if(typeof(Storage) !== "undefined")
  {
    var storage_vlc_control_autoreset = localStorage.getItem("longmynd-vlc-control-autoreset");
    if(storage_vlc_control_autoreset != null)
    {
      try
      {
        var _vlc_control_autoreset = JSON.parse(storage_vlc_control_autoreset);
        vlc_control_autoreset = _vlc_control_autoreset["enabled"];
        vlc_control_autoreset_ip = _vlc_control_autoreset["ip"];

        $("#input-vlcautoreset-enable")[0].checked = vlc_control_autoreset;
        $("#input-vlcautoreset-ip").val(vlc_control_autoreset_ip);

        if (vlc_control_autoreset) {
          $('#input-vlcautoreset-ip').prop("readonly", false);
        } else {
          $('#input-vlcautoreset-ip').prop("readonly", true);
        }
      }
      catch(e)
      {
        console.log("Error parsing storage_vlc_control_autoreset!", e);
      }
    }
    var storage_lo_frequency = localStorage.getItem("longmynd-lo-frequency");
    if(storage_lo_frequency != null)
    {
      try
      {
        lo_frequency = JSON.parse(storage_lo_frequency);

        $("#input-frequency-lo").val(lo_frequency);
      }
      catch(e)
      {
        console.log("Error parsing storage_lo_frequency!", e);
      }
    }

    /* Save defaults even if we didn't load anything */
    save_settings();
  }
}

function save_settings()
{
  if(typeof(Storage) !== "undefined")
  {
    var _vlc_control_autoreset = {
      "enabled": vlc_control_autoreset,
      "ip": vlc_control_autoreset_ip
    };
    localStorage.setItem("longmynd-vlc-control-autoreset", JSON.stringify(_vlc_control_autoreset));

    localStorage.setItem("longmynd-lo-frequency", JSON.stringify(lo_frequency));
  }
}

function longmynd_render_status(data_json)
{
  var status_obj;
  var status_packet;

  /*
    {"type":"status","timestamp":1571256202.388,"packet":{"rx":{"demod_state":4,"frequency":742530,"symbolrate":1998138,
    "vber":0,"ber":1250,"mer":80,"modcod":6,"short_frame":false,"pilot_symbols":true,
    "constellation":[[221,227],[19,213],[35,44],[203,213],[51,62],[77,221],[229,219],[234,35],[199,57],[31,230],[216,210],[228,38],[24,221],[247,31],[230,207],[237,203]]},
    "ts":{"service_name":"A71A","service_provider_name":"QARS","null_ratio":0,"PIDs":[[257,27],[258,3]]}}}
  */

  try {
    status_obj = JSON.parse(data_json);
    if(status_obj != null)
    {
      rx_status = status_obj.packet.rx;

      console.log(rx_status);

      if(rx_status.demod_state == 2)
      {
        /* Header.. */
        $("#badge-state")
            .removeClass("badge-light badge-success")
            .addClass("badge badge-warning")
            .text(demod_state_lookup[rx_status.demod_state]);
      }
      else if(rx_status.demod_state > 2)
      {
        /* Demod! */
        $("#badge-state")
            .removeClass("badge-light badge-warning")
            .addClass("badge badge-success")
            .text(demod_state_lookup[rx_status.demod_state]);
      }
      else
      {
        /* Init / Hunt */
        $("#badge-state")
            .removeClass("badge-success badge-warning")
            .addClass("badge badge-light")
            .text(demod_state_lookup[rx_status.demod_state]);
      }

      $("#span-status-frequency").text((rx_status.frequency + lo_frequency)/1000.0 + "MHz");
      $("#span-status-symbolrate").text((rx_status.symbolrate / 1000.0)+"KS");

      if(rx_status.rfport == 0)
      {
        $("#button-port-bottom").removeClass("active");
        $("#button-port-bottom > input")[0].checked = false;

        $("#button-port-top").addClass("active")
        $("#button-port-top > input")[0].checked = true;
      }
      else if(rx_status.rfport == 1)
      {
        $("#button-port-top").removeClass("active");
        $("#button-port-top > input")[0].checked = false;

        $("#button-port-bottom").addClass("active")
        $("#button-port-bottom > input")[0].checked = true;
      }
      else
      {
        /* Input Unknown */
        $("#button-port-top").removeClass("active");
        $("#button-port-top > input")[0].checked = false;
        $("#button-port-bottom").removeClass("active");
        $("#button-port-bottom > input")[0].checked = false;
      }

      if(rx_status.lnb_voltage_enabled)
      {
        if(rx_status.lnb_voltage_polarisation_h)
        {
          $("#button-lnbv-0v").removeClass("active")
          $("#button-lnbv-0v > input")[0].checked = false;

          $("#button-lnbv-12v").removeClass("active")
          $("#button-lnbv-12v > input")[0].checked = false;

          $("#button-lnbv-18v").addClass("active")
          $("#button-lnbv-18v > input")[0].checked = true;
        }
        else
        {
          $("#button-lnbv-0v").removeClass("active")
          $("#button-lnbv-0v > input")[0].checked = false;

          $("#button-lnbv-12v").addClass("active")
          $("#button-lnbv-12v > input")[0].checked = true;

          $("#button-lnbv-18v").removeClass("active")
          $("#button-lnbv-18v > input")[0].checked = false;
        }
      }
      else
      {
        $("#button-lnbv-0v").addClass("active")
        $("#button-lnbv-0v > input")[0].checked = true;

        $("#button-lnbv-12v").removeClass("active")
        $("#button-lnbv-12v > input")[0].checked = false;

        $("#button-lnbv-18v").removeClass("active")
        $("#button-lnbv-18v > input")[0].checked = false;
      }

      if(rx_status.demod_state == 3) // DVB-S
      {
        $("#span-status-modcod").text(modcod_lookup_dvbs[rx_status.modcod]);
      }
      else if(rx_status.demod_state == 4) // DVB-S2
      {
        $("#span-status-modcod").text(modcod_lookup_dvbs2[rx_status.modcod]);
      }
      else
      {
        $("#span-status-modcod").text("");
      }
      $("#progressbar-mer").css('width', (rx_status.mer/3.1)+'%').attr('aria-valuenow', rx_status.mer).text(rx_status.mer/10.0+"dB");
      $("#progressbar-ber").css('width', (rx_status.ber)+'%').attr('aria-valuenow', rx_status.ber).text(rx_status.ber/10.0+"%");

      if(rx_status.errors_bch_count > 0)
      {
        if(rx_status.errors_bch_uncorrected == true)
        {
          $("#span-status-errors-bch")
            .removeClass("badge-warning badge-success")
            .addClass("badge badge-danger")
            .text(rx_status.errors_bch_count.toString().padStart(3, '\xa0'));
        }
        else
        {
          $("#span-status-errors-bch")
            .removeClass("badge-danger badge-success")
            .addClass("badge badge-warning")
            .text(rx_status.errors_bch_count.toString().padStart(3, '\xa0'));
        }
      }
      else
      {
        $("#span-status-errors-bch")
          .removeClass("badge-danger badge-warning")
          .addClass("badge badge-success")
          .text(rx_status.errors_bch_count.toString().padStart(3, '\xa0'));
      }

      $("#span-status-errors-ldpc")
        .addClass("badge badge-light")
        .text(rx_status.errors_ldpc_count.toString().padStart(4, '\xa0'));
      
      ts_status = status_obj.packet.ts;

      $("#span-status-name").text(ts_status.service_name);
      $("#span-status-provider").text(ts_status.service_provider_name);

      var ulTsPids = $('<ul />');
      for (pid in ts_status.PIDs) {
        $('<li />')
          .text(ts_status.PIDs[pid][0]+": "+mpeg_type_lookup[ts_status.PIDs[pid][1]])
          .appendTo(ulTsPids);
      }
      $("#div-ts-pids").empty();
      $("#div-ts-pids").append(ulTsPids);

      $("#progressbar-density").css('width', (100.0 - ts_status.null_ratio)+'%').attr('aria-valuenow', (100.0 - ts_status.null_ratio)).text((100.0 - ts_status.null_ratio)+"%");

      /* Scan mode */
      if(tuning_mode_scan && signals.length > 1)
      {
        /* Check if we're demodulating, and have a Service Name */
        if(rx_status.demod_state >= 3 && ts_status.service_name.length > 0)
        {
          scan_last_lock_timestamp = (Date.now());
          /* Check we don't have one already */
          var k;
          for(k = 0; k < signals_decoded.length; k++)
          {
            if(ts_status.service_name == signals_decoded[k].name
              || (signals_decoded[k].frequency > (rx_status.frequency + lo_frequency)/1000.0 - 0.06
                && signals_decoded[k].frequency < (rx_status.frequency + lo_frequency)/1000.0 + 0.06))
            {
              /* Update it */
              signals_decoded[k].name = ts_status.service_name;
              signals_decoded[k].frequency = (rx_status.frequency + lo_frequency)/1000.0;
              signals_decoded[k].last_seen = (Date.now());

              break;
            }
          }
          if(k == signals_decoded.length)
          {
            /* Not found, new or changed so add */
            signals_decoded.push({
              "name": ts_status.service_name,
              "frequency": (rx_status.frequency + lo_frequency)/1000.0,
              "last_seen": (Date.now())
            });
          }

          /* Advance to next signal */
          scan_signals_index = scan_signals_index + 1;
          if(scan_signals_index >= signals.length)
          {
            scan_signals_index = 0;
          }

          signal_tune(signals[scan_signals_index].frequency, signals[scan_signals_index].symbolrate);
        }
        else if(scan_last_lock_timestamp < ((Date.now())-6*1000))
        {
          /* More than 6s trying to find the station, move on */
          scan_last_lock_timestamp = (Date.now());

          signals_decoded.push({
              "name": "??",
              "frequency": (rx_status.frequency + lo_frequency)/1000.0,
              "last_seen": (Date.now())
            });

          /* Advance to next signal */
          scan_signals_index = scan_signals_index + 1;
          if(scan_signals_index >= signals.length)
          {
            scan_signals_index = 0;
          }

          signal_tune(signals[scan_signals_index].frequency, signals[scan_signals_index].symbolrate);
        }
      }
    }
  }
  catch(e)
  {
    console.log("Error parsing message!",e);
  }
}

var ip_address_regex =/^(?!0)(?!.*\.$)((1?\d?\d|25[0-5]|2[0-4]\d)(\.|$)){4}$/;
$(document).ready(function()
{
  load_settings();

  $('#input-frequency-lo').val(lo_frequency);
  $('#input-vlcautoreset').prop("checked", vlc_control_autoreset);
  $('#input-vlcautoreset-ip').val(vlc_control_autoreset_ip);


  $('#input-frequency-lo').keyup(function()
  {
    var inputInt = parseInt($('#input-frequency-lo').val());
    if(!isNaN(inputInt))
    {
      $('#input-frequency-lo').removeClass("is-invalid");
      lo_frequency = inputInt;
      save_settings();
    }
    else
    {
      $('#input-frequency-lo').addClass("is-invalid");
    }
  });

  $('#input-vlcautoreset-enable').change(function()
  {  
    if (this.checked) {
      vlc_control_autoreset = true;
      $('#input-vlcautoreset-ip').prop("readonly", false);
    } else {
      vlc_control_autoreset = false;
      $('#input-vlcautoreset-ip').prop("readonly", true);
    }
    save_settings();
  });

  $('#input-vlcautoreset-ip').keyup(function()
  {
    if(ip_address_regex.test($('#input-vlcautoreset-ip').val()))
    {
      $('#input-vlcautoreset-ip').removeClass("is-invalid");
      vlc_control_autoreset_ip = $('#input-vlcautoreset-ip').val();
      save_settings();
    }
    else
    {
      $('#input-vlcautoreset-ip').addClass("is-invalid");
    }
  });

  $('#button-tune-click').click(function()
  {  
    tuning_mode_scan = false;
    $("#button-tune-click").addClass("active")
    $("#button-tune-click > input")[0].checked = true;

    $("#button-tune-scan").removeClass("active")
    $("#button-tune-scan > input")[0].checked = false;
  });
  $('#button-tune-scan').click(function()
  {  
    tuning_mode_scan = true;
    $("#button-tune-scan").addClass("active")
    $("#button-tune-scan > input")[0].checked = true;

    $("#button-tune-click").removeClass("active")
    $("#button-tune-click > input")[0].checked = false;
  });

  $('#button-lnbv-0v').click(function()
  {  
    longmynd_lnbv(false, false);
  });
  $('#button-lnbv-12v').click(function()
  {  
    longmynd_lnbv(true, false);
  });
  $('#button-lnbv-18v').click(function()
  {  
    longmynd_lnbv(true, true);
  });

  $('#button-port-top').click(function()
  {  
    longmynd_rfport(0);
  });
  $('#button-port-bottom').click(function()
  {  
    longmynd_rfport(1);
  });

  $('#button-udpts-submit').click(function()
  {
    var udpts_host;
    var udpts_port;
    if(ip_address_regex.test($('#input-udpts-host').val()))
    {
      $('#input-udpts-host').removeClass("is-invalid");
      udpts_host = $('#input-udpts-host').val();

      if($('#input-udpts-port').val() < 65535 && $('#input-udpts-port').val() > 1024)
      {
        udpts_port = $('#input-udpts-port').val();

        longmynd_udpts(udpts_host, udpts_port);
      }
      else
      {
        $('#input-udpts-port').addClass("is-invalid");
      }
    }
    else
    {
      $('#input-udpts-host').addClass("is-invalid");
    }
  });

  ws_monitor = new strWebsocket(ws_longmynd_url, "monitor", ws_monitor_buffer);
  ws_control = new strWebsocket(ws_longmynd_url, "control", ws_control_buffer);

  /* Set up listener for websocket */
  render_timer = setInterval(function()
  {
    if(!render_busy)
    {
      render_busy = true;
      if(ws_monitor_buffer.length > 0)
      {
        /* Pull newest data off the buffer and render it */
        var data_frame = ws_monitor_buffer.pop();

        longmynd_render_status(data_frame);

        ws_monitor_buffer.length = 0;
      }
      render_busy = false;
    }
    else
    {
      console.log("Slow render blocking next frame, configured interval is ", render_interval);
    }
  }, render_interval);

  scan_cull_timer = setInterval(function()
  {
    /* Scan for old decoded and remove */
    var _ts_now = (Date.now()) - (30*1000);
    for(var k = 0; k < signals_decoded.length; k++)
    {
      if(signals_decoded[k].last_seen < (_ts_now - (30*1000))
        || (signals_decoded[k].name == "??" && (_ts_now - (5*1000))))
      {
        signals_decoded.splice(k, 1);
      }
    }
  }, 1000);
});

