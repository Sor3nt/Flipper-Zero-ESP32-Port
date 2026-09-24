/* Appended after the complete production radio module by the host runner. */
static void stage(void* context,const char* text) { CHECK(context && text[0]); ++stages; }
static void ble_ready(void) {
    esp_ble_gap_cb_param_t p={.scan_param_cmpl={.status=0}};
    ble_handler(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,&p);
}
static void ble_running(void) {
    esp_ble_gap_cb_param_t p={.scan_start_cmpl={.status=0}};
    ble_handler(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,&p);
}
int main(void) {
    FuriMessageQueue q={0}; WardriverRadio* r=wardriver_radio_alloc(&q,stage,&q);
    CHECK(r && wardriver_radio_start(r) && leased && registered && controller==2 && host==2);
    ble_ready(); CHECK(r->ble_params==1 && r->done==0); /* BLE cannot complete Wi-Fi. */
    wardriver_radio_poll(r,100);
    CHECK(scans==1 && ble_scans==1 && r->scanning && !r->ble_scanning);
    ble_running(); wardriver_radio_poll(r,110);
    CHECK(r->ble_scanning && r->scanning && !strcmp(r->status,"LISTENING"));
    wifi_event_sta_scan_done_t done={0}; wifi_handler(NULL,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);
    wardriver_radio_poll(r,1000); CHECK(collections==1 && !r->scanning && r->ble_scanning);
    esp_ble_gap_cb_param_t adv={.scan_rst={.search_evt=ESP_GAP_SEARCH_INQ_RES_EVT,.rssi=-50,
        .bda={0,1,2,3,4,5},.ble_addr_type=BLE_ADDR_TYPE_PUBLIC,.ble_adv={5,9,'T','e','s','t'},.adv_data_len=6}};
    ble_handler(ESP_GAP_BLE_SCAN_RESULT_EVT,&adv);
    CHECK(q.count==2 && q.rows[0].mode==WardriverWifi && q.rows[1].mode==WardriverBle);
    CHECK(!memcmp(q.rows[0].mac,q.rows[1].mac,6));
    /* Missing Wi-Fi completion retries without stopping BLE. */
    wardriver_radio_poll(r,1750); unsigned previous=scans;
    wardriver_radio_poll(r,16751); CHECK(r->retries==1 && !r->scanning && r->ble_scanning);
    wardriver_radio_poll(r,19750); CHECK(scans==previous);
    wardriver_radio_poll(r,19751); CHECK(scans==previous+1 && r->scanning);
    wifi_handler(NULL,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done); wardriver_radio_poll(r,20000);
    CHECK(!r->retries && r->ble_scanning);
    /* Repeated errors halt Wi-Fi, leaving BLE live with an explicit status. */
    scan_error=true;
    for(unsigned i=0;i<4;i++) wardriver_radio_poll(r,20750+3000*i);
    CHECK(r->halted && r->ble_scanning && !strcmp(r->status,"WIFI FAILED - BLE RUNNING"));
    previous=scans; wardriver_radio_poll(r,100000); CHECK(scans==previous);
    /* Quiesce must remove/drain both sets of callbacks before restoration. */
    wardriver_radio_quiesce(r);
    CHECK(!owner && !callback_count && !registered && !controller && !host && !restores && leased);
    CHECK(!r->records && r->quiesced && stages>=7);
    previous=stages; wardriver_radio_quiesce(r); CHECK(stages==previous);
    wardriver_radio_stop(r); CHECK(restores==1 && !leased);
    wardriver_radio_stop(r); CHECK(restores==1);
    /* Start again resets independent state. BLE init failure stays visible. */
    scan_error=false; ble_init_error=true;
    CHECK(wardriver_radio_start(r)); wardriver_radio_poll(r,100);
    CHECK(r->scanning && r->ble_halted && !strcmp(r->status,"BLE FAILED - WIFI RUNNING"));
    wardriver_radio_stop(r); CHECK(restores==2);
    /* BLE parameter/start failures do not stop the Wi-Fi sweep. */
    ble_init_error=false; CHECK(wardriver_radio_start(r));
    esp_ble_gap_cb_param_t failure={.scan_param_cmpl={.status=1}};
    ble_handler(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,&failure); wardriver_radio_poll(r,10);
    CHECK(r->ble_halted && r->scanning && !r->failed);
    wardriver_radio_stop(r);
    CHECK(wardriver_radio_start(r)); ble_ready(); wardriver_radio_poll(r,10);
    failure.scan_start_cmpl.status=1; ble_handler(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,&failure);
    wardriver_radio_poll(r,20); CHECK(r->ble_halted && r->scanning && !r->failed);
    wardriver_radio_stop(r);
    CHECK(wardriver_radio_start(r)); wardriver_radio_poll(r,10001);
    CHECK(r->ble_halted && r->scanning); /* BLE timeout remains independent. */
    worker_failure=true; r->scanning=false; r->next_scan=0;
    wardriver_radio_poll(r,11000); CHECK(!r->scanning && r->retries==1);
    worker_failure=false; wardriver_radio_stop(r); wardriver_radio_free(r);
    puts("PASS: complete radio backend runs passive Wi-Fi/BLE together, isolates callbacks/errors, retries, drains before restore, and reopens");
    return 0;
}
