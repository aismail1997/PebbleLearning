#pragma once

/** @file */

#include <pebble.h>

/** Handler to notify your Pebble app when recording starts or stops (whether initiated from the watch or the phone) */
typedef void (*FmRecordingHandler)(bool is_recording);

/** Handler to notify your Pebble app when connected to or disconnected from the FocusMotion SDK on the phone */
typedef void (*FmConnectedHandler)(bool is_connected);


/** Call this when your app is initialized; this is typically done from your app's init() function.

 The app_version should be the same value provided when you initialize your phone app.
 If the values do not match, the connection will fail; this is a useful way to prevent
 your phone app from connecting to outdated Pebble apps.

 The Pebble SDK does not allow multiple handlers to be registered for more services, so
 if your app uses the AppMessage, Accelerometer, or Bluetooth Connection services, rather
 than registering handlers for them directly, pass your handlers in here; this library
 registers its own handlers, and then will call yours.  You can pass in NULL for handlers
 for services your app does not use.

 If you would like your app to be notified when recording starts or stops (whether initiated
 from the watch or the phone), pass in an FmRecordingHandler.

 If you would like your app to be notified when a connection is established with the FocusMotion SDK
 on the phone, pass in an FmConnectedHandler. */

void focusmotion_startup(uint16_t app_version,
                         AppMessageInboxReceived inbox_received_handler,
                         AppMessageOutboxFailed outbox_failed_handler,
                         AccelDataHandler accel_handler,
                         BluetoothConnectionHandler bluetooth_handler,
                         FmConnectedHandler connected_handler,
                         FmRecordingHandler recording_handler);


/** Start recording sensor data. */
void focusmotion_start_recording();

/** Stop recording sensor data. */
void focusmotion_stop_recording();

/** Returns true if sensor data is being recorded. */
bool focusmotion_is_recording();

/** Returns true if the watch is connected to the FocusMotion SDK on the phone. */
bool focusmotion_is_connected();

/** Get the sampling rate of the accelerometer. */
AccelSamplingRate focusmotion_get_sampling_rate();

/** Set the sampling rate of the accelerometer.
 The default sampling rate of the accelerometer is 50 Hz, which is recommended for most types of motion. */
void focusmotion_set_sampling_rate(AccelSamplingRate);

/** Call this when your app shuts down; this is typically done from your app's deinit() function. */
void focusmotion_shutdown();
