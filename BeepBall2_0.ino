/*
  Senior Design project
  Xiao ESP32-C6 + MAX98357 — AP Web UI: Volume + Frequency + Mode(Continuous/Beep)
  - Pins: BCLK=D8(GPIO19), LRCLK=D9(GPIO20), DIN=D7(GPIO17)
  - UI made fore mobile in mind. 
*/

#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"

//Wi-Fi Access point settings (password and SSID)
const char* AP_SSID = "BeepBaseball";
const char* AP_PASS = "SuperSecure420-69";
// I really want this section to possibly be where the user can set this online.

/*I²S pins for the ESP32:
   -D7 or GPIO-17 is our data In pin,
   -LRCLK, BLCK:  Both pins are CLK pins to sync the data transmition. 
   In these lines were just declaring pin names, Also Constexpers is creating a java equivalent of a private var.(its an immutable CONSTANT)

*/
constexpr int I2S_BCLK_PIN  = 19; // D8
constexpr int I2S_LRCLK_PIN = 20; // D9 (
constexpr int I2S_DATA_PIN  = 17; // D7 

// OPTIONAL: drive MAX98357 SD (shutdown) HIGH via GPIO.
// According to documentation if WE hard-wire SD to 3V3, We need to set this to -1.
constexpr int HARD_MUTE_PIN = 6; // A6 (or -1 to disable)

//Audio Settings (our Format settings)
constexpr int   SAMPLE_RATE   = 22050;     // 22.05 kHz We can bump this up for a clearer tone this (44100 is double the samples so its smoother (might fix the beep error and clean waveform))
constexpr size_t AUDIO_CHUNK  = 256;       // frames per write this is a pretty standard number 

/* Battery Percent sense (optional) But I want to add
 ####need to add pin declaration for ADC pin on ESP32#####
 #### ALSO idk if this even works so this is all experimental #### Sean Brain wasn't present here
*/
constexpr int   BATTERY_ADC_PIN = -1;   // e.g., 2 for A0 on some boards, else -1
constexpr float ADC_REF_V      = 3.3f;  // approx
constexpr int   ADC_BITS       = 12;    // 12-bit ADC
constexpr float BAT_DIVIDER    = 2.0f;  // adjust to your resistor divider
constexpr float BATT_MIN_V     = 3.30f; // 0% point
constexpr float BATT_MAX_V     = 4.20f; // 100% point

WebServer server(80);// web serber hosted on port 80 this is standard for http , ######IMPORTANT#### Figure out if HTTPS is possible (Port: 443)

// Web App Defualt state 
volatile int   volPercent = 50;     //Volume percent range 0-100 (maybe make volume work logarithmically)
volatile int   freqHz        = 1000;   // range: 10-10000
volatile bool  muted         = true; // Starts up muted to not be annoying



//

// Mode: 0=Continuous, 1=Beep
volatile uint8_t modeSel = 0;
// Beep rate in tenths of Hz (5..100 = 0.5..10.0 Hz)
volatile int    beepRate_tenths = 20;  // default 2.0 Hz

// i forgot what this does tbh i need to write better code
volatile float phase = 0.0f;  // carrier phase
volatile float tLFO  = 0.0f;  // seconds for beep LFO

static i2s_chan_handle_t i2s_tx_chan = nullptr;


/* BATTERY PERCENT EXPLAINED

  This math might be wrong but logically this is just:
  Yo, does this board have an ADC pin specifically for battery? 
    if the battery ADC pin defual is -1 it returns -1.
    however if not this is skipped
  AnalogReadResolution: lets you set the ADC resolution which is like sampling the data into more bits for a finer resolution.
  Analog Read: converts voltage in into a bit by taking the 0v-5v logic zones, sampling it and this way we have a fine metric of voltage = a certain specific bit
     which lets us read the voltage via a voltage divider (2 resistor network, this is noted on the website but it should be 220k Ohm with another 1:2 ration resistor per website)
  Math Time!!!!
  Raw: is the raw value (currently its set to 12 bit resolution) so raw can exist 0-4095
    raw is multiplied by ADC_REF_V which is 3.3v / ((1 << ADC_BITS) - 1 ||||| ((1<<ADC_BITS)-1 =Total bits-1, this is just max bit size for 12 bit resolution. 
    essentially think of it like 1.65v comes in. for arguments sake you don't know the voltage
    So it comes in as 2045*3.3v/4095 = 1.647V which is close enough

  So that 1.64V value gets set as the v_ADC var and we multiplythat by BAT_DIVIDER which is just the ratio of thw resistor divider (documentation says 1:2) so ours is set to 2
      (1.65v*2) = 3.30,

  Pct: to find the percent we subtract that 3.3-BATT_MIN (BATT_MIN=3.3)---> 3.3-3.3v / (BATT_MAX_V - BATT_MIN_V); -----> 0/(4.2-3.3) = 0/0.9
      hypothetically I had 0.5/0.9 = 0.55 I would then return the 0.55 Percent 


    Why Experimental? So the ADC refrence voltage (ADC REf_V) is not garunteed 3.3v it actually has a crazy wide range of 2.4v-3.3V
      We can use this feature that may or not be released yet called "esp_adc_cal_characterize(...);" AND  "esp_adc_cal_raw_to_voltage(...);"
      however, idk if the C6 chipset supports this yet therefore we call it experimental. 

*/
int batteryPercent() {
  if (BATTERY_ADC_PIN < 0) return -1;
  analogReadResolution(ADC_BITS); // this just lets us set the bit Resolution for the ADC which can be changed to 10bits, 12, 14, 16 (depends on capability of board)
  int raw = analogRead(BATTERY_ADC_PIN); 
  float v_adc  = (raw * ADC_REF_V) / ((1 << ADC_BITS) - 1);
  float v_batt = v_adc * BAT_DIVIDER;
  float pct    = (v_batt - BATT_MIN_V) / (BATT_MAX_V - BATT_MIN_V);
  return (int)lroundf(fmaxf(0.0f, fminf(1.0f, pct)) * 100.0f);
}

void applyTone() {} // synthesis reads globals directly

/*
I optimized this HTML section for mobile decives, honestly I pretty much followed a tutorial so this section is pretty hobbled together since I figured its 3 sliders and adding text elements,
I'm thinking the future we can use some html template creator, we could learn it but i feel like its outside the scope of the class. 

*/
String htmlPage() {
  String page = R"====(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1"/>
<title>ESP Audio Control</title>
<style>
  :root { color-scheme: dark; --bg:#0b0f14; --panel:#121820; --accent:#4da3ff; --accent-2:#7fc7ff; --text:#e6edf3; --muted:#9aa7b3; --ok:#34d399; --warn:#f59e0b; }
  * { box-sizing:border-box; font-family: system-ui, -apple-system, Segoe UI, Roboto, Inter, sans-serif; }
  body { margin:0; background: radial-gradient(1200px 800px at 80% -20%, #14202c 0%, var(--bg) 55%) fixed, var(--bg); color:var(--text); }
  .wrap { max-width: 820px; margin: 4vh auto; padding: clamp(12px, 3vw, 24px); }
  .card { background: linear-gradient(180deg, rgba(255,255,255,.02), rgba(0,0,0,.2));
          border: 1px solid rgba(255,255,255,.06); border-radius: 16px; padding: clamp(14px, 3vw, 22px);
          box-shadow: 0 6px 20px rgba(0,0,0,.35); backdrop-filter: blur(6px); }
  h1 { font-size: clamp(1.05rem, 1.6rem, 1.35rem); margin: 0 0 10px; letter-spacing: .3px; display:flex; gap:10px; align-items:center; flex-wrap: wrap;}
  p  { margin: 0 0 14px; color: var(--muted); font-size: clamp(.9rem, 1rem, 1rem); }
  .row { display:grid; gap:12px; grid-template-columns: 1fr auto; align-items:center; margin:12px 0 18px;}
  @media (max-width:600px){ .row { grid-template-columns: 1fr; } }
  label { font-weight:600; color: var(--accent-2); font-size: clamp(.95rem, 1rem, 1.05rem);  }
  input[type="range"] { width: 100%; height: 36px; }
  .readout { min-width: 110px; background:#0f141b; color:var(--text);
             border:1px solid rgba(255,255,255,.08); border-radius:10px; padding:10px 12px; text-align:center; font-feature-settings:"tnum"; font-size:1rem; }
  .grid { display:grid; gap:16px; grid-template-columns: 1fr; }
  .btns { display:flex; gap:10px; flex-wrap:wrap; margin-top: 6px; }
  button, select { padding:12px 14px; border-radius:12px; border:1px solid rgba(255,255,255,.1);
                   background:#10161e; color:var(--text); cursor:pointer; font-weight:600; font-size:1rem; }
  button:hover { border-color: var(--accent); transform: translateY(-1px); }
  .primary{ background: linear-gradient(180deg, #132235, #0f1a27); border-color: rgba(77,163,255,.35); }
  .good{ border-color: rgba(52,211,153,.35); }
  .warn{ border-color: rgba(245,158,11,.35); }
  .pill { display:inline-flex; align-items:center; gap:6px; padding:.25rem .6rem; border:1px solid rgba(255,255,255,.08);
          border-radius:999px; color: var(--muted); font-size:.9rem;}
  .footer { margin-top:12px; font-size:.95rem; color:var(--muted); display:flex; justify-content:space-between; gap:10px; flex-wrap:wrap;}
  .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace; color:#c4d1de;}
  .hide { display:none; }
</style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>
        ESP Audio Control
        <span class="pill mono" id="status">Connecting...</span>
        <span class="pill mono">Battery: <span id="bat">n/a</span></span>
      </h1>
      <p>Control <strong>Volume</strong>, <strong>Frequency</strong>, and <strong>Mode</strong> (Continuous / Beep). Output via MAX98357 I²S amp.</p>

      <div class="grid">

        <div>
          <label for="vol">Volume</label>
          <div class="row">
            <input id="vol" type="range" min="0" max="100" value="50" step="1"/>
            <input id="volBox" class="readout" type="text" inputmode="numeric" value="50%" />
          </div>
        </div>

        <div>
          <label for="freq">Frequency</label>
          <div class="row">
            <input id="freq" type="range" min="100" max="10000" value="1000" step="1"/>
            <input id="freqBox" class="readout" type="text" inputmode="numeric" value="1000 Hz" />
          </div>
        </div>

        <div>
          <label for="mode">Mode</label>
          <div class="row">
            <select id="mode">
              <option value="0">Continuous</option>
              <option value="1">Beep</option>
            </select>
            <div id="beepControls" class="readout" style="display:flex; gap:10px; align-items:center; justify-content:space-between;">
              <span class="mono" style="opacity:.8;">Beep rate</span>
              <input id="rate" type="range" min="5" max="100" value="20" step="1" style="width: 180px;"/>
              <span id="rateLabel" class="mono">2.0 Hz</span>
            </div>
          </div>
        </div>

        <div class="btns">
          <button class="primary" id="apply">Apply</button>
          <button class="good" id="mute">Mute/Unmute</button>
          <button class="warn" id="stop">Stop</button>
        </div>
      </div>

      <div class="footer">
        <div>Frequency range: <span class="mono">100–10,000 Hz</span></div>
        <div>Device: <span class="mono" id="devName">ESP32-C6</span></div>
      </div>
    </div>
  </div>

<script>
const $ = (id)=>document.getElementById(id);
const statusEl=$("status"), vol=$("vol"), volBox=$("volBox"), freq=$("freq"), freqBox=$("freqBox");
const modeSel=$("mode"), rate=$("rate"), rateLabel=$("rateLabel"), beepControls=$("beepControls"), bat=$("bat");
const applyBtn=$("apply"), muteBtn=$("mute"), stopBtn=$("stop");

let state = { vol:50, freq:1000, muted:false, mode:0, rate:20, bat:-1 };

function showBeepUI(){
  beepControls.style.display = (state.mode===1) ? "flex" : "none";
}

function format(){
  volBox.value  = `${state.vol}%`;
  freqBox.value = `${state.freq} Hz`;
  vol.value     = state.vol;
  freq.value    = state.freq;

  modeSel.value = String(state.mode);
  rate.value    = state.rate;
  rateLabel.textContent = `${(state.rate/10).toFixed(1)} Hz`;
  bat.textContent = (state.bat>=0) ? `${state.bat}%` : "n/a";

  showBeepUI();
}

function clamp(n,min,max){ return Math.max(min, Math.min(max, n)); }

async function fetchJSON(url){ const r=await fetch(url,{cache:"no-store"}); if(!r.ok) throw new Error("net"); return await r.json(); }

async function init(){
  try{
    statusEl.textContent="Syncing…";
    const s = await fetchJSON("/get");
    state = s; format();
    statusEl.textContent = s.muted ? "Muted" : "Ready";

    setInterval(async ()=>{
      try { const s = await fetchJSON("/get"); state.bat = s.bat; bat.textContent = (state.bat>=0)?(`${state.bat}%`):"n/a"; } catch(_){}
    }, 5000);
  }catch(e){ statusEl.textContent="Offline?"; }
}

async function push(){
  const url = `/set?vol=${state.vol}&freq=${state.freq}&mode=${state.mode}&rate=${state.rate}`;
  try{
    statusEl.textContent="Updating…";
    const s = await fetchJSON(url);
    state = s; format();
    statusEl.textContent = s.muted ? "Muted" : "Ready";
  }catch(e){ statusEl.textContent="Error"; }
}

vol.addEventListener("input", ()=>{ state.vol=clamp(+vol.value,0,100); volBox.value=`${state.vol}%`; });
freq.addEventListener("input",()=>{ state.freq=clamp(+freq.value,100,10000); freqBox.value=`${state.freq} Hz`; });
volBox.addEventListener("change", ()=>{ const n=parseInt(volBox.value.replace(/[^\d]/g,"")||"0",10); state.vol=clamp(n,0,100); vol.value=state.vol; volBox.value=`${state.vol}%`; });
freqBox.addEventListener("change",()=>{ const n=parseInt(freqBox.value.replace(/[^\d]/g,"")||"0",10); state.freq=clamp(n,100,10000); freq.value=state.freq; freqBox.value=`${state.freq} Hz`; });

modeSel.addEventListener("change", ()=>{ state.mode=+modeSel.value|0; showBeepUI(); });
rate.addEventListener("input", ()=>{ state.rate = clamp(+rate.value,5,100); rateLabel.textContent = `${(state.rate/10).toFixed(1)} Hz`; });

applyBtn.addEventListener("click", ()=>{ state.muted=false; push(); });
muteBtn.addEventListener("click", async()=>{ try{ const s=await fetchJSON("/toggle_mute"); state=s; format(); statusEl.textContent=s.muted?"Muted":"Ready"; }catch(_){ statusEl.textContent="Error"; }});
stopBtn.addEventListener("click", async()=>{ try{ const s=await fetchJSON("/stop"); state=s; format(); statusEl.textContent="Stopped"; }catch(_){ statusEl.textContent="Error"; }});

init();
</script>
</body>
</html>)====";
  return page;
}

// HTTP handlers, again HTML is sketch and this is just json to allow the values from the sliders to pass as strings 
void sendState() {
  int batPct = batteryPercent();
  String json = String("{\"vol\":")+volumePercent+
                ",\"freq\":"+freqHz+
                ",\"muted\":"+(muted?"true":"false")+
                ",\"mode\":"+String((int)modeSel)+
                ",\"rate\":"+String(beepRate_tenths)+
                ",\"bat\":"+String(batPct)+
                "}";
  server.send(200, "application/json", json);
}

void handleRoot()       { server.send(200, "text/html; charset=utf-8", htmlPage()); }
void handleGet()        { sendState(); }
void handleSet() {
  //slider stuff
  if (server.hasArg("vol"))   volumePercent    = constrain(server.arg("vol").toInt(), 0, 100);
  if (server.hasArg("freq"))  freqHz           = constrain(server.arg("freq").toInt(), 100, 10000);
  if (server.hasArg("mode"))  modeSel          = (uint8_t)constrain(server.arg("mode").toInt(), 0, 1);
  if (server.hasArg("rate"))  beepRate_tenths  = constrain(server.arg("rate").toInt(), 5, 100);
  muted = false;
  applyTone();
  sendState();
}
void handleToggleMute() { muted = !muted; applyTone(); sendState(); }
void handleStop()       { muted = false; volumePercent = 0; sendState(); }


// I^2s initializers-
bool i2s_init() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan_cfg, &i2s_tx_chan, NULL) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_BCLK_PIN,
      .ws   = (gpio_num_t)I2S_LRCLK_PIN,
      .dout = (gpio_num_t)I2S_DATA_PIN,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv=false, .bclk_inv=false, .ws_inv=false } // flip to true only if needed
    }
  };
  std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT; // 32-bit slots
  std_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_LEFT;        // mono-left

  if (i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg) != ESP_OK) return false;
  if (i2s_channel_enable(i2s_tx_chan) != ESP_OK) return false;
  return true;
}

// basic set up
void setup() {
  Serial.begin(115200);
  delay(200);

  if (HARD_MUTE_PIN >= 0) {
    pinMode(HARD_MUTE_PIN, OUTPUT);
    digitalWrite(HARD_MUTE_PIN, HIGH); // enable amp
  }

  if (BATTERY_ADC_PIN >= 0) {
    pinMode(BATTERY_ADC_PIN, INPUT);
    analogReadResolution(ADC_BITS);
    (void)analogRead(BATTERY_ADC_PIN); // throw away first sample
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, false, 4);
  Serial.printf("AP SSID: %s  PASS: %s  IP: %s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());

  if (!i2s_init()) Serial.println("I2S init FAILED"); else Serial.println("I2S started");

  server.on("/",            HTTP_GET, handleRoot);
  server.on("/get",         HTTP_GET, handleGet);
  server.on("/set",         HTTP_GET, handleSet);
  server.on("/toggle_mute", HTTP_GET, handleToggleMute);
  server.on("/stop",        HTTP_GET, handleStop);
  server.begin();
  Serial.println("HTTP server started");
}

// main loop
void loop() {
  server.handleClient();
  if (!i2s_tx_chan) return;

  static int16_t buf[2 * AUDIO_CHUNK];

  const float twoPi = 6.28318530718f;
  const float dt    = 1.0f / (float)SAMPLE_RATE;

  // base amplitude from volume
  const float baseAmp = muted ? 0.0f : 32767.0f * (float)volumePercent / 100.0f;

  // Beep rate in Hz
  const float beepHz = (float)beepRate_tenths / 10.0f;

  for (size_t i = 0; i < AUDIO_CHUNK; ++i) {
    // Carrier frequency clamp
    float f = (float)freqHz;
    if (f < 100.0f) f = 100.0f;
    if (f > 10000.0f) f = 10000.0f;

    // controls 50% duty here
    float amp = baseAmp;
    if (modeSel == 1) { // if this beeps 
      float s = sinf(twoPi * beepHz * tLFO);
      if (s < 0.0f) amp = 0.0f; // make the gate 50%
    }

    // Here we generate a sample
    float dphi = twoPi * f * dt;
    int16_t smp = (int16_t)lroundf(sinf(phase) * amp);
    buf[2*i + 0] = smp; // L
    buf[2*i + 1] = smp; // R

    phase += dphi; if (phase >= twoPi) phase -= twoPi;
    tLFO  += dt;   if (tLFO > 3600.0f) tLFO = fmodf(tLFO, 1.0f);
  }

  size_t bytes_written = 0;
  i2s_channel_write(i2s_tx_chan, buf, sizeof(buf), &bytes_written, portMAX_DELAY);
}

