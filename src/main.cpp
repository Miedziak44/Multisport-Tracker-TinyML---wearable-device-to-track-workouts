#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <stdio.h>

/**
 * @brief Zewnętrzne biblioteki inferencji modelu Edge Impulse.
 */
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

/**
 * @brief Zależności stosu komunikacyjnego Bluetooth Low Energy (BLE) oraz usługi NUS.
 */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/services/nus.h>

/**
 * @brief Konfiguracja parametrów algorytmu histerezy maszyny stanów.
 */
#define START_THRESHOLD 0.90f /**< Próg aktywacji stanu ćwiczenia (minimalna pewność modelu: 90%). */
#define END_THRESHOLD 0.40f   /**< Próg deaktywacji stanu ćwiczenia (spadek pewności poniżej 40%). */
#define STRIDE_SAMPLES 10     /**< Przesunięcie okna inferencji (10 próbek * 20ms = częstotliwość predykcji co 200ms). */

static struct bt_conn *current_conn;

/**
 * @brief Globalne flagi zdarzeń komunikacyjnych wyzwalanych przez aplikację mobilną.
 */
volatile bool request_reset = false;
volatile bool request_summary = false;

/**
 * @brief Wywołanie zwrotne (callback) uruchamiane w momencie nawiązania połączenia BLE.
 * * @param conn Wskaźnik na strukturę nawiązanego połączenia.
 * @param err  Kod błędu (0 w przypadku sukcesu).
 */
static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) { printk("Blad polaczenia BLE\n"); return; }
    printk("BLE: Polaczono z telefonem!\n");
    current_conn = bt_conn_ref(conn);
}

/**
 * @brief Wywołanie zwrotne (callback) uruchamiane w momencie zerwania połączenia BLE.
 * * @param conn   Wskaźnik na strukturę połączenia.
 * @param reason Kod przyczyny rozłączenia.
 */
static void disconnected(struct bt_conn *conn, uint8_t reason) {
    printk("BLE: Rozlaczono\n");
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/**
 * @brief Przerwanie obsługujące dane RX z usługi Nordic UART Service (NUS).
 * * @param conn Wskaźnik na strukturę połączenia.
 * @param data Bufor zawierający odebrane dane.
 * @param len  Długość odebranych danych w bajtach.
 */
static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data, uint16_t len) {
    if (len > 0) {
        if (data[0] == '1') request_reset = true;
        if (data[0] == '2') request_summary = true;
    }
}

static struct bt_nus_cb nus_cb = { .received = bt_receive_cb };

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/**
 * @brief Pętla główna aplikacji: inicjalizacja urządzeń, BLE oraz cykliczny proces inferencji AI.
 * * @return Zwraca 0 po poprawnym zakończeniu (systemy RTOS z reguły nie opuszczają tej funkcji).
 */
int main(void)
{
    const struct device *const dev = DEVICE_DT_GET(DT_ALIAS(imu0));
    k_msleep(2000); 
    printk("\n--- XIAO ML AI Inference Start ---\n");

    if (!device_is_ready(dev)) {
        printk("BŁĄD: Sensor nie gotowy!\n");
        return 0;
    }

    struct sensor_value odr_attr;
    odr_attr.val1 = 104; 
    odr_attr.val2 = 0;
    sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);

    bt_enable(NULL);
    bt_nus_init(&nus_cb);
    bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), NULL, 0);

    struct k_timer ml_timer;
    k_timer_init(&ml_timer, NULL, NULL);

    /**
     * @brief Bufor na surowe dane wejściowe dla bloku cyfrowego przetwarzania sygnału (DSP).
     */
    float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE]; 
    int feature_ix = 0;
    
    /**
     * @brief Rejestry stanu maszyny decyzyjnej i liczniki aktywności dyskretnych.
     */
    bool is_exercising = false;
    int active_exercise_idx = -1;
    int rep_counts[EI_CLASSIFIER_LABEL_COUNT] = {0};
    char ble_msg[64];

    /**
     * @brief Rejestry czasowe dla blokady szumów i zliczania aktywności o charakterze ciągłym.
     */
    int cooldown_timer = 0;
    uint32_t run_start_time = 0;
    uint32_t total_run_time_s = 0;

    while (1) {
        k_timer_start(&ml_timer, K_MSEC(20), K_NO_WAIT);
        
        /* --- OBSŁUGA KOMEND Z INTERFEJSU MOBILNEGO --- */
        if (request_reset) {
            for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                rep_counts[i] = 0;
            }
            total_run_time_s = 0;
            is_exercising = false;
            cooldown_timer = 0;
            request_reset = false;
            
            if (current_conn) {
                snprintf(ble_msg, sizeof(ble_msg), "\n[ ZRESETOWANO LICZNIKI ]\n");
                bt_nus_send(current_conn, (uint8_t *)ble_msg, strlen(ble_msg));
            }
        }

        if (request_summary) {
            request_summary = false;
            if (current_conn) {
                snprintf(ble_msg, sizeof(ble_msg), "\n################\n");
                bt_nus_send(current_conn, (uint8_t *)ble_msg, strlen(ble_msg));
                k_msleep(50); /* Oczekiwanie na zwolnienie bufora MTU w stosie sieciowym BLE */
                
                for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                    const char* label = ei_classifier_inferencing_categories[ix];
                    if (strcmp(label, "idle") != 0 && strcmp(label, "run") != 0) {
                        snprintf(ble_msg, sizeof(ble_msg), "%s: %d\n", label, rep_counts[ix]);
                        bt_nus_send(current_conn, (uint8_t *)ble_msg, strlen(ble_msg));
                        k_msleep(50);
                    }
                }
                
                /* Czas aktywności przesyłany jako osobna wartość ciągła */
                snprintf(ble_msg, sizeof(ble_msg), "bieg: %u s\n################\n", total_run_time_s);
                bt_nus_send(current_conn, (uint8_t *)ble_msg, strlen(ble_msg));
            }
        }

        struct sensor_value accel[3];
        
        if (sensor_sample_fetch(dev) == 0) {
            sensor_channel_get(dev, SENSOR_CHAN_ACCEL_X, &accel[0]);
            sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Y, &accel[1]);
            sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Z, &accel[2]);

            features[feature_ix++] = sensor_value_to_double(&accel[0]);
            features[feature_ix++] = sensor_value_to_double(&accel[1]);
            features[feature_ix++] = sensor_value_to_double(&accel[2]);

            if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
                
                signal_t signal;
                numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
                
                ei_impulse_result_t result = { 0 };
                EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

                if (err == EI_IMPULSE_OK) {
                    float max_prob = 0.0f;
                    int max_idx = -1;
                    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                        if (result.classification[ix].value > max_prob) {
                            max_prob = result.classification[ix].value;
                            max_idx = ix;
                        }
                    }
                    const char* best_label = result.classification[max_idx].label;

                    /* Dekrementacja licznika blokady cyfrowej przy każdym wywołaniu predykcji */
                    if (cooldown_timer > 0) {
                        cooldown_timer--;
                    }

                    /* --- LOGIKA HISTEREZY --- */
                    if (!is_exercising) {
                        /* Inicjalizacja stanu ruchu po przekroczeniu marginesu błędu i zwolnieniu blokady czasowej */
                        if (max_prob > START_THRESHOLD && strcmp(best_label, "idle") != 0 && cooldown_timer == 0) {
                            is_exercising = true;
                            active_exercise_idx = max_idx;
                            
                            /* Oznaczenie punktu startowego dla estymacji czasu rzeczywistego (aktywności ciągłe) */
                            if (strcmp(best_label, "run") == 0) {
                                run_start_time = k_uptime_get_32();
                            }
                            printk("START RUCHU: %s (Pewnosc: %.2f)\n", best_label, max_prob);
                        }
                    } else {
                        /* Deaktywacja stanu ruchu w wyniku gwałtownego spadku pewności (zakończenie cyklu) */
                        if (result.classification[active_exercise_idx].value < END_THRESHOLD) {
                            is_exercising = false;
                            cooldown_timer = 3; /* Blokada typu Debouncing: 3 okna * 200 ms = 600 ms zamrożenia logiki */
                            
                            const char* active_label = result.classification[active_exercise_idx].label;
                            
                            if (strcmp(active_label, "run") == 0) {
                                /* Akumulacja całkowitego czasu trwania dla aktywności ciągłych */
                                uint32_t duration_s = (k_uptime_get_32() - run_start_time) / 1000;
                                if (duration_s > 0) { /* Filtrowanie szumów krótkotrwałych poniżej 1 sekundy */
                                    total_run_time_s += duration_s;
                                    printk(">>> BIEG PRZERWANY | Czas calkowity: %u s <<<\n", total_run_time_s);
                                }
                            } else {
                                /* Inkrementacja wektora powtórzeń dla ćwiczeń dyskretnych */
                                rep_counts[active_exercise_idx]++;
                                printk(">>> ZALICZONO: %s | Lacznie: %d <<<\n", active_label, rep_counts[active_exercise_idx]);
                                
                                if (current_conn) {
                                    snprintf(ble_msg, sizeof(ble_msg), "%s: %d\n", active_label, rep_counts[active_exercise_idx]);
                                    bt_nus_send(current_conn, (uint8_t *)ble_msg, strlen(ble_msg));
                                }
                            }
                        }
                    }
                }

                /* Aktualizacja bufora - implementacja algorytmu okna przesuwnego (Sliding Window) */
                int shift_floats = STRIDE_SAMPLES * 3;
                memmove(features, features + shift_floats, (EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - shift_floats) * sizeof(float));
                feature_ix -= shift_floats;
            }
        }
        k_timer_status_sync(&ml_timer);
    }
    return 0;
}