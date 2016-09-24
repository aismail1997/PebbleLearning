#include "focusmotion.h"
#include <pebble.h>
#include <pebble_process_info.h>

////////////////////////////////////////
// simple replacement for standard assert function, which causes link errors

#define FM_ASSERT(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            APP_LOG(APP_LOG_LEVEL_ERROR, "ASSERTION FAILED: " #cond); \
            *((int*) 0) = 0; \
        } \
    } while (false)


////////////////////////////////////////
// debug logging

//#define FM_LOG_ENABLED 1

#if FM_LOG_ENABLED
#  define FM_LOG(fmt, args...) APP_LOG(APP_LOG_LEVEL_DEBUG, fmt, ## args)
#else
#  define FM_LOG(fmt, args...)
#endif


////////////////////////////////////////
// version info

static const int k_version_major =
#include "../../../../etc/version/major.txt"
    ;

static const int k_version_minor =
#include "../../../../etc/version/minor.txt"
    ;

static const int k_version_build =
#include "../../../../etc/version/build.txt"
    ;

static const char* k_version_label =
#include "../../../../etc/version/label.txt"
    ;

////////////////////////////////////////

static bool g_inited = false;
static bool g_recording = false;
static bool g_connected = false;
static uint32_t g_connection_id = 1;

// client's event handlers
static AppMessageInboxReceived g_inbox_received_handler = NULL;
static AppMessageOutboxFailed g_outbox_failed_handler = NULL;
static AccelDataHandler g_accel_handler = NULL;
static BluetoothConnectionHandler g_bluetooth_handler = NULL;
static FmRecordingHandler g_recording_handler = NULL;
static FmConnectedHandler g_connected_handler = NULL;

// buffer for accelerometer samples
typedef struct __attribute__ ((__packed__))
{
    int16_t x;
    int16_t y;
    int16_t z;
} Sample;

#define ACCEL_BUF_SIZE 500
static Sample g_accel_buf[ACCEL_BUF_SIZE];
static int g_accel_buf_count = 0; // number of samples in buffer

// buffer for messages to be re-sent
typedef struct
{
    uint8_t* buf;
    int size;
} MsgBuf;

#define RESEND_BUF_SIZE 10
static MsgBuf g_resend_buf[RESEND_BUF_SIZE];
static int g_resend_buf_count = 0;

static int g_samples_sent = 0; // so we can compare with # received on phone
static int g_samples_measured = 0; // since some might not have even been sent if g_accel_buf was full

static int g_msg_flags = 0; // which messages need to be sent?

static char g_metadata_buf[256]; // contains metadata about this app/device that is sent to phone
static int g_metadata_len = 0;

#define DEFAULT_SAMPLING_RATE ACCEL_SAMPLING_50HZ
static AccelSamplingRate g_sampling_rate = DEFAULT_SAMPLING_RATE;
static AccelSamplingRate g_next_sampling_rate = DEFAULT_SAMPLING_RATE;

static AppTimer* g_data_timer = NULL; // sends data to phone at regular intervals

// This interval must be short enough that sensor data will fit in one message (about 656 bytes)
// but not so short that the app messaging system gets overloaded with requests
// 6 bytes per sample at 50 Hz -> 300 bytes per second -> about 2000 ms max interval
#define DATA_TIMER_MS 100

static time_t g_last_message_time = 0;

#define PROTOCOL_VERSION 3
static uint16_t g_app_version = 0;


static void accel_handler(AccelData* inData, uint32_t inCount);
static void set_connected(bool connected);

////////////////////////////////////////
// message keys

// 0x46 0x4D = ascii "FM"

enum
{
    KEYS_BEGIN = 0x464d0000-1,

    // sent from phone to start recording on watch,
    // or sent from watch to notify phone that recording was initiated on watch
    KEY_START,

    // sent from phone to stop recording on watch,
    // and also sent from watch to notify phone that recording has completed and all data has been sent
    KEY_STOP,

    // sensor data sent from watch
    KEY_SENSOR_DATA,

    // metadata sent from watch
    KEY_METADATA,

    // index of sensor data
    KEY_SENSOR_OFFSET,

    // sent from phone to try to connect (with app and protocol version)
    // or sent from watch to confirm connection (with connection id)
    KEY_CONNECT,

    // sent from phone or watch to disconnect
    KEY_DISCONNECT,

    // indicates that the containing message was resent
    KEY_RESEND,

    // sampling rate of sensor data
    KEY_SENSOR_RATE,

    // periodic message to detect disconnection
    KEY_HEARTBEAT,


    KEYS_END // insert new values BEFORE this
};


////////////////////////////////////////
// message flags

static void set_msg_flag(uint32_t key)
{
    g_msg_flags |= (1 << (key - KEY_START));
}

static void clear_msg_flag(uint32_t key)
{
    g_msg_flags &= ~(1 << (key - KEY_START));
}

static bool get_msg_flag(uint32_t key)
{
    return g_msg_flags & (1 << (key - KEY_START));
}

static void clear_resend_buf()
{
    for (int i = 0; i < g_resend_buf_count; ++i)
    {
        free(g_resend_buf[i].buf);
    }
    g_resend_buf_count = 0;
}

////////////////////////////////////////

// all messages are sent from this function, which is triggered at regular intervals by a timer
static void send_data()
{
    if (g_resend_buf_count > 0)
    {
        FM_LOG("resending");
        MsgBuf msgbuf = g_resend_buf[g_resend_buf_count-1];
        DictionaryIterator* out_iter;
        if (app_message_outbox_begin(&out_iter) != APP_MSG_OK)
        {
            FM_LOG("resending return 1");
            return;
        }

        DictionaryIterator in_iter;
        Tuple* t = dict_read_begin_from_buffer(&in_iter, msgbuf.buf, msgbuf.size);
        int resend = -1;
        while (t)
        {
            if (t->key > KEYS_BEGIN && t->key < KEYS_END)
            {
                if (t->key == KEY_RESEND)
                {
                    resend = t->value[0].uint8;
                }
                else
                {
                    switch (t->type)
                    {
                        // TODO what if there's too much data to fit in this message?
                        case TUPLE_BYTE_ARRAY: dict_write_data(out_iter, t->key, t->value[0].data, t->length);            break;
                        case TUPLE_CSTRING:    dict_write_cstring(out_iter, t->key, t->value[0].cstring);                 break;
                        case TUPLE_UINT:       dict_write_int(out_iter, t->key, &t->value[0].uint32, t->length, false);   break;
                        case TUPLE_INT:        dict_write_int(out_iter, t->key, &t->value[0].int32, t->length, true);     break;
                        default: APP_LOG(APP_LOG_LEVEL_DEBUG, "unknown type");
                    }
                }
            }

            t = dict_read_next(&in_iter);
        }

        ++resend;
        dict_write_uint8(out_iter, KEY_RESEND, resend);

        dict_write_end(out_iter);

        // if message fails to be sent, it will be sent in the next call to this function.
        if (app_message_outbox_send() == APP_MSG_OK)
        {
            free(g_resend_buf[g_resend_buf_count-1].buf);
            --g_resend_buf_count;
        }
        else
        {
            FM_LOG("resending return 2");
        }
    }

    if (g_accel_buf_count > 0 || g_msg_flags)
    {
//        FM_LOG("sending %d %d", g_accel_buf_count, g_msg_flags);
        DictionaryIterator* iter;
        AppMessageResult r;
        if ((r=app_message_outbox_begin(&iter)) != APP_MSG_OK)
        {
            FM_LOG("sending return 1 %d", r);
            return;
        }

        if (get_msg_flag(KEY_CONNECT))
        {
            uint32_t version = ((g_app_version << 16) | PROTOCOL_VERSION);
            dict_write_uint32(iter, KEY_CONNECT, g_connected ? (g_connection_id << 16) : version);
            if (g_connected)
            {
                dict_write_data(iter, KEY_METADATA, (const uint8_t*) g_metadata_buf, g_metadata_len);
            }
        }

        if (get_msg_flag(KEY_START))
        {
            dict_write_uint8(iter, KEY_START, 1);
        }

        if (get_msg_flag(KEY_HEARTBEAT))
        {
            dict_write_uint8(iter, KEY_HEARTBEAT, 1);
        }

        int samples_to_send = 0;
        int new_msg_flags = 0;

        if (g_accel_buf_count > 0)
        {
            // index of data
            dict_write_uint32(iter, KEY_SENSOR_OFFSET, g_samples_sent);

            dict_write_uint8(iter, KEY_SENSOR_RATE, g_sampling_rate);

            // sensor data
            int bytes_available = (uint8_t*) iter->end - (uint8_t*) iter->cursor;
            bytes_available -= 32; // leave a little extra space in case we need to add RESEND key; also if we don't leave enough, Pebble crashes!
            int samples_max = bytes_available / sizeof(Sample);
            samples_to_send = g_accel_buf_count;
            if (samples_to_send > samples_max)
            {
                samples_to_send = samples_max;
            }
            dict_write_data(iter, KEY_SENSOR_DATA, (const uint8_t*) g_accel_buf, samples_to_send * sizeof(Sample));
        }

        // don't stop or disconnect until all samples have been sent
        if (samples_to_send == g_accel_buf_count)
        {
            if (get_msg_flag(KEY_STOP))
            {
                int data[] = { g_samples_sent + samples_to_send, g_samples_measured };
                dict_write_data(iter, KEY_STOP, (const uint8_t*) data, sizeof(data));
            }

            if (get_msg_flag(KEY_DISCONNECT))
            {
                dict_write_uint8(iter, KEY_DISCONNECT, g_connection_id);
            }
        }
        else
        {
            new_msg_flags = g_msg_flags & ((1 << (KEY_STOP - KEY_START)) | (1 << (KEY_DISCONNECT - KEY_START)));
        }


        dict_write_end(iter);

        /*
#if FM_LOG_ENABLED
        {
            Tuple* t = dict_read_first(iter);
            while (t)
            {
                if (t->key != KEY_SENSOR_OFFSET && t->key != KEY_SENSOR_RATE && t->key != KEY_SENSOR_DATA)
                {
                    FM_LOG("sending key: %x", (int) t->key);
                    t = dict_read_next(iter);
                }
            }
        }
#endif
*/

        // if message fails to be sent, it will be sent in the next call to this function.
        if (app_message_outbox_send() == APP_MSG_OK)
        {
            g_msg_flags = new_msg_flags;

            if (samples_to_send > 0)
            {
                if (samples_to_send < g_accel_buf_count)
                {
                    memmove(g_accel_buf, g_accel_buf + samples_to_send, (g_accel_buf_count - samples_to_send) * sizeof(Sample));
                }
                g_accel_buf_count -= samples_to_send;
                FM_ASSERT(g_accel_buf_count >= 0);

                g_samples_sent += samples_to_send;
            }
        }
        else
        {
            FM_LOG("sending return 2");
        }
    }
}



////////////////////////////////////////
// data timer

static void data_timer_callback(void*);

static void register_data_timer()
{
    if (!g_data_timer)
    {
        g_data_timer = app_timer_register(DATA_TIMER_MS, data_timer_callback, NULL);
    }
}

static void cancel_data_timer()
{
    if (g_data_timer)
    {
        app_timer_cancel(g_data_timer);
    }
    g_data_timer = NULL;
}

static void data_timer_callback(void* data)
{
    g_data_timer = NULL;
    send_data();

    if (g_last_message_time > 0)
    {
        time_t d = time(NULL) - g_last_message_time;
        if (g_connected && d > 8)
        {
            FM_LOG("timeout!");
            set_connected(false);
        }
    }

    register_data_timer();
}


////////////////////////////////////////
// start/stop recording

static void start_recording()
{
    if (!g_recording)
    {
        FM_LOG("starting recording");
        g_sampling_rate = g_next_sampling_rate;
        accel_data_service_subscribe(10, accel_handler);
        accel_service_set_sampling_rate(g_sampling_rate); // must be after subscribe
        app_comm_set_sniff_interval(SNIFF_INTERVAL_REDUCED);

        g_accel_buf_count = 0;
        g_samples_sent = 0;
        g_samples_measured = 0;

        set_msg_flag(KEY_START);
        clear_msg_flag(KEY_STOP);
        send_data();

        g_recording = true;
        if (g_recording_handler)
        {
            g_recording_handler(true);
        }
    }
}

static void stop_recording()
{
    if (g_recording)
    {
        FM_LOG("stopping recording");
        set_msg_flag(KEY_STOP);
        clear_msg_flag(KEY_START);
        send_data();

        accel_data_service_unsubscribe();
        app_comm_set_sniff_interval(SNIFF_INTERVAL_NORMAL);

        g_recording = false;
        if (g_recording_handler)
        {
            g_recording_handler(false);
        }
    }
}

////////////////////////////////////////


// handle change in connected/disconnected state.
// (This means the state of the connection to the SDK on the phone, not just the bluetooth connection to the phone.)
static void set_connected(bool connected)
{
    if (g_connected != connected)
    {
        FM_LOG("set connected: %d", connected);
        if (connected)
        {
            ++g_connection_id;
            if (g_connection_id > 1000)
            {
                g_connection_id = 1;
            }
        }
        else
        {
            stop_recording();
            clear_resend_buf();
            g_accel_buf_count = 0;
            g_msg_flags = 0;
            g_last_message_time = 0;

            set_msg_flag(KEY_DISCONNECT);
        }

        g_connected = connected;

        // call handler
        if (g_connected_handler)
        {
            g_connected_handler(connected);
        }
    }
}


// message was sent but was not delivered.
static void outbox_failed_handler(DictionaryIterator* in_iter, AppMessageResult reason, void* context)
{
    if (reason == APP_MSG_SEND_REJECTED)
    {
        // on Android, we've been getting this, which is supposed to indicate that the message is being NACK'd on the
        // phone, when it isn't... ignoring for now.
        return;
    }

    FM_LOG("message failed");

    if (g_connected)
    {
        FM_LOG("message failed 0");
        Tuple* t = dict_find(in_iter, KEY_RESEND);
        if (t && t->value[0].uint8 > 5)
        {
            FM_LOG(" bailing");
            // we've already tried to re-send this message too many times; give up.
            set_connected(false);
        }
        else
        {
            if (g_resend_buf_count < RESEND_BUF_SIZE)
            {
                FM_LOG(" retrying %d", t ? (int) t->value[0].uint8:0);
                // copy the message and save it to be re-sent
                uint32_t size = dict_size(in_iter);
                uint8_t* buf = malloc(size);
                memcpy(buf, in_iter->dictionary, size);
                g_resend_buf[g_resend_buf_count] = (MsgBuf) { .buf = buf, .size = size };
                ++g_resend_buf_count;
            }
            else
            {
                FM_LOG(" disconnecting");
                set_connected(false);
            }
        }
    }

    if (g_outbox_failed_handler)
    {
        g_outbox_failed_handler(in_iter, reason, context);
    }
    FM_LOG("message failed end");
}

// handle incoming messages
static void inbox_received_handler(DictionaryIterator* iter, void* context)
{
    g_last_message_time = time(NULL);

    bool handled = false;

    Tuple* t = dict_read_first(iter);

    while (t)
    {
//       FM_LOG("got key: %x", (int) t->key);
        switch (t->key)
        {
            case KEY_START:
                {
                    start_recording();
                    handled = true;
                }
                break;

            case KEY_STOP:
                stop_recording();
                handled = true;
                break;

            case KEY_DISCONNECT:
                set_connected(false);
                handled = true;
                break;

            case KEY_CONNECT:
                {
                    uint32_t version = t->value[0].uint32;
                    if (version > 0)
                    {
                        uint16_t protocol_version = (version & 0xffff);
                        uint16_t app_version = ((version >> 16) & 0xffff);
                        FM_LOG(" protocol version: %d %d", protocol_version, PROTOCOL_VERSION);
                        FM_LOG(" app version: %d %d", app_version, g_app_version);
                        if (protocol_version != PROTOCOL_VERSION || app_version != g_app_version)
                        {
                            // protocol version mismatch
                            set_connected(false);
                            set_msg_flag(KEY_CONNECT); // will send watch version, so phone can show sensible error message
                        }
                        else
                        {
                            set_connected(true);
                            set_msg_flag(KEY_CONNECT); // send acknowledgement
                        }
                    }
                    else
                    {
                        set_connected(true);
                    }
                }
                handled = true;
                break;

            default:
                break;
        }

        t = dict_read_next(iter);
    }

    // client's handler
    if (!handled && g_inbox_received_handler)
    {
        g_inbox_received_handler(iter, context);
    }
}

static void accel_handler(AccelData* inData, uint32_t inCount)
{
    if (g_recording)
    {
        g_samples_measured += inCount;

        // store the accelerometer samples
        int n = inCount;
        int nBuf = ACCEL_BUF_SIZE - g_accel_buf_count;
        if (n > nBuf)
        {
            FM_LOG("buffer full!  dropping %d", n-nBuf);
            n = nBuf; // dropping samples!
        }

        if (n > 0)
        {
            Sample* pIn = &g_accel_buf[g_accel_buf_count];
            AccelData* pOut = inData;
            for (int i = 0; i < n; ++i)
            {
#if 1
                pIn->x = pOut->x;
                pIn->y = pOut->y;
                pIn->z = pOut->z;
//#ifdef PBL_PLATFORM_DIORITE
//                // workaround for bug in early release firmware of Pebble 2
//                pIn-> = -pIn->z;
//#endif
#else
                static int fake = 0;
                pIn->x = fake;
                pIn->y = fake;
                pIn->z = fake;
                ++fake;
#endif
                ++pIn;
                ++pOut;
            }
            g_accel_buf_count += n;
        }
    }

    if (g_accel_handler)
    {
        // call client's handler
        g_accel_handler(inData, inCount);
    }
}

static void bluetooth_handler(bool connected)
{
    if (!connected)
    {
        set_connected(false);
    }

    // client's handler
    if (g_bluetooth_handler)
    {
        g_bluetooth_handler(connected);
    }
}


////////////////////////////////////////

void init_metadata()
{
    const char* hardwareName = NULL;
    WatchInfoModel model = watch_info_get_model();
    switch (model)
    {
        case WATCH_INFO_MODEL_PEBBLE_ORIGINAL:      hardwareName = "Pebble"; break;
        case WATCH_INFO_MODEL_PEBBLE_STEEL:         hardwareName = "Pebble Steel"; break;
        case WATCH_INFO_MODEL_PEBBLE_TIME:          hardwareName = "Pebble Time"; break;
        case WATCH_INFO_MODEL_PEBBLE_TIME_STEEL:    hardwareName = "Pebble Time Steel"; break;
        case WATCH_INFO_MODEL_PEBBLE_TIME_ROUND_14: hardwareName = "Pebble Time Round 14mm"; break;
        case WATCH_INFO_MODEL_PEBBLE_TIME_ROUND_20: hardwareName = "Pebble Time Round 20mm"; break;
        default:
              {
                  static char buf[24];
                  snprintf(buf, sizeof(buf), "Pebble (unknown:%d)", (int)model);
                  hardwareName = buf;
              }
    }

    extern const PebbleProcessInfo __pbl_app_info;
    snprintf(g_metadata_buf, sizeof(g_metadata_buf),
            "{"
            "\"deviceHardwareName\":\"%s\","
            "\"deviceAppId\":\"%s (%s)\","
            "\"deviceAppVersion\":\"%d.%d\","
            "\"deviceSdkVersion\":\"%d.%d.%d%s%s\""
            "}",
            hardwareName,
            __pbl_app_info.name,
            __pbl_app_info.company,
            __pbl_app_info.process_version.major,
            __pbl_app_info.process_version.minor,
            k_version_major, k_version_minor, k_version_build, (strlen(k_version_label) ? " " : ""), k_version_label);
    g_metadata_len = strlen(g_metadata_buf);
}

////////////////////////////////////////

void focusmotion_startup(uint16_t app_version,
                      AppMessageInboxReceived client_inbox_received_handler,
                      AppMessageOutboxFailed client_outbox_failed_handler,
                      AccelDataHandler client_accel_handler,
                      BluetoothConnectionHandler client_bluetooth_handler,
                      FmConnectedHandler client_connected_handler,
                      FmRecordingHandler client_recording_handler)
{
    if (!g_inited)
    {
        g_app_version = app_version;
        g_inbox_received_handler = client_inbox_received_handler;
        g_outbox_failed_handler = client_outbox_failed_handler;
        g_accel_handler = client_accel_handler;
        g_bluetooth_handler = client_bluetooth_handler;
        g_recording_handler = client_recording_handler;
        g_connected_handler = client_connected_handler;

        // message service
        app_message_register_inbox_received(inbox_received_handler);
        app_message_register_outbox_failed(outbox_failed_handler);
//        app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum()); // TODO don't use maximum size to save memory?
        app_message_open(3000, 3000);
        register_data_timer();

        // bluetooth service
        bluetooth_connection_service_subscribe(bluetooth_handler);

        // init metadata string
        init_metadata();

        g_accel_buf_count = 0;
        g_samples_sent = 0;
        g_samples_measured = 0;
        g_msg_flags = 0;
        g_connected = false;

        g_recording = false;
        g_inited = true;
    }
}

void focusmotion_start_recording()
{
    if (bluetooth_connection_service_peek())
    {
        start_recording();
    }
}

void focusmotion_stop_recording()
{
    stop_recording();
}

bool focusmotion_is_recording()
{
    return g_recording;
}

bool focusmotion_is_connected()
{
    return g_connected;
}

AccelSamplingRate focusmotion_get_sampling_rate()
{
    return g_sampling_rate;
}

void focusmotion_set_sampling_rate(AccelSamplingRate rate)
{
    g_next_sampling_rate = rate;

    // if recording, g_sampling_rate will be set from g_next_sampling_rate
    // next time we start.

    if (!g_recording)
    {
        g_sampling_rate = rate;
    }
}

void focusmotion_shutdown()
{
    if (g_inited)
    {
        g_inbox_received_handler = NULL;
        g_accel_handler = NULL;
        g_bluetooth_handler = NULL;
        g_recording_handler = NULL;
        g_connected_handler = NULL;

        set_msg_flag(KEY_DISCONNECT);
        stop_recording();
        send_data(); // send queued data, e.g. to stop recording

        // try sending any last messages
        int i = 0;
        while (g_msg_flags && i < 5)
        {
            FM_LOG("sleeping %d %d", i, g_msg_flags);
            psleep(50);
            send_data();
            ++i;
        }

        app_message_register_inbox_received(NULL);
        bluetooth_connection_service_unsubscribe();
        cancel_data_timer();
        clear_resend_buf();
    }
    g_inited = false;
}
