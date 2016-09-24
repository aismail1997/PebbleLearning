#include <pebble.h>
#include "focusmotion.h"
#include "version.h"

static Window* g_window = NULL;
static TextLayer* g_title_layer = NULL;
static TextLayer* g_status_layer = NULL;

static GBitmap* g_record_bitmap = NULL;
static GBitmap* g_stop_bitmap = NULL;
static ActionBarLayer* g_action_bar = NULL;

////////////////////////////////////////

static void click_handler(ClickRecognizerRef recognizer, void* context)
{
    if (focusmotion_is_connected())
    {
        // toggle recording with middle (select) button
        if (focusmotion_is_recording())
        {
            focusmotion_stop_recording();
        }
        else
        {
            focusmotion_start_recording();
        }
    }
}

static void click_config_provider(void* context)
{
    window_single_click_subscribe(BUTTON_ID_SELECT, click_handler);
}

static void update_ui()
{
    if (focusmotion_is_connected())
    {
        if (focusmotion_is_recording())
        {
            action_bar_layer_set_icon(g_action_bar, BUTTON_ID_SELECT, g_stop_bitmap);
            text_layer_set_text(g_status_layer, "recording");
        }
        else
        {
            action_bar_layer_set_icon(g_action_bar, BUTTON_ID_SELECT, g_record_bitmap);
            text_layer_set_text(g_status_layer, "ready");
        }
    }
    else
    {
        action_bar_layer_set_icon(g_action_bar, BUTTON_ID_SELECT, NULL);
        text_layer_set_text(g_status_layer, "disconnected");
    }
}

static void connected_handler(bool connected)
{
    update_ui();
}

static void recording_handler(bool recording)
{
    // vibrate when starting/stopping
    static const uint32_t pulse = 100;
    VibePattern pat;
    pat.durations = &pulse;
    pat.num_segments = 1;
    vibes_enqueue_custom_pattern(pat);

    update_ui();
}

////////////////////////////////////////

static void init()
{
    g_window = window_create();
    window_set_background_color(g_window, GColorBlack);
    window_stack_push(g_window, true);

    Layer* window_layer = window_get_root_layer(g_window);
    GRect bounds = layer_get_frame(window_layer);

    // title
    g_title_layer = text_layer_create((GRect) { .origin = { 13, 35 }, .size = { bounds.size.w, 60 } });
    text_layer_set_text_color(g_title_layer, GColorWhite);
    text_layer_set_background_color(g_title_layer, GColorBlack);
    text_layer_set_font(g_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(g_title_layer, GTextAlignmentLeft);
    text_layer_set_text(g_title_layer, "FocusMotion\nSimple Demo");
    layer_add_child(window_layer, text_layer_get_layer(g_title_layer));

    // layer for displaying status
    g_status_layer = text_layer_create((GRect) { .origin = { 13, 90 }, .size = { bounds.size.w, 30 } });
    text_layer_set_text_color(g_status_layer, GColorWhite);
    text_layer_set_background_color(g_status_layer, GColorBlack);
    text_layer_set_font(g_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    text_layer_set_text_alignment(g_status_layer, GTextAlignmentLeft);
    layer_add_child(window_layer, text_layer_get_layer(g_status_layer));

    // action bar
    g_record_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_RECORD);
    g_stop_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_STOP);
    g_action_bar = action_bar_layer_create();
    action_bar_layer_set_icon(g_action_bar, BUTTON_ID_SELECT, g_record_bitmap);
    action_bar_layer_set_background_color(g_action_bar, GColorWhite);
    action_bar_layer_set_click_config_provider(g_action_bar, click_config_provider);
    action_bar_layer_add_to_window(g_action_bar, g_window);

    // initialize focusmotion library
    focusmotion_startup(PEBBLE_APP_VERSION, NULL, NULL, NULL, NULL, connected_handler, recording_handler);

    update_ui();
}


static void deinit()
{
    focusmotion_shutdown();

    action_bar_layer_destroy(g_action_bar);
    gbitmap_destroy(g_record_bitmap);
    gbitmap_destroy(g_stop_bitmap);
    text_layer_destroy(g_status_layer);
    text_layer_destroy(g_title_layer);
    window_destroy(g_window);
}


int main()
{
    init();
    app_event_loop();
    deinit();
}
