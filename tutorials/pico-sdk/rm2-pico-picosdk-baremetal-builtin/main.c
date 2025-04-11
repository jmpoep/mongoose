// Copyright (c) 2025 Cesanta Software Limited
// All rights reserved

#include "mongoose.h"
#include "hal.h"


#define WIFI_SSID "YOUR_WIFI_NETWORK_NAME"  // SET THIS!
#define WIFI_PASS "YOUR_WIFI_PASSWORD"      // SET THIS!


static const struct mg_tcpip_spi_ spi = {NULL, hwspecific_spi_begin, hwspecific_spi_end, hwspecific_spi_txn};

#ifndef CYW43_RESOURCE_ATTRIBUTE
#define CYW43_RESOURCE_ATTRIBUTE
#endif
#include "pico-sdk/lib/cyw43-driver/firmware/w43439A0_7_95_49_00_combined.h"
#include "pico-sdk/lib/cyw43-driver/firmware/wifi_nvram_43439.h"
static const struct mg_tcpip_driver_cyw_firmware fw = {
  (const uint8_t *)w43439A0_7_95_49_00_combined, (size_t)CYW43_WIFI_FW_LEN,
  (const uint8_t *)wifi_nvram_4343, (size_t)sizeof(wifi_nvram_4343),
  (const uint8_t *)(w43439A0_7_95_49_00_combined + sizeof(w43439A0_7_95_49_00_combined) - CYW43_CLM_LEN), (size_t)CYW43_CLM_LEN};

// mif user states
enum {AP, SCANNING, STOPPING_AP, CONNECTING, READY};
static unsigned int state;
static uint32_t s_ip, s_mask;


static void mif_fn(struct mg_tcpip_if *ifp, int ev, void *ev_data) {
  // TODO(): should we include this inside ifp ? add an fn_data ?
  if (ev == MG_TCPIP_EV_ST_CHG) {
    MG_INFO(("State change: %u", *(uint8_t *) ev_data));
  }
  switch(state) {
    case AP: // we are in AP mode, wait for a user connection to trigger a scan or a connection to a network
      if (ev == MG_TCPIP_EV_ST_CHG && *(uint8_t *) ev_data == MG_TCPIP_STATE_UP) {
        MG_INFO(("Access Point started"));
        s_ip = ifp->ip, ifp->ip = MG_IPV4(192, 168, 169, 1);
        s_mask = ifp->mask, ifp->mask = MG_IPV4(255, 255, 255, 0);
        ifp->enable_dhcp_client = false;
        ifp->enable_dhcp_server = true;
      } else if (ev == MG_TCPIP_EV_ST_CHG && *(uint8_t *) ev_data == MG_TCPIP_STATE_READY) {
        MG_INFO(("Access Point READY !"));

        // simulate user request to scan for networks
        bool res = mg_wifi_scan();
        MG_INFO(("Starting scan: %s", res ? "OK":"FAIL"));
        if (res) state = SCANNING;
      }
      break;
    case SCANNING:
      if (ev == MG_TCPIP_EV_WIFI_SCAN_RESULT) {
        struct mg_wifi_scan_bss_data *bss = (struct mg_wifi_scan_bss_data *) ev_data;
        MG_INFO(("BSS: %.*s (%u) (%M) %d dBm %u", bss->SSID.len, bss->SSID.buf, bss->channel, mg_print_mac, bss->BSSID, (int) bss->RSSI, bss->security));
      } else if (ev == MG_TCPIP_EV_WIFI_SCAN_END) {
        struct mg_tcpip_driver_cyw_data *d = (struct mg_tcpip_driver_cyw_data *) ifp->driver_data;
        MG_INFO(("Wi-Fi scan finished"));

        // simulate user selection of a network (1/2: stop AP)
        bool res = mg_wifi_ap_stop();
        MG_INFO(("Manually stopping AP: %s", res ? "OK":"FAIL"));
        if (res) state = STOPPING_AP;
        // else we have a hw/fw problem
      }
      break;
    case STOPPING_AP:
      if (ev == MG_TCPIP_EV_ST_CHG && *(uint8_t *) ev_data == MG_TCPIP_STATE_DOWN) {
        struct mg_tcpip_driver_cyw_data *d = (struct mg_tcpip_driver_cyw_data *) ifp->driver_data;
        d->apmode = false;

        // simulate user selection of a network (2/2: actual connect)
        bool res = mg_wifi_connect(d->ssid, d->pass);
        MG_INFO(("Manually connecting: %s", res ? "OK":"FAIL"));
        if (res) {
          state = CONNECTING;
          ifp->ip = s_ip;
          ifp->mask = s_mask;
          if (ifp->ip == 0) ifp->enable_dhcp_client = true;
          ifp->enable_dhcp_server = false;
        } // else manually start AP as below
      }
      break;
    case CONNECTING:
      if (ev == MG_TCPIP_EV_ST_CHG && *(uint8_t *) ev_data == MG_TCPIP_STATE_READY) {
        MG_INFO(("READY!"));
        state = READY;

        // simulate user code disconnection and go back to AP mode (1/2: disconnect)
        bool res = mg_wifi_disconnect();
        MG_INFO(("Manually disconnecting: %s", res ? "OK":"FAIL"));
      } else if (ev == MG_TCPIP_EV_WIFI_CONNECT_ERR) {
        MG_ERROR(("Wi-Fi connect failed"));
        // manually start AP as below
      }
      break;
    case READY:
      // go back to AP mode after a disconnection (simulation 2/2), you could retry
      if (ev == MG_TCPIP_EV_ST_CHG && *(uint8_t *) ev_data == MG_TCPIP_STATE_DOWN) {
        struct mg_tcpip_driver_cyw_data *d = (struct mg_tcpip_driver_cyw_data *) ifp->driver_data;
        bool res = mg_wifi_ap_start(d->apssid, d->appass, d->apchannel);
        MG_INFO(("Disconnected"));
        MG_INFO(("Manually starting AP: %s", res ? "OK":"FAIL"));
        if (res) {
          state = AP;
          d->apmode = true;
        }
      }
      break;
  }
}


static struct mg_tcpip_driver_cyw_data d = {
  (struct mg_tcpip_spi_ *)&spi, (struct mg_tcpip_driver_cyw_firmware *)&fw, WIFI_SSID, WIFI_PASS, "mongoose", "mongoose", 0, 0, 10, true, true};

int main(void) {
  // initialize stdio
  stdio_init_all();

  hwspecific_spi_init();

  state = d.apmode ? AP : CONNECTING;

  struct mg_mgr mgr;        // Initialise Mongoose event manager
  mg_mgr_init(&mgr);        // and attach it to the interface
  mg_log_set(MG_LL_DEBUG);  // Set log level

  // Initialise Mongoose network stack
  // Either set use_dhcp or enter a static config.
  // For static configuration, specify IP/mask/GW in network byte order
  struct mg_tcpip_if mif = {
      .ip = 0,
      .driver = (struct mg_tcpip_driver *)&mg_tcpip_driver_cyw,
      .driver_data = (struct mg_tcpip_driver_cyw_data*)&d,
      .fn = mif_fn,
//      .recv_queue.size = 8192
  };

  mg_tcpip_init(&mgr, &mif);
  MG_INFO(("Init done, starting main loop"));

  MG_INFO(("Initialising application..."));

  MG_INFO(("Starting event loop"));
  for (;;) {
    mg_mgr_poll(&mgr, 0);
  }

  return 0;
}
