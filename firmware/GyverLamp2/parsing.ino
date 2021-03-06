void parsing() {
  if (Udp.parsePacket()) {
    static uint32_t tmr = 0;
    static char buf[UDP_TX_PACKET_MAX_SIZE + 1];

    int n = Udp.read(buf, UDP_TX_PACKET_MAX_SIZE);
    if (millis() - tmr < 500) return;  // принимаем посылки не чаще 2 раз в секунду
    tmr = millis();

    buf[n] = NULL;
    DEBUGLN(buf);   // пакет вида <ключ>,<канал>,<тип>,<дата1>,<дата2>...
    byte keyLen = strchr(buf, ',') - buf;     // indexof
    if (strncmp(buf, GL_KEY, keyLen)) return; // не наш ключ

    byte data[MAX_PRESETS * PRES_SIZE + 5];
    memset(data, 0, MAX_PRESETS * PRES_SIZE + 5);
    int count = 0;
    char *str, *p = buf + keyLen;  // сдвиг до даты
    char *ssid, *pass;

    while ((str = strtok_r(p, ",", &p)) != NULL) {
      uint32_t thisInt = atoi(str);
      data[count++] = (byte)thisInt;
      if (data[1] == 0) {
        if (count == 4) ssid = str;
        if (count == 5) pass = str;
      }
      if (data[1] == 1) {
        if (count == 15) cfg.length = thisInt;
        if (count == 16) cfg.width = thisInt;
        if (count == 17) cfg.GMT = byte(thisInt);
        if (count == 18) cfg.cityID = thisInt;
        if (count == 19) cfg.mqtt = byte(thisInt);
        if (count == 20) strcpy(cfg.mqttID, str);
        if (count == 21) strcpy(cfg.mqttHost, str);
        if (count == 22) cfg.mqttPort = thisInt;
        if (count == 23) strcpy(cfg.mqttLogin, str);
        if (count == 24) strcpy(cfg.mqttPass, str);
      }
    }

    // широковещательный запрос времени для local устройств в сети AP лампы
    if (data[0] == 0 && cfg.WiFimode && !gotNTP) {
      now.day = data[1];
      now.hour = data[2];
      now.min = data[3];
      now.sec = data[4];
      now.setMs(0);
    }

    if (data[0] != cfg.group) return;     // не наш адрес, выходим

    switch (data[1]) {  // тип 0 - control, 1 - config, 2 - effects, 3 - dawn, 4 - from master, 5 - palette
      case 0: DEBUGLN("Control"); blinkTmr.restart();
        switch (data[2]) {
          case 0: controlHandler(0); break;               // выкл
          case 1: controlHandler(1); break;               // вкл
          case 2: cfg.minLight = phot.getRaw(); break;    // мин яркость
          case 3: cfg.maxLight = phot.getRaw(); break;    // макс яркость
          case 4: changePreset(-1); break;                // пред пресет
          case 5: changePreset(1); break;                 // след пресет
          case 6: setPreset(data[3] - 1); break;          // конкретный пресет data[3]
          case 7: cfg.WiFimode = data[3]; EE_updCfgRst(); break;  // смена режима WiFi
          case 8: cfg.role = data[3]; break;              // смена роли
          case 9: cfg.group = data[3]; break;             // смена группы
          case 10:                                        // установка настроек WiFi
            strcpy(cfg.ssid, ssid);
            strcpy(cfg.pass, pass);
            break;
          case 11: EE_updCfgRst(); break;                 // рестарт
          case 12: if (gotNTP) {                          // OTA обновление, если есть интернет
              cfg.update = 1;
              EE_updCfg();
              FastLED.clear();
              FastLED.show();
              char OTA[60];
              mString ota(OTA, 60);
              ota.clear();
              ota += OTAhost;
              ota += OTAfile[data[3]];
              DEBUG("Update to ");
              DEBUGLN(OTA);
              delay(100);
              ESPhttpUpdate.update(OTA);
            } break;
          case 13:                                        // выключить через
            if (data[3] == 0) turnoffTmr.stop();
            else {
              fadeDown((uint32_t)data[3] * 60000ul);
            }
            break;
        }
        EE_updCfg();
        break;

      case 1: DEBUGLN("Config"); blinkTmr.restart();
        FOR_i(0, CFG_SIZE) {
          *((byte*)&cfg + i) = data[i + 2];   // загоняем в структуру
        }
        if (cfg.deviceType == GL_TYPE_STRIP) {
          if (cfg.length > MAX_LEDS) cfg.length = MAX_LEDS;
          cfg.width = 1;
        }
        if (cfg.length * cfg.width > MAX_LEDS) cfg.width = MAX_LEDS / cfg.length;
        ntp.setTimeOffset((cfg.GMT - 13) * 3600);
        FastLED.setMaxPowerInVoltsAndMilliamps(STRIP_VOLT, cfg.maxCur * 100);
        if (cfg.adcMode == GL_ADC_BRI) switchToPhot();
        else if (cfg.adcMode == GL_ADC_MIC) switchToMic();
        else disableADC();
        EE_updCfg();
        break;

      case 2: DEBUGLN("Preset");
        cfg.presetAmount = data[2];   // кол-во режимов
        FOR_j(0, cfg.presetAmount) {
          FOR_i(0, PRES_SIZE) {
            *((byte*)&preset + j * PRES_SIZE + i) = data[j * PRES_SIZE + i + 3]; // загоняем в структуру
          }
        }
        //if (!cfg.rotation) setPreset(data[cfg.presetAmount * PRES_SIZE + 3] - 1);
        setPreset(data[cfg.presetAmount * PRES_SIZE + 3] - 1);
        EE_updatePreset();
        //presetRotation(true); // форсировать смену режима
        holdPresTmr.restart();
        loading = true;
        break;

      case 3: DEBUGLN("Dawn"); blinkTmr.restart();
        FOR_i(0, (2 + 3 * 7)) {
          *((byte*)&dawn + i) = data[i + 2]; // загоняем в структуру
        }
        EE_updateDawn();
        break;

      case 4: DEBUGLN("From master");
        if (cfg.role == GL_SLAVE) {
          switch (data[2]) {
            case 0: fade(data[3]); break;     // вкл выкл
            case 1: setPreset(data[3]); break;    // пресет
            case 2: cfg.bright = data[3]; break;  // яркость
          }
          EE_updateCfg();
        }
        break;

      case 5: DEBUGLN("Palette"); blinkTmr.restart();
        FOR_i(0, 1 + 16 * 3) {
          *((byte*)&pal + i) = data[i + 2]; // загоняем в структуру
        }
        updPal();
        EE_updatePal();
        break;

      case 6: DEBUGLN("Time"); blinkTmr.restart();
        if (!cfg.WiFimode) {  // если мы AP
          now.day = data[2];
          now.hour = data[3];
          now.min = data[4];
        }
        gotTime = true;
        break;
    }
    FastLED.clear();    // на всякий случай
  }
}

void sendToSlaves(byte data1, byte data2) {
  if (cfg.role == GL_MASTER) {
    IPAddress ip = WiFi.localIP();
    ip[3] = 255;

    char reply[20];
    mString packet(reply, sizeof(reply));
    packet.clear();
    packet += GL_KEY;
    packet += ',';
    packet += cfg.group;
    packet += ",4,";
    packet += data1;
    packet += ',';
    packet += data2;

    DEBUG("Sending to Slaves: ");
    DEBUGLN(reply);

    FOR_i(0, 3) {
      Udp.beginPacket(ip, 8888);
      Udp.write(reply);
      Udp.endPacket();
      delay(10);
    }
  }
}
