#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <nrf24.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>

#define TAG           "jammer_beta_app"
#define HOLD_DELAY_MS 100
#define APP_REVISION  "1.5"

#define MAX_NRF24 4

typedef enum {
    MENU_BLUETOOTH,
    MENU_DRONE,
    MENU_WIFI,
    MENU_BLE,
    MENU_ZIGBEE,
    MENU_MISC,
    MENU_SETTINGS,
    MENU_INFO,
    MENU_COUNT
} MenuType;

typedef enum {
    WIFI_MODE_SELECT,
    WIFI_MODE_ALL,
    WIFI_MODE_COUNT
} WifiMode;

typedef enum {
    MISC_STATE_IDLE,
    MISC_STATE_SET_START,
    MISC_STATE_SET_STOP,
    MISC_STATE_ERROR,
    MISC_STATE_COUNT
} MiscState;

typedef enum {
    MISC_MODE_CHANNEL_SWITCHING,
    MISC_MODE_PACKET_SENDING,
    MISC_MODE_COUNT
} MiscMode;

typedef enum {
    BLUETOOTH_MODE_LIST,
    BLUETOOTH_MODE_RANDOM,
    BLUETOOTH_MODE_BRUTEFORCE,
    BLUETOOTH_MODE_COUNT
} BluetoothJamMethod;

typedef enum {
    DRONE_MODE_BRUTEFORCE,
    DRONE_MODE_RANDOM,
    DRONE_MODE_COUNT
} DroneJamMethod;

typedef enum {
    MODULES_MODE_SEPARATE,
    MODULES_MODE_TOGETHER,
    MODULES_MODE_COUNT
} ModulesMode;

typedef enum {
    SETTINGS_ITEM_SPI_MODE,
    SETTINGS_ITEM_MODULES_MODE,
    SETTINGS_ITEM_BLUETOOTH_METHOD,
    SETTINGS_ITEM_DRONE_METHOD,
    SETTINGS_ITEM_BT_DWELL,
    SETTINGS_ITEM_COUNT
} SettingsItem;

typedef enum {
    SPI_MODE_DEFAULT, // CS on PA4 (standard standalone NRF24)
    SPI_MODE_EXTRA, // CS on PC3 (2-in-1 NRF24+CC1101 module)
    SPI_MODE_COUNT
} SpiMode;

typedef struct {
    FuriMutex* mutex;
    NotificationApp* notifications;
    FuriThread* thread;
    ViewPort* view_port;

    bool is_running;
    bool is_stop;
    bool wifi_menu_active;
    bool show_jamming_started;
    bool wifi_channel_select;
    bool is_modules_connected;
    bool settings_menu_active;
    bool ble_menu_active;
    uint8_t ble_selected;

    MenuType current_menu;
    WifiMode wifi_mode;
    MiscState misc_state;
    MiscMode misc_mode;
    uint8_t wifi_channel;
    uint8_t misc_start;
    uint8_t misc_stop;

    SpiMode spi_mode;
    ModulesMode modules_mode;
    BluetoothJamMethod bluetooth_jam_method;
    DroneJamMethod drone_jam_method;

    SettingsItem selected_setting_item;

    InputKey held_key;
    uint32_t hold_counter;
    uint32_t last_up_press_time;
    uint32_t last_down_press_time;
    uint8_t up_press_count;
    uint8_t down_press_count;
    uint8_t len_modules;

    // Bluetooth "freeze": stop hopping and keep the carrier on one channel.
    bool bt_hold; // false = hopping, true = frozen on bt_hold_channel
    uint8_t bt_hold_channel; // held NRF channel (0..80 -> 2400..2480 MHz)
    uint8_t anim_frame; // frame counter for the broadcast animation

    bool info_active; // true when the Infos screen is shown
    uint8_t bt_dwell_idx; // index into bt_dwell_us[] (per-channel dwell)
} PluginState;

typedef enum {
    EVENT_TICK,
    EVENT_KEY,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} PluginEvent;

const NotificationSequence error_sequence = {
    &message_red_255,
    &message_vibro_on,
    &message_delay_250,
    &message_vibro_off,
    &message_red_0,
    NULL,
};

nrf24_device_t nrf24_dev[MAX_NRF24];

static const uint8_t bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0,  1,  2,  4, 6,
                                             8,  22, 24, 26, 28, 30, 74, 76, 78, 80};
static const uint8_t ble_channels[] = {2, 26, 80};
static const uint8_t zigbee_channels[] =
    {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26};

// Per-channel dwell for BT jamming (microseconds). 0 = fastest sweep.
static const uint16_t bt_dwell_us[] = {0, 100, 200, 400};
#define BT_DWELL_COUNT (sizeof(bt_dwell_us) / sizeof(bt_dwell_us[0]))

static const int ble_channels_count = sizeof(ble_channels) / sizeof(ble_channels[0]);
static const int zigbee_channels_count = sizeof(zigbee_channels) / sizeof(zigbee_channels[0]);

static void settings_save(PluginState* state) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);

    storage_simply_mkdir(storage, "/ext/apps_data/jammer");

    if(file_stream_open(
           stream, "/ext/apps_data/jammer/settings.txt", FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "spi_mode=%d\n", state->spi_mode);
        stream_write(stream, (uint8_t*)buffer, strlen(buffer));
        snprintf(buffer, sizeof(buffer), "modules_mode=%d\n", state->modules_mode);
        stream_write(stream, (uint8_t*)buffer, strlen(buffer));
        snprintf(buffer, sizeof(buffer), "bluetooth_jam_method=%d\n", state->bluetooth_jam_method);
        stream_write(stream, (uint8_t*)buffer, strlen(buffer));
        snprintf(buffer, sizeof(buffer), "drone_jam_method=%d\n", state->drone_jam_method);
        stream_write(stream, (uint8_t*)buffer, strlen(buffer));
        snprintf(buffer, sizeof(buffer), "bt_dwell=%d\n", state->bt_dwell_idx);
        stream_write(stream, (uint8_t*)buffer, strlen(buffer));
        file_stream_close(stream);
    }

    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

static void settings_load(PluginState* state) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);

    state->spi_mode = SPI_MODE_DEFAULT;
    state->modules_mode = MODULES_MODE_SEPARATE;
    state->bluetooth_jam_method = BLUETOOTH_MODE_LIST;
    state->drone_jam_method = DRONE_MODE_BRUTEFORCE;
    state->bt_dwell_idx = 0;

    if(file_stream_open(
           stream, "/ext/apps_data/jammer/settings.txt", FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t file_size = stream_size(stream);
        if(file_size > 0 && file_size < 1024) {
            uint8_t* file_buf = malloc(file_size + 1);
            memset(file_buf, 0, file_size + 1);
            size_t bytes_read = stream_read(stream, file_buf, file_size);

            if(bytes_read == file_size) {
                char* content = (char*)file_buf;
                char* line = content;

                while(line != NULL && *line != '\0') {
                    char* next_line = strchr(line, '\n');
                    if(next_line != NULL) {
                        *next_line = '\0';
                        next_line++;
                    }

                    if(strstr(line, "spi_mode=") != NULL) {
                        char* value = strchr(line, '=');
                        if(value != NULL) {
                            value++;
                            state->spi_mode = atoi(value);
                            if(state->spi_mode >= SPI_MODE_COUNT)
                                state->spi_mode = SPI_MODE_DEFAULT;
                        }
                    } else if(strstr(line, "modules_mode=") != NULL) {
                        char* value = strchr(line, '=');
                        if(value != NULL) {
                            value++;
                            state->modules_mode = atoi(value);
                            if(state->modules_mode >= MODULES_MODE_COUNT)
                                state->modules_mode = MODULES_MODE_SEPARATE;
                        }
                    } else if(strstr(line, "bluetooth_jam_method=") != NULL) {
                        char* value = strchr(line, '=');
                        if(value != NULL) {
                            value++;
                            state->bluetooth_jam_method = atoi(value);
                            if(state->bluetooth_jam_method >= BLUETOOTH_MODE_COUNT)
                                state->bluetooth_jam_method = BLUETOOTH_MODE_LIST;
                        }
                    } else if(strstr(line, "drone_jam_method=") != NULL) {
                        char* value = strchr(line, '=');
                        if(value != NULL) {
                            value++;
                            state->drone_jam_method = atoi(value);
                            if(state->drone_jam_method >= DRONE_MODE_COUNT)
                                state->drone_jam_method = DRONE_MODE_BRUTEFORCE;
                        }
                    } else if(strstr(line, "bt_dwell=") != NULL) {
                        char* value = strchr(line, '=');
                        if(value != NULL) {
                            value++;
                            state->bt_dwell_idx = atoi(value);
                            if(state->bt_dwell_idx >= BT_DWELL_COUNT) state->bt_dwell_idx = 0;
                        }
                    }
                    line = next_line;
                }
            }

            free(file_buf);
        }
        file_stream_close(stream);
    }

    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

static inline bool is_separate_mode(PluginState* state) {
    return state->modules_mode == MODULES_MODE_SEPARATE;
}

static void start_const_carrier(uint8_t len_modules) {
    for(uint8_t i = 0; i < len_modules; i++) {
        nrf24_startConstCarrier(&nrf24_dev[i], 6, 45);
    }
}

static void stop_const_carrier(uint8_t len_modules) {
    for(uint8_t i = 0; i < len_modules; i++) {
        nrf24_stopConstCarrier(&nrf24_dev[i]);
    }
}

static void write_channel_random_separate(uint8_t limit, uint8_t len_modules) {
    limit++;
    for(uint8_t i = 0; i < len_modules; i++) {
        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, (furi_hal_random_get() % limit));
    }
}

static void write_channel_random(uint8_t limit, uint8_t len_modules) {
    limit++;
    uint8_t ch = (furi_hal_random_get() % limit);
    for(uint8_t i = 0; i < len_modules; i++) {
        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
    }
}

static void write_channel_all_separate(uint8_t limit, uint8_t len_modules, uint8_t start) {
    for(uint8_t ch = start; ch <= limit; ch++) {
        uint8_t i = ch % len_modules;
        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
    }
}

static void write_channel_all(uint8_t limit, uint8_t len_modules, uint8_t start) {
    for(uint8_t ch = start; ch <= limit; ch++) {
        for(uint8_t i = 0; i < len_modules; i++) {
            nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
        }
    }
}

static void jam_bluetooth(PluginState* state) {
    start_const_carrier(state->len_modules);

    // One channel per loop iteration: keeps a well-defined "current channel"
    // (so a freeze parks exactly there) and reacts instantly to hold/stop.
    // In separate mode with several modules we do NOT round-robin a single
    // moving channel (that clusters the carriers on adjacent channels). Instead
    // we spread the N carriers evenly across the band so they jam N well-
    // separated channels at the same time -> maximum simultaneous coverage.
    uint8_t list_idx = 0; // index into bluetooth_channels for LIST method
    uint8_t bf_ch = 2; // sweeping channel for BRUTEFORCE method (BT band starts at 2)

    while(!state->is_stop) {
        if(state->bt_hold) {
            // Frozen: concentrate every module on the single held channel.
            for(uint8_t i = 0; i < state->len_modules; i++) {
                nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, state->bt_hold_channel);
            }
            furi_delay_ms(1); // parked: no need to spin the SPI bus flat out
            continue;
        }

        // Pick the next channel according to the selected method.
        // Bruteforce/random stay inside the real BT band: channels 2..80
        // (2402..2480 MHz). Channels 0-1 are outside BT and were wasted.
        uint8_t list_base = list_idx; // base index used to spread modules (LIST)
        uint8_t ch;
        if(state->bluetooth_jam_method == BLUETOOTH_MODE_RANDOM) {
            ch = 2 + (furi_hal_random_get() % 79); // 2..80
        } else if(state->bluetooth_jam_method == BLUETOOTH_MODE_BRUTEFORCE) {
            ch = bf_ch;
            bf_ch = (bf_ch >= 80) ? 2 : (bf_ch + 1);
        } else { // BLUETOOTH_MODE_LIST
            ch = bluetooth_channels[list_idx];
            list_idx = (list_idx + 1) % sizeof(bluetooth_channels);
        }

        // Remember where we are so a freeze parks on the current frequency.
        state->bt_hold_channel = ch;

        if(is_separate_mode(state) && state->len_modules > 1) {
            // Spread the N modules evenly across the band: each one holds a
            // distinct, well-separated channel, so N channels are jammed at once.
            uint8_t n = state->len_modules;
            for(uint8_t i = 0; i < n; i++) {
                uint8_t mch;
                if(state->bluetooth_jam_method == BLUETOOTH_MODE_RANDOM) {
                    mch = 2 + (furi_hal_random_get() % 79);
                } else if(state->bluetooth_jam_method == BLUETOOTH_MODE_BRUTEFORCE) {
                    mch = 2 + (((ch - 2) + i * (79 / n)) % 79);
                } else { // BLUETOOTH_MODE_LIST
                    uint8_t sz = sizeof(bluetooth_channels);
                    mch = bluetooth_channels[(list_base + i * (sz / n)) % sz];
                }
                nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, mch);
            }
        } else {
            for(uint8_t i = 0; i < state->len_modules; i++) {
                nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
            }
        }

        // Optional per-channel dwell: lets the PLL settle so the carrier
        // actually radiates on this channel before hopping away.
        uint16_t dwell = bt_dwell_us[state->bt_dwell_idx];
        if(dwell) furi_delay_us(dwell);
    }

    stop_const_carrier(state->len_modules);
}

static void jam_drone(PluginState* state) {
    start_const_carrier(state->len_modules);

    while(!state->is_stop) {
        if(is_separate_mode(state)) {
            if(state->drone_jam_method == DRONE_MODE_BRUTEFORCE) {
                write_channel_all_separate(125, state->len_modules, 0);
            } else if(state->drone_jam_method == DRONE_MODE_RANDOM) {
                write_channel_random_separate(125, state->len_modules);
            }
        } else {
            if(state->drone_jam_method == DRONE_MODE_BRUTEFORCE) {
                write_channel_all(125, state->len_modules, 0);
            } else if(state->drone_jam_method == DRONE_MODE_RANDOM) {
                write_channel_random(125, state->len_modules);
            }
        }
    }

    stop_const_carrier(state->len_modules);
}

static void jam_ble_advertising(PluginState* state) {
    uint8_t mac[] = {0xFF, 0xFF};
    uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};

    for(uint8_t i = 0; i < state->len_modules; i++) {
        nrf24_configure(&nrf24_dev[i], 2, mac, mac, 2, 1, true, true);
        nrf24_set_txpower(&nrf24_dev[i], 6);
        nrf24_set_tx_mode(&nrf24_dev[i]);
    }
    while(!state->is_stop) {
        if(is_separate_mode(state)) {
            for(uint8_t ch = 0; ch < ble_channels_count; ch++) {
                uint8_t i = ch % state->len_modules;
                nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ble_channels[ch]);
                nrf24_spi_trx(&nrf24_dev[i], tx, NULL, 3, nrf24_TIMEOUT);
            }
        } else {
            for(uint8_t ch = 0; ch < ble_channels_count; ch++) {
                for(uint8_t i = 0; i < state->len_modules; i++) {
                    nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ble_channels[ch]);
                    nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                }
            }
        }
    }
}

static void jam_ble_data(PluginState* state) {
    start_const_carrier(state->len_modules);

    while(!state->is_stop) {
        if(is_separate_mode(state)) {
            for(uint8_t ch = 2; ch <= 80; ch += 2) {
                uint8_t i = ch % state->len_modules;
                nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
            }
        } else {
            for(uint8_t ch = 2; ch <= 80; ch += 2) {
                for(uint8_t i = 0; i < state->len_modules; i++) {
                    nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                }
            }
        }
    }

    stop_const_carrier(state->len_modules);
}

static void jam_misc(PluginState* state) {
    if(state->misc_mode == MISC_MODE_CHANNEL_SWITCHING) {
        start_const_carrier(state->len_modules);

        while(!state->is_stop) {
            if(is_separate_mode(state)) {
                write_channel_all_separate(
                    state->misc_stop, state->len_modules, state->misc_start);
            } else {
                write_channel_all(state->misc_stop, state->len_modules, state->misc_start);
            }
        }

        stop_const_carrier(state->len_modules);
    } else {
        uint8_t mac[] = {0xFF, 0xFF};
        uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};

        for(uint8_t i = 0; i < state->len_modules; i++) {
            nrf24_configure(&nrf24_dev[i], 2, mac, mac, 2, state->misc_start, true, true);
            nrf24_set_txpower(&nrf24_dev[i], 6);
            nrf24_set_tx_mode(&nrf24_dev[i]);
        }

        while(!state->is_stop) {
            if(is_separate_mode(state)) {
                for(uint8_t ch = state->misc_start; ch <= state->misc_stop; ch++) {
                    uint8_t i = ch % state->len_modules;
                    nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                    nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                }
            } else {
                for(uint8_t ch = state->misc_start; ch <= state->misc_stop; ch++) {
                    for(uint8_t i = 0; i < state->len_modules; i++) {
                        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                        nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                    }
                }
            }
        }
    }
}

static void jam_wifi(PluginState* state) {
    uint8_t mac[] = {0xFF, 0xFF};
    uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};

    for(uint8_t i = 0; i < state->len_modules; i++) {
        nrf24_configure(&nrf24_dev[i], 2, mac, mac, 2, state->misc_start, true, true);
        nrf24_set_txpower(&nrf24_dev[i], 6);
        nrf24_set_tx_mode(&nrf24_dev[i]);
    }

    while(!state->is_stop) {
        if(state->wifi_mode == WIFI_MODE_ALL) {
            if(is_separate_mode(state)) {
                for(uint8_t channel = 0; channel <= 13 && !state->is_stop; channel++) {
                    for(uint8_t ch = (channel * 5) + 1;
                        ch <= (channel * 5) + 23 && !state->is_stop;
                        ch++) {
                        uint8_t i = ch % state->len_modules;
                        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                        nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                    }
                }
            } else {
                for(uint8_t channel = 0; channel <= 13 && !state->is_stop; channel++) {
                    for(uint8_t ch = (channel * 5) + 1;
                        ch <= (channel * 5) + 23 && !state->is_stop;
                        ch++) {
                        for(uint8_t i = 0; i < state->len_modules; i++) {
                            nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                            nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                        }
                    }
                }
            }
        } else {
            uint8_t wifi_tmp = state->wifi_channel - 1;
            if(is_separate_mode(state)) {
                for(uint8_t ch = (wifi_tmp * 5) + 1; ch <= (wifi_tmp * 5) + 23 && !state->is_stop;
                    ch++) {
                    uint8_t i = ch % state->len_modules;
                    nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                    nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                }
            } else {
                for(uint8_t ch = (wifi_tmp * 5) + 1; ch <= (wifi_tmp * 5) + 23 && !state->is_stop;
                    ch++) {
                    for(uint8_t i = 0; i < state->len_modules; i++) {
                        nrf24_write_reg(&nrf24_dev[i], REG_RF_CH, ch);
                        nrf24_spi_trx(&nrf24_dev[i], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                    }
                }
            }
        }
    }
}

static void jam_zigbee(PluginState* state) {
    uint8_t mac[] = {0xFF, 0xFF};
    uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};

    for(uint8_t i = 0; i < state->len_modules; i++) {
        nrf24_configure(&nrf24_dev[i], 2, mac, mac, 2, state->misc_start, true, true);
        nrf24_set_txpower(&nrf24_dev[i], 6);
        nrf24_set_tx_mode(&nrf24_dev[i]);
    }

    while(!state->is_stop) {
        if(is_separate_mode(state)) {
            for(uint8_t i = 0; i < zigbee_channels_count && !state->is_stop; i++) {
                for(uint8_t ch = 4 + 5 * (zigbee_channels[i] - 11);
                    ch <= (4 + 5 * (zigbee_channels[i] - 11)) + 2 && !state->is_stop;
                    ch++) {
                    uint8_t k = ch % state->len_modules;
                    nrf24_write_reg(&nrf24_dev[k], REG_RF_CH, ch);
                    nrf24_spi_trx(&nrf24_dev[k], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                }
            }
        } else {
            for(uint8_t i = 0; i < zigbee_channels_count && !state->is_stop; i++) {
                for(uint8_t ch = 4 + 5 * (zigbee_channels[i] - 11);
                    ch <= (4 + 5 * (zigbee_channels[i] - 11)) + 2 && !state->is_stop;
                    ch++) {
                    for(uint8_t k = 0; k < state->len_modules; k++) {
                        nrf24_write_reg(&nrf24_dev[k], REG_RF_CH, ch);
                        nrf24_spi_trx(&nrf24_dev[k], tx, NULL, sizeof(tx), nrf24_TIMEOUT);
                    }
                }
            }
        }
    }
}

static int32_t jam_thread(void* ctx) {
    PluginState* state = ctx;
    state->is_running = true;
    state->is_stop = false;
    state->bt_hold = false; // always start in hopping mode

    switch(state->current_menu) {
    case MENU_BLUETOOTH:
        jam_bluetooth(state);
        break;
    case MENU_DRONE:
        jam_drone(state);
        break;
    case MENU_WIFI:
        jam_wifi(state);
        break;
    case MENU_BLE:
        if(state->ble_selected == 0)
            jam_ble_advertising(state);
        else
            jam_ble_data(state);
        break;
    case MENU_ZIGBEE:
        jam_zigbee(state);
        break;
    case MENU_MISC:
        jam_misc(state);
        break;
    case MENU_SETTINGS:
    case MENU_INFO:
    case MENU_COUNT:
        break;
    default:
        break;
    }

    state->is_running = false;
    if(state->current_menu == MENU_MISC) {
        state->show_jamming_started = false;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// UI : tout est dessine au Canvas, aucun asset bitmap.
// ---------------------------------------------------------------------------

static const char* const menu_titles[MENU_COUNT] =
    {"Bluetooth", "Drone", "WiFi", "BLE", "Zigbee", "Misc", "Settings", "Infos"};

// Barre de titre inversee, avec un libelle optionnel aligne a droite.
static void draw_header(Canvas* canvas, const char* title, const char* right) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 10, title);
    if(right) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 124, 7, AlignRight, AlignCenter, right);
    }
    canvas_set_color(canvas, ColorBlack);
}

// Glyphe "emission" anime : un point emetteur et des anneaux qui grandissent.
static void draw_broadcast(Canvas* canvas, uint8_t cx, uint8_t cy, uint8_t frame) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_disc(canvas, cx, cy, 2);
    for(uint8_t k = 0; k < 3; k++) {
        uint8_t r = 4 + k * 4 + (frame % 4);
        canvas_draw_circle(canvas, cx, cy, r);
    }
}

// Ligne d'aide centree tout en bas de l'ecran.
static void draw_footer_hint(Canvas* canvas, const char* hint) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_line(canvas, 2, 53, 125, 53);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 59, AlignCenter, AlignCenter, hint);
}

// ---- menu principal : liste verticale ------------------------------------

static void render_main_menu(Canvas* canvas, PluginState* state) {
    char idx[8];
    snprintf(idx, sizeof(idx), "%d/%d", state->current_menu + 1, MENU_COUNT);
    draw_header(canvas, "nRF24 JAMMER", idx);

    const uint8_t row_h = 12;
    const uint8_t top = 15;
    const uint8_t visible = 4;
    static uint8_t scroll = 0;

    if(state->current_menu < scroll) {
        scroll = state->current_menu;
    } else if(state->current_menu >= scroll + visible) {
        scroll = state->current_menu - visible + 1;
    }

    for(uint8_t i = 0; i < visible && (i + scroll) < MENU_COUNT; i++) {
        uint8_t item = i + scroll;
        uint8_t y = top + i * row_h;
        bool sel = (item == state->current_menu);

        if(sel) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 2, y, 120, row_h - 1);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 8, y + 9, menu_titles[item]);
        if(sel) canvas_draw_str(canvas, 112, y + 9, ">");
        canvas_set_color(canvas, ColorBlack);
    }

    // Ascenseur fin sur le bord droit.
    canvas_draw_line(canvas, 125, 15, 125, 62);
    uint8_t bar_h = (48 * visible) / MENU_COUNT;
    if(bar_h < 4) bar_h = 4;
    uint8_t bar_y = 15 + (scroll * (48 - bar_h)) / (MENU_COUNT - visible);
    canvas_draw_box(canvas, 124, bar_y, 3, bar_h);
}

// ---- ecran "modules" (aucun module ou detection en cours) -----------------

static void render_module_status(Canvas* canvas, PluginState* state) {
    draw_header(canvas, "nRF24 JAMMER", NULL);
    canvas_set_color(canvas, ColorBlack);
    char b[32];
    if(state->len_modules == 0) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "No module");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 64, 44, AlignCenter, AlignCenter, "Connect an nRF24 module");
    } else {
        canvas_set_font(canvas, FontPrimary);
        snprintf(b, sizeof(b), "%d module(s)", state->len_modules);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, b);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, "Ready...");
    }
}

// ---- ecran de jamming actif (commun a toutes les bandes RF) ----------------

static void render_jamming(Canvas* canvas, PluginState* state) {
    draw_header(canvas, menu_titles[state->current_menu], "JAM");
    draw_broadcast(canvas, 22, 34, state->anim_frame);

    canvas_set_color(canvas, ColorBlack);
    const uint8_t x = 46;
    char b[24];

    switch(state->current_menu) {
    case MENU_BLUETOOTH:
        if(state->bt_hold) {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, x, 27, "HOLD");
            snprintf(b, sizeof(b), "%d MHz", 2400 + state->bt_hold_channel);
            canvas_draw_str(canvas, x, 40, b);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, x, 49, "Up/Dn: tune");
        } else {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, x, 30, "HOPPING");
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(
                canvas,
                x,
                44,
                state->bluetooth_jam_method == BLUETOOTH_MODE_LIST   ? "Method: List" :
                state->bluetooth_jam_method == BLUETOOTH_MODE_RANDOM ? "Method: Random" :
                                                                       "Method: Brute");
        }
        break;
    case MENU_DRONE:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, x, 30, "SWEEPING");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(
            canvas,
            x,
            44,
            state->drone_jam_method == DRONE_MODE_RANDOM ? "Random 0-125" : "Brute 0-125");
        break;
    case MENU_WIFI:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, x, 30, "JAMMING");
        canvas_set_font(canvas, FontSecondary);
        if(state->wifi_mode == WIFI_MODE_ALL) {
            canvas_draw_str(canvas, x, 44, "All channels");
        } else {
            snprintf(b, sizeof(b), "Channel %d", state->wifi_channel);
            canvas_draw_str(canvas, x, 44, b);
        }
        break;
    case MENU_BLE:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, x, 30, "JAMMING");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, x, 44, state->ble_selected == 0 ? "Advertising" : "Data ch");
        break;
    case MENU_ZIGBEE:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, x, 30, "JAMMING");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, x, 44, "16 channels");
        break;
    default:
        break;
    }

    if(state->current_menu == MENU_BLUETOOTH) {
        draw_footer_hint(
            canvas, state->bt_hold ? "OK: hop   Back: stop" : "OK: freeze   Back: stop");
    } else {
        draw_footer_hint(canvas, "Back: stop");
    }
}

// ---- ecran de jamming Misc -------------------------------------------------

static void render_misc_running(Canvas* canvas, PluginState* state) {
    draw_header(canvas, "Misc", "JAM");
    draw_broadcast(canvas, 22, 34, state->anim_frame);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 46, 28, "RUNNING");
    canvas_set_font(canvas, FontSecondary);
    char b[24];
    snprintf(b, sizeof(b), "CH %d - %d", state->misc_start, state->misc_stop);
    canvas_draw_str(canvas, 46, 42, b);
    draw_footer_hint(canvas, "Back: stop");
}

// ---- reglages Misc (start / stop / mode) -----------------------------------

static void render_misc_config(Canvas* canvas, PluginState* state) {
    draw_header(canvas, "Misc", NULL);
    canvas_set_color(canvas, ColorBlack);
    char b[32];
    const char* mode = (state->misc_mode == MISC_MODE_CHANNEL_SWITCHING) ? "Channel switching" :
                                                                           "Packet sending";

    if(state->misc_state == MISC_STATE_SET_START) {
        canvas_set_font(canvas, FontPrimary);
        snprintf(b, sizeof(b), "Start: %d", state->misc_start);
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, b);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 37, AlignCenter, AlignCenter, mode);
        draw_footer_hint(canvas, "Up/Dn set  L/R mode  OK");
    } else if(state->misc_state == MISC_STATE_SET_STOP) {
        canvas_set_font(canvas, FontPrimary);
        snprintf(b, sizeof(b), "Start %d  Stop %d", state->misc_start, state->misc_stop);
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, b);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 37, AlignCenter, AlignCenter, mode);
        draw_footer_hint(
            canvas, state->misc_stop > state->misc_start ? "OK: start" : "Stop must be > Start");
    } else if(state->misc_state == MISC_STATE_ERROR) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "Invalid range");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Start must be < Stop");
    }
}

// ---- selecteur WiFi --------------------------------------------------------

static void render_wifi_menu(Canvas* canvas, PluginState* state) {
    draw_header(canvas, "WiFi", NULL);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas,
        64,
        30,
        AlignCenter,
        AlignCenter,
        state->wifi_mode == WIFI_MODE_ALL ? "All channels" : "Select channel");
    draw_footer_hint(canvas, "Up/Dn: mode   OK: next");
}

static void render_wifi_channel_select(Canvas* canvas, PluginState* state) {
    draw_header(canvas, "WiFi Channel", NULL);
    canvas_set_color(canvas, ColorBlack);
    char b[16];
    snprintf(b, sizeof(b), "CH %d", state->wifi_channel);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, b);
    draw_footer_hint(canvas, "Up/Dn: ch   OK: start");
}

// ---- selecteur BLE ---------------------------------------------------------

static void render_ble_menu(Canvas* canvas, PluginState* state) {
    draw_header(canvas, "BLE", NULL);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas,
        64,
        30,
        AlignCenter,
        AlignCenter,
        state->ble_selected == 0 ? "Advertising ch" : "Data channels");
    draw_footer_hint(canvas, "Up/Dn: type   OK: start");
}

// ---- liste des reglages ----------------------------------------------------

static void render_settings_menu(Canvas* canvas, PluginState* state) {
    draw_header(canvas, "Settings", NULL);

    const uint8_t row_h = 12;
    const uint8_t top = 15;
    const uint8_t visible = 4;
    const uint8_t total = SETTINGS_ITEM_COUNT;
    static uint8_t scroll = 0;

    if(state->selected_setting_item < scroll) {
        scroll = state->selected_setting_item;
    } else if(state->selected_setting_item >= scroll + visible) {
        scroll = state->selected_setting_item - visible + 1;
    }

    for(uint8_t i = 0; i < visible && (i + scroll) < total; i++) {
        uint8_t item = i + scroll;
        uint8_t y = top + i * row_h;
        bool sel = (item == state->selected_setting_item);

        if(sel) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 2, y, 124, row_h - 1);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        canvas_set_font(canvas, FontSecondary);

        const char* label = "";
        const char* value = "";
        char dwell_buf[8];
        switch(item) {
        case SETTINGS_ITEM_SPI_MODE:
            label = "SPI Pin";
            value = (state->spi_mode == SPI_MODE_DEFAULT) ? "Default 4" : "Extra 7";
            break;
        case SETTINGS_ITEM_MODULES_MODE:
            label = "Modules";
            value = (state->modules_mode == MODULES_MODE_SEPARATE) ? "Separate" : "Together";
            break;
        case SETTINGS_ITEM_BLUETOOTH_METHOD:
            label = "Bluetooth";
            value = (state->bluetooth_jam_method == BLUETOOTH_MODE_LIST)   ? "List" :
                    (state->bluetooth_jam_method == BLUETOOTH_MODE_RANDOM) ? "Random" :
                                                                             "Brute";
            break;
        case SETTINGS_ITEM_DRONE_METHOD:
            label = "Drone";
            value = (state->drone_jam_method == DRONE_MODE_RANDOM) ? "Random" : "Brute";
            break;
        case SETTINGS_ITEM_BT_DWELL:
            label = "BT Dwell";
            snprintf(dwell_buf, sizeof(dwell_buf), "%dus", bt_dwell_us[state->bt_dwell_idx]);
            value = dwell_buf;
            break;
        default:
            break;
        }
        canvas_draw_str(canvas, 6, y + 9, label);
        canvas_draw_str(canvas, 66, y + 9, value);
        canvas_set_color(canvas, ColorBlack);
    }
}

// ---- ecran Infos -----------------------------------------------------------

static void render_info(Canvas* canvas) {
    draw_header(canvas, "Infos", NULL);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, "nRF24 Jammer");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 37, AlignCenter, AlignCenter, "Dev: Mathias s0ca");
    canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignCenter, "Revision " APP_REVISION);
    draw_footer_hint(canvas, "Back");
}

// ---- dispatcher de rendu ---------------------------------------------------

static void render_callback(Canvas* canvas, void* ctx) {
    PluginState* state = ctx;
    canvas_clear(canvas);

    if(!state->is_modules_connected) {
        render_module_status(canvas, state);
    } else if(state->current_menu == MENU_MISC && state->show_jamming_started) {
        render_misc_running(canvas, state);
    } else if(state->is_running) {
        render_jamming(canvas, state);
    } else if(state->current_menu == MENU_SETTINGS && state->settings_menu_active) {
        render_settings_menu(canvas, state);
    } else if(state->current_menu == MENU_MISC && state->misc_state != MISC_STATE_IDLE) {
        render_misc_config(canvas, state);
    } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
        if(state->wifi_channel_select) {
            render_wifi_channel_select(canvas, state);
        } else {
            render_wifi_menu(canvas, state);
        }
    } else if(state->current_menu == MENU_BLE && state->ble_menu_active) {
        render_ble_menu(canvas, state);
    } else if(state->current_menu == MENU_INFO && state->info_active) {
        render_info(canvas);
    } else {
        render_main_menu(canvas, state);
    }
}

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    PluginEvent plugin_event = {.type = EVENT_KEY, .input = *event};
    furi_message_queue_put(queue, &plugin_event, FuriWaitForever);
}

static void handle_settings_input(PluginState* state, InputKey key, bool is_hold) {
    uint8_t increment = 1;

    if(!is_hold) {
        if(key == InputKeyUp) {
            uint32_t now = furi_get_tick();
            if(now - state->last_up_press_time < 200) {
                state->up_press_count++;
            } else {
                state->up_press_count = 1;
            }
            state->last_up_press_time = now;

            if(state->up_press_count == 2)
                increment = 9;
            else if(state->up_press_count >= 3)
                increment = 90;
        } else if(key == InputKeyDown) {
            uint32_t now = furi_get_tick();
            if(now - state->last_down_press_time < 200) {
                state->down_press_count++;
            } else {
                state->down_press_count = 1;
            }
            state->last_down_press_time = now;

            if(state->down_press_count == 2)
                increment = 9;
            else if(state->down_press_count >= 3)
                increment = 90;
        }
    }

    if(key == InputKeyUp) {
        if(state->misc_state == MISC_STATE_SET_START) {
            if(state->misc_start + increment <= 125) {
                state->misc_start += increment;
            }
        } else if(state->misc_state == MISC_STATE_SET_STOP) {
            if(state->misc_stop + increment <= 125) {
                state->misc_stop += increment;
            }
        }
    } else if(key == InputKeyDown) {
        if(state->misc_state == MISC_STATE_SET_START) {
            if(state->misc_start >= increment) {
                state->misc_start -= increment;
            }
        } else if(state->misc_state == MISC_STATE_SET_STOP) {
            if(state->misc_stop >= increment) {
                state->misc_stop -= increment;
            }
        }
    }

    if(state->misc_state == MISC_STATE_SET_STOP && state->misc_stop <= state->misc_start) {
        if(state->misc_start + 1 <= 125) {
            state->misc_stop = state->misc_start + 1;
        }
    }
}

static void handle_menu_input(PluginState* state, InputKey key) {
    if(key == InputKeyDown) {
        state->current_menu = (state->current_menu + 1) % MENU_COUNT;
    } else if(key == InputKeyUp) {
        state->current_menu = (state->current_menu == 0) ? (MENU_COUNT - 1) :
                                                           (state->current_menu - 1);
    }

    state->misc_state = MISC_STATE_IDLE;
    state->wifi_menu_active = false;
    state->wifi_channel_select = false;
    state->settings_menu_active = false;
    state->ble_menu_active = false;
    state->info_active = false;
    state->selected_setting_item = SETTINGS_ITEM_SPI_MODE;
}

static void handle_settings_menu_input(PluginState* state, InputKey key) {
    switch(key) {
    case InputKeyUp:
        if(state->selected_setting_item == 0) {
            state->selected_setting_item = SETTINGS_ITEM_COUNT - 1;
        } else {
            state->selected_setting_item--;
        }
        break;
    case InputKeyDown:
        state->selected_setting_item = (state->selected_setting_item + 1) % SETTINGS_ITEM_COUNT;
        break;
    case InputKeyLeft:
        if(state->selected_setting_item == SETTINGS_ITEM_SPI_MODE) {
            if(state->spi_mode == 0) {
                state->spi_mode = SPI_MODE_COUNT - 1;
            } else {
                state->spi_mode--;
            }
        } else if(state->selected_setting_item == SETTINGS_ITEM_MODULES_MODE) {
            if(state->modules_mode == 0) {
                state->modules_mode = MODULES_MODE_COUNT - 1;
            } else {
                state->modules_mode--;
            }
        } else if(state->selected_setting_item == SETTINGS_ITEM_BLUETOOTH_METHOD) {
            if(state->bluetooth_jam_method == 0) {
                state->bluetooth_jam_method = BLUETOOTH_MODE_COUNT - 1;
            } else {
                state->bluetooth_jam_method--;
            }
        } else if(state->selected_setting_item == SETTINGS_ITEM_DRONE_METHOD) {
            if(state->drone_jam_method == 0) {
                state->drone_jam_method = DRONE_MODE_COUNT - 1;
            } else {
                state->drone_jam_method--;
            }
        } else if(state->selected_setting_item == SETTINGS_ITEM_BT_DWELL) {
            if(state->bt_dwell_idx == 0) {
                state->bt_dwell_idx = BT_DWELL_COUNT - 1;
            } else {
                state->bt_dwell_idx--;
            }
        }
        break;
    case InputKeyRight:
        if(state->selected_setting_item == SETTINGS_ITEM_SPI_MODE) {
            state->spi_mode = (state->spi_mode + 1) % SPI_MODE_COUNT;
        } else if(state->selected_setting_item == SETTINGS_ITEM_MODULES_MODE) {
            state->modules_mode = (state->modules_mode + 1) % MODULES_MODE_COUNT;
        } else if(state->selected_setting_item == SETTINGS_ITEM_BLUETOOTH_METHOD) {
            state->bluetooth_jam_method = (state->bluetooth_jam_method + 1) % BLUETOOTH_MODE_COUNT;
        } else if(state->selected_setting_item == SETTINGS_ITEM_DRONE_METHOD) {
            state->drone_jam_method = (state->drone_jam_method + 1) % DRONE_MODE_COUNT;
        } else if(state->selected_setting_item == SETTINGS_ITEM_BT_DWELL) {
            state->bt_dwell_idx = (state->bt_dwell_idx + 1) % BT_DWELL_COUNT;
        }
        break;
    default:
        break;
    }
}

static void handle_wifi_input(PluginState* state, InputKey key) {
    switch(key) {
    case InputKeyUp:
    case InputKeyRight:
        state->wifi_channel = (state->wifi_channel % 14) + 1;
        break;
    case InputKeyDown:
    case InputKeyLeft:
        state->wifi_channel = (state->wifi_channel == 1) ? 14 : (state->wifi_channel - 1);
        break;
    default:
        break;
    }
}

int32_t jammer_beta_app(void* p) {
    UNUSED(p);
    PluginState* state = malloc(sizeof(PluginState));
    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(PluginEvent));

    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->notifications = furi_record_open(RECORD_NOTIFICATION);

    settings_load(state);

    state->is_running = false;
    state->is_stop = true;
    state->is_modules_connected = false;
    state->wifi_menu_active = false;
    state->wifi_channel_select = false;
    state->show_jamming_started = false;
    state->settings_menu_active = false;
    state->ble_menu_active = false;
    state->ble_selected = 0;
    state->current_menu = MENU_BLUETOOTH;
    state->wifi_mode = WIFI_MODE_ALL;
    state->misc_state = MISC_STATE_IDLE;
    state->misc_mode = MISC_MODE_CHANNEL_SWITCHING;
    state->wifi_channel = 1;
    state->misc_start = 0;
    state->misc_stop = 0;
    state->selected_setting_item = SETTINGS_ITEM_SPI_MODE;
    state->len_modules = 0;
    state->held_key = InputKeyMAX;
    state->hold_counter = 0;
    state->last_up_press_time = 0;
    state->last_down_press_time = 0;
    state->up_press_count = 0;
    state->down_press_count = 0;
    state->bt_hold = false;
    state->bt_hold_channel = 40; // 2440 MHz, centre de la bande, valeur par defaut
    state->anim_frame = 0;
    state->info_active = false;
    // bt_dwell_idx is loaded by settings_load() above

    if(!furi_hal_power_is_otg_enabled()) furi_hal_power_enable_otg();

    Gui* gui = furi_record_open(RECORD_GUI);
    state->view_port = view_port_alloc();
    view_port_draw_callback_set(state->view_port, render_callback, state);
    view_port_input_callback_set(state->view_port, input_callback, queue);
    gui_add_view_port(gui, state->view_port, GuiLayerFullscreen);

    state->thread = furi_thread_alloc_ex("nRFJammer", 1024, jam_thread, state);

    if(state->spi_mode == SPI_MODE_EXTRA) {
        // 2-in-1 NRF24+CC1101 module: only 1 NRF24, CS on PC3, CE on PB2
        nrf24_dev[0].spi_handle = (FuriHalSpiBusHandle*)&furi_hal_spi_bus_handle_external;
        nrf24_dev[0].initialized = false;
        nrf24_dev[0].ce_pin = &gpio_ext_pb2;
        nrf24_dev[0].cs_pin = &gpio_ext_pc3;
        nrf24_init(&nrf24_dev[0]);
        for(uint8_t i = 1; i < MAX_NRF24; i++) {
            nrf24_dev[i].initialized = false;
        }
    } else {
        for(uint8_t i = 0; i < MAX_NRF24; i++) {
            nrf24_dev[i].spi_handle = (FuriHalSpiBusHandle*)&furi_hal_spi_bus_handle_external;
            nrf24_dev[i].initialized = false;
            if(i == 0) {
                nrf24_dev[i].ce_pin = &gpio_ext_pb2;
                nrf24_dev[i].cs_pin = &gpio_ext_pa4;
            } else if(i == 1) {
                nrf24_dev[i].ce_pin = &gpio_swclk;
                nrf24_dev[i].cs_pin = &gpio_ext_pc3;
            } else if(i == 2) {
                nrf24_dev[i].ce_pin = &gpio_ext_pc1;
                nrf24_dev[i].cs_pin = &gpio_swdio;
            } else if(i == 3) {
                nrf24_dev[i].ce_pin = &gpio_ibutton;
                nrf24_dev[i].cs_pin = &gpio_ext_pc0;
            }
            nrf24_init(&nrf24_dev[i]);
        }
    }

    PluginEvent event;
    bool running = true;
    uint32_t last_tick = furi_get_tick();
    uint32_t last_anim = furi_get_tick();

    while(running) {
        FuriStatus status = furi_message_queue_get(queue, &event, 50);
        uint32_t current_tick = furi_get_tick();

        if(!state->is_modules_connected) {
            uint8_t max_check = (state->spi_mode == SPI_MODE_EXTRA) ? 1 : MAX_NRF24;
            for(uint8_t i = 0; i < max_check; i++) {
                if(nrf24_check_connected(&nrf24_dev[i])) state->len_modules++;
            }
            view_port_update(state->view_port);
            furi_delay_ms(100);
            if(state->len_modules > 0) {
                for(uint8_t i = 0; i < state->len_modules; i++) {
                    if(!nrf24_check_connected(&nrf24_dev[i])) state->len_modules--;
                }
                view_port_update(state->view_port);
                furi_delay_ms(2000);
                state->is_modules_connected = true;
                view_port_update(state->view_port);
            }
        }

        if(state->is_running && (current_tick - last_anim >= 120)) {
            last_anim = current_tick;
            state->anim_frame++;
            view_port_update(state->view_port);
        }

        if(current_tick - last_tick >= HOLD_DELAY_MS) {
            last_tick = current_tick;
            if(state->held_key != InputKeyMAX && state->is_running &&
               state->current_menu == MENU_BLUETOOTH && state->bt_hold) {
                // Auto-repeat tuning while frozen on a BT channel.
                state->hold_counter++;
                if(state->hold_counter >= 3) {
                    if(state->held_key == InputKeyUp || state->held_key == InputKeyRight) {
                        if(state->bt_hold_channel < 80) state->bt_hold_channel++;
                        view_port_update(state->view_port);
                    } else if(state->held_key == InputKeyDown || state->held_key == InputKeyLeft) {
                        if(state->bt_hold_channel > 0) state->bt_hold_channel--;
                        view_port_update(state->view_port);
                    }
                }
            } else if(state->held_key != InputKeyMAX && !state->is_running) {
                state->hold_counter++;
                if(state->hold_counter >= 3) {
                    if(state->current_menu == MENU_MISC && state->misc_state != MISC_STATE_IDLE) {
                        if(state->held_key == InputKeyUp || state->held_key == InputKeyDown) {
                            handle_settings_input(state, state->held_key, true);
                            view_port_update(state->view_port);
                        }
                    } else if(state->current_menu == MENU_WIFI && state->wifi_channel_select) {
                        if(state->held_key == InputKeyUp || state->held_key == InputKeyDown ||
                           state->held_key == InputKeyLeft || state->held_key == InputKeyRight) {
                            handle_wifi_input(state, state->held_key);
                            view_port_update(state->view_port);
                        }
                    } else if(state->current_menu == MENU_SETTINGS && state->settings_menu_active) {
                        if(state->held_key == InputKeyUp || state->held_key == InputKeyDown) {
                            handle_settings_menu_input(state, state->held_key);
                            view_port_update(state->view_port);
                        }
                    }
                }
            }
        }

        if(status == FuriStatusOk && event.type == EVENT_KEY) {
            if(event.input.type == InputTypePress) {
                switch(event.input.key) {
                case InputKeyUp:
                    state->held_key = InputKeyUp;
                    state->hold_counter = 0;
                    if(state->is_running && state->current_menu == MENU_BLUETOOTH &&
                       state->bt_hold) {
                        if(state->bt_hold_channel < 80) state->bt_hold_channel++;
                        view_port_update(state->view_port);
                    } else if(!state->is_running && state->is_modules_connected) {
                        if(state->current_menu == MENU_MISC &&
                           state->misc_state != MISC_STATE_IDLE) {
                            handle_settings_input(state, InputKeyUp, false);
                            view_port_update(state->view_port);
                        } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                            if(state->wifi_channel_select) {
                                handle_wifi_input(state, InputKeyUp);
                            } else {
                                state->wifi_mode = (state->wifi_mode + 1) % WIFI_MODE_COUNT;
                            }
                        } else if(state->current_menu == MENU_SETTINGS && state->settings_menu_active) {
                            handle_settings_menu_input(state, InputKeyUp);
                            view_port_update(state->view_port);
                        } else if(state->current_menu == MENU_BLE && state->ble_menu_active) {
                            state->ble_selected = (state->ble_selected == 0) ? 1 : 0;
                            view_port_update(state->view_port);
                        } else {
                            handle_menu_input(state, InputKeyUp);
                        }
                    }
                    break;

                case InputKeyDown:
                    state->held_key = InputKeyDown;
                    state->hold_counter = 0;
                    if(state->is_running && state->current_menu == MENU_BLUETOOTH &&
                       state->bt_hold) {
                        if(state->bt_hold_channel > 0) state->bt_hold_channel--;
                        view_port_update(state->view_port);
                    } else if(!state->is_running && state->is_modules_connected) {
                        if(state->current_menu == MENU_MISC &&
                           state->misc_state != MISC_STATE_IDLE) {
                            handle_settings_input(state, InputKeyDown, false);
                            view_port_update(state->view_port);
                        } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                            if(state->wifi_channel_select) {
                                handle_wifi_input(state, InputKeyDown);
                            } else {
                                if(state->wifi_mode == 0) {
                                    state->wifi_mode = WIFI_MODE_COUNT - 1;
                                } else {
                                    state->wifi_mode--;
                                }
                            }
                        } else if(state->current_menu == MENU_SETTINGS && state->settings_menu_active) {
                            handle_settings_menu_input(state, InputKeyDown);
                            view_port_update(state->view_port);
                        } else if(state->current_menu == MENU_BLE && state->ble_menu_active) {
                            state->ble_selected = (state->ble_selected == 0) ? 1 : 0;
                            view_port_update(state->view_port);
                        } else {
                            handle_menu_input(state, InputKeyDown);
                        }
                    }
                    break;

                case InputKeyOk:
                    // While BT jamming is running, OK toggles freeze/hop.
                    if(state->is_running && state->current_menu == MENU_BLUETOOTH) {
                        state->bt_hold = !state->bt_hold;
                        view_port_update(state->view_port);
                        break;
                    }
                    uint8_t count = 0;
                    if(!state->is_running) {
                        for(uint8_t i = 0; i < state->len_modules; i++) {
                            if(!nrf24_check_connected(&nrf24_dev[i])) count++;
                        }
                    }
                    if(count == state->len_modules && state->is_modules_connected) {
                        notification_message(state->notifications, &error_sequence);
                        state->len_modules = 0;
                        state->is_modules_connected = false;
                    } else if(!state->is_running && state->is_modules_connected) {
                        if(state->current_menu == MENU_MISC) {
                            if(state->misc_state == MISC_STATE_IDLE) {
                                state->misc_state = MISC_STATE_SET_START;
                                state->misc_start = 0;
                                state->misc_stop = 1;
                            } else if(state->misc_state == MISC_STATE_SET_START) {
                                state->misc_state = MISC_STATE_SET_STOP;
                                if(state->misc_start + 1 <= 125) {
                                    state->misc_stop = state->misc_start + 1;
                                }
                            } else if(state->misc_state == MISC_STATE_SET_STOP) {
                                if(state->misc_stop > state->misc_start) {
                                    state->show_jamming_started = true;
                                    furi_thread_start(state->thread);
                                } else {
                                    state->misc_state = MISC_STATE_ERROR;
                                    notification_message(state->notifications, &error_sequence);
                                }
                            } else if(state->misc_state == MISC_STATE_ERROR) {
                                state->misc_state = MISC_STATE_SET_STOP;
                            }
                        } else if(state->current_menu == MENU_WIFI) {
                            if(state->wifi_menu_active) {
                                if(state->wifi_channel_select) {
                                    furi_thread_start(state->thread);
                                } else {
                                    if(state->wifi_mode == WIFI_MODE_SELECT) {
                                        state->wifi_channel_select = true;
                                    } else {
                                        furi_thread_start(state->thread);
                                    }
                                }
                            } else {
                                state->wifi_menu_active = true;
                            }
                        } else if(state->current_menu == MENU_SETTINGS) {
                            if(state->settings_menu_active) {
                                settings_save(state);
                                notification_message_block(
                                    state->notifications, &sequence_success);
                            } else {
                                state->settings_menu_active = true;
                            }
                        } else if(state->current_menu == MENU_BLE) {
                            if(state->ble_menu_active) {
                                furi_thread_start(state->thread);
                            } else {
                                state->ble_menu_active = true;
                            }
                        } else if(state->current_menu == MENU_INFO) {
                            state->info_active = true;
                        } else {
                            furi_thread_start(state->thread);
                        }
                    }
                    break;

                case InputKeyBack:
                    if(state->is_running) {
                        state->is_stop = true;
                        furi_thread_join(state->thread);
                        if(state->current_menu == MENU_MISC) {
                            state->show_jamming_started = false;
                        }
                        if(state->current_menu == MENU_BLE) {
                            state->ble_menu_active = true;
                        }
                    } else if(state->current_menu == MENU_MISC) {
                        if(state->misc_state == MISC_STATE_SET_STOP) {
                            state->misc_state = MISC_STATE_SET_START;
                        } else if(state->misc_state == MISC_STATE_SET_START) {
                            state->misc_state = MISC_STATE_IDLE;
                        } else {
                            running = false;
                        }
                    } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                        if(state->wifi_channel_select) {
                            state->wifi_channel_select = false;
                        } else {
                            state->wifi_menu_active = false;
                        }
                    } else if(state->current_menu == MENU_SETTINGS) {
                        if(state->settings_menu_active) {
                            settings_save(state);
                            state->settings_menu_active = false;
                        } else {
                            running = false;
                        }
                    } else if(state->current_menu == MENU_BLE) {
                        if(state->ble_menu_active) {
                            state->ble_menu_active = false;
                        } else {
                            running = false;
                        }
                    } else if(state->current_menu == MENU_INFO) {
                        if(state->info_active) {
                            state->info_active = false;
                        } else {
                            running = false;
                        }
                    } else {
                        running = false;
                    }
                    break;

                case InputKeyLeft:
                    state->held_key = InputKeyLeft;
                    state->hold_counter = 0;
                    if(state->is_running && state->current_menu == MENU_BLUETOOTH &&
                       state->bt_hold) {
                        if(state->bt_hold_channel > 0) state->bt_hold_channel--;
                        view_port_update(state->view_port);
                    } else if(!state->is_running && state->is_modules_connected) {
                        if(state->current_menu == MENU_MISC &&
                           state->misc_state != MISC_STATE_IDLE) {
                            if(state->misc_mode == 0) {
                                state->misc_mode = MISC_MODE_COUNT - 1;
                            } else {
                                state->misc_mode--;
                            }
                        } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                            if(state->wifi_channel_select) {
                                handle_wifi_input(state, InputKeyLeft);
                            } else {
                                if(state->wifi_mode == 0) {
                                    state->wifi_mode = WIFI_MODE_COUNT - 1;
                                } else {
                                    state->wifi_mode--;
                                }
                            }
                        } else if(state->current_menu == MENU_SETTINGS && state->settings_menu_active) {
                            handle_settings_menu_input(state, InputKeyLeft);
                            view_port_update(state->view_port);
                        } else {
                            handle_menu_input(state, InputKeyLeft);
                        }
                    }
                    break;

                case InputKeyRight:
                    state->held_key = InputKeyRight;
                    state->hold_counter = 0;
                    if(state->is_running && state->current_menu == MENU_BLUETOOTH &&
                       state->bt_hold) {
                        if(state->bt_hold_channel < 80) state->bt_hold_channel++;
                        view_port_update(state->view_port);
                    } else if(!state->is_running && state->is_modules_connected) {
                        if(state->current_menu == MENU_MISC &&
                           state->misc_state != MISC_STATE_IDLE) {
                            state->misc_mode = (state->misc_mode + 1) % MISC_MODE_COUNT;
                        } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                            if(state->wifi_channel_select) {
                                handle_wifi_input(state, InputKeyRight);
                            } else {
                                state->wifi_mode = (state->wifi_mode + 1) % WIFI_MODE_COUNT;
                            }
                        } else if(state->current_menu == MENU_SETTINGS && state->settings_menu_active) {
                            handle_settings_menu_input(state, InputKeyRight);
                            view_port_update(state->view_port);
                        } else {
                            handle_menu_input(state, InputKeyRight);
                        }
                    }
                    break;
                default:
                    break;
                }
                view_port_update(state->view_port);
            } else if(event.input.type == InputTypeRelease) {
                if(event.input.key == InputKeyUp || event.input.key == InputKeyDown ||
                   event.input.key == InputKeyRight || event.input.key == InputKeyLeft) {
                    state->held_key = InputKeyMAX;
                }
            }
        }
    }

    settings_save(state);

    gui_remove_view_port(gui, state->view_port);
    for(uint8_t i = 0; i < MAX_NRF24; i++) {
        nrf24_deinit(&nrf24_dev[i]);
    }
    view_port_free(state->view_port);
    furi_thread_free(state->thread);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_hal_power_disable_otg();
    furi_mutex_free(state->mutex);
    furi_message_queue_free(queue);
    free(state);

    return 0;
}