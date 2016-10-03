/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Modified from hardware/libhardware/modules/camera/Camera.cpp

#include <cstdlib>
#include <memory>
#include <vector>
#include <stdio.h>
#include <hardware/camera3.h>
#include <sync/sync.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>
#include <utils/Mutex.h>

#include "metadata/metadata_common.h"
#include "stream.h"

//#define LOG_NDEBUG 0
#define LOG_TAG "Camera"
#include <cutils/log.h>

#define ATRACE_TAG (ATRACE_TAG_CAMERA | ATRACE_TAG_HAL)
#include <utils/Trace.h>

#include "camera.h"

#define CAMERA_SYNC_TIMEOUT 5000 // in msecs

namespace default_camera_hal {

extern "C" {
// Shim passed to the framework to close an opened device.
static int close_device(hw_device_t* dev)
{
    camera3_device_t* cam_dev = reinterpret_cast<camera3_device_t*>(dev);
    Camera* cam = static_cast<Camera*>(cam_dev->priv);
    return cam->close();
}
} // extern "C"

Camera::Camera(int id)
  : mId(id),
    mSettingsSet(false),
    mBusy(false),
    mCallbackOps(NULL),
    mStreams(NULL),
    mNumStreams(0)
{
    memset(&mTemplates, 0, sizeof(mTemplates));
    memset(&mDevice, 0, sizeof(mDevice));
    mDevice.common.tag    = HARDWARE_DEVICE_TAG;
    mDevice.common.version = CAMERA_DEVICE_API_VERSION_3_4;
    mDevice.common.close  = close_device;
    mDevice.ops           = const_cast<camera3_device_ops_t*>(&sOps);
    mDevice.priv          = this;
}

Camera::~Camera()
{
}

int Camera::openDevice(const hw_module_t *module, hw_device_t **device)
{
    ALOGI("%s:%d: Opening camera device", __func__, mId);
    ATRACE_CALL();
    android::Mutex::Autolock al(mDeviceLock);

    if (mBusy) {
        ALOGE("%s:%d: Error! Camera device already opened", __func__, mId);
        return -EBUSY;
    }

    int connectResult = connect();
    if (connectResult != 0) {
      return connectResult;
    }
    mBusy = true;
    mDevice.common.module = const_cast<hw_module_t*>(module);
    *device = &mDevice.common;
    return 0;
}

int Camera::getInfo(struct camera_info *info)
{
    android::Mutex::Autolock al(mStaticInfoLock);

    info->device_version = mDevice.common.version;
    initDeviceInfo(info);
    if (mStaticInfo == NULL) {
        std::unique_ptr<android::CameraMetadata> static_info =
            std::make_unique<android::CameraMetadata>();
        if (initStaticInfo(static_info.get())) {
            return -ENODEV;
        }
        mStaticInfo = std::move(static_info);
    }
    // The "locking" here only causes non-const methods to fail,
    // which is not a problem since the CameraMetadata being locked
    // is already const. Destructing automatically "unlocks".
    info->static_camera_characteristics = mStaticInfo->getAndLock();

    // Get facing & orientation from the static info.
    uint8_t facing = 0;
    int res = v4l2_camera_hal::SingleTagValue(
        *mStaticInfo, ANDROID_LENS_FACING, &facing);
    if (res) {
        ALOGE("%s:%d: Failed to get facing from static metadata.",
              __func__, mId);
        return res;
    }
    switch (facing) {
        case (ANDROID_LENS_FACING_FRONT):
            info->facing = CAMERA_FACING_FRONT;
            break;
        case (ANDROID_LENS_FACING_BACK):
            info->facing = CAMERA_FACING_BACK;
            break;
        case (ANDROID_LENS_FACING_EXTERNAL):
            info->facing = CAMERA_FACING_EXTERNAL;
            break;
        default:
            ALOGE("%s:%d: Invalid facing from metadata: %d.",
                  __func__, mId, facing);
            return -ENODEV;
    }
    int32_t orientation = 0;
    res = v4l2_camera_hal::SingleTagValue(
        *mStaticInfo, ANDROID_SENSOR_ORIENTATION, &orientation);
    if (res) {
        ALOGE("%s:%d: Failed to get orientation from static metadata.",
              __func__, mId);
        return res;
    }
    info->orientation = static_cast<int>(orientation);

    return 0;
}

int Camera::close()
{
    ALOGI("%s:%d: Closing camera device", __func__, mId);
    ATRACE_CALL();
    android::Mutex::Autolock al(mDeviceLock);

    if (!mBusy) {
        ALOGE("%s:%d: Error! Camera device not open", __func__, mId);
        return -EINVAL;
    }

    disconnect();
    mBusy = false;
    return 0;
}

int Camera::initialize(const camera3_callback_ops_t *callback_ops)
{
    int res;

    ALOGV("%s:%d: callback_ops=%p", __func__, mId, callback_ops);
    mCallbackOps = callback_ops;
    // per-device specific initialization
    res = initDevice();
    if (res != 0) {
        ALOGE("%s:%d: Failed to initialize device!", __func__, mId);
        return res;
    }
    return 0;
}

int Camera::configureStreams(camera3_stream_configuration_t *stream_config)
{
    camera3_stream_t *astream;
    Stream **newStreams = NULL;
    int res = 0;

    // Must provide new settings after configureStreams.
    mSettingsSet = false;

    ALOGV("%s:%d: stream_config=%p", __func__, mId, stream_config);
    ATRACE_CALL();
    android::Mutex::Autolock al(mDeviceLock);

    if (stream_config == NULL) {
        ALOGE("%s:%d: NULL stream configuration array", __func__, mId);
        return -EINVAL;
    }
    if (stream_config->num_streams == 0) {
        ALOGE("%s:%d: Empty stream configuration array", __func__, mId);
        return -EINVAL;
    }

    // Create new stream array
    newStreams = new Stream*[stream_config->num_streams];
    ALOGV("%s:%d: Number of Streams: %d", __func__, mId,
            stream_config->num_streams);

    // Mark all current streams unused for now
    for (int i = 0; i < mNumStreams; i++)
        mStreams[i]->mReuse = false;
    // Fill new stream array with reused streams and new streams
    for (unsigned int i = 0; i < stream_config->num_streams; i++) {
        astream = stream_config->streams[i];
        if (astream->max_buffers > 0) {
            ALOGV("%s:%d: Reusing stream %d", __func__, mId, i);
            newStreams[i] = reuseStream(astream);
        } else {
            ALOGV("%s:%d: Creating new stream %d", __func__, mId, i);
            newStreams[i] = new Stream(mId, astream);
        }

        if (newStreams[i] == NULL) {
            ALOGE("%s:%d: Error processing stream %d", __func__, mId, i);
            goto err_out;
        }
        astream->priv = newStreams[i];
    }

    // Verify the set of streams in aggregate
    if (!isValidStreamSet(newStreams, stream_config->num_streams,
                          stream_config->operation_mode)) {
        ALOGE("%s:%d: Invalid stream set", __func__, mId);
        goto err_out;
    }

    // Set up all streams (calculate usage/max_buffers for each,
    // do any device-specific initialization)
    res = setupStreams(newStreams, stream_config->num_streams);
    if (res) {
        ALOGE("%s:%d: Failed to setup stream set", __func__, mId);
        goto err_out;
    }

    // Destroy all old streams and replace stream array with new one
    destroyStreams(mStreams, mNumStreams);
    mStreams = newStreams;
    mNumStreams = stream_config->num_streams;

    return 0;

err_out:
    // Clean up temporary streams, preserve existing mStreams/mNumStreams
    destroyStreams(newStreams, stream_config->num_streams);
    // Set error if it wasn't specified.
    if (!res) {
      res = -EINVAL;
    }
    return res;
}

void Camera::destroyStreams(Stream **streams, int count)
{
    if (streams == NULL)
        return;
    for (int i = 0; i < count; i++) {
        // Only destroy streams that weren't reused
        if (streams[i] != NULL && !streams[i]->mReuse)
            delete streams[i];
    }
    delete [] streams;
}

Stream *Camera::reuseStream(camera3_stream_t *astream)
{
    Stream *priv = reinterpret_cast<Stream*>(astream->priv);
    // Verify the re-used stream's parameters match
    if (!priv->isValidReuseStream(mId, astream)) {
        ALOGE("%s:%d: Mismatched parameter in reused stream", __func__, mId);
        return NULL;
    }
    // Mark stream to be reused
    priv->mReuse = true;
    return priv;
}

bool Camera::isValidStreamSet(Stream **streams, int count, uint32_t mode)
{
    int inputs = 0;
    int outputs = 0;

    if (streams == NULL) {
        ALOGE("%s:%d: NULL stream configuration streams", __func__, mId);
        return false;
    }
    if (count == 0) {
        ALOGE("%s:%d: Zero count stream configuration streams", __func__, mId);
        return false;
    }
    // Validate there is at most one input stream and at least one output stream
    for (int i = 0; i < count; i++) {
        // A stream may be both input and output (bidirectional)
        if (streams[i]->isInputType())
            inputs++;
        if (streams[i]->isOutputType())
            outputs++;
    }
    ALOGV("%s:%d: Configuring %d output streams and %d input streams",
            __func__, mId, outputs, inputs);
    if (outputs < 1) {
        ALOGE("%s:%d: Stream config must have >= 1 output", __func__, mId);
        return false;
    }
    if (inputs > 1) {
        ALOGE("%s:%d: Stream config must have <= 1 input", __func__, mId);
        return false;
    }

    // check for correct number of Bayer/YUV/JPEG/Encoder streams
    return isSupportedStreamSet(streams, count, mode);
}

int Camera::setupStreams(Stream **streams, int count)
{
    /*
     * This is where the HAL has to decide internally how to handle all of the
     * streams, and then produce usage and max_buffer values for each stream.
     * Note, the stream array has been checked before this point for ALL invalid
     * conditions, so it must find a successful configuration for this stream
     * array. The only errors should be from individual streams requesting
     * unsupported features (such as data_space or rotation).
     */
    for (int i = 0; i < count; i++) {
        uint32_t usage = 0;
        if (streams[i]->isOutputType())
            usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
        if (streams[i]->isInputType())
            usage |= GRALLOC_USAGE_SW_READ_OFTEN;
        streams[i]->setUsage(usage);

        uint32_t max_buffers;
        int res = setupStream(streams[i], &max_buffers);
        if (res) {
          return res;
        }
        streams[i]->setMaxBuffers(max_buffers);
    }
    return 0;
}

bool Camera::isValidTemplateType(int type)
{
    return type > 0 && type < CAMERA3_TEMPLATE_COUNT;
}

const camera_metadata_t* Camera::constructDefaultRequestSettings(int type)
{
    ALOGV("%s:%d: type=%d", __func__, mId, type);

    if (!isValidTemplateType(type)) {
        ALOGE("%s:%d: Invalid template request type: %d", __func__, mId, type);
        return NULL;
    }

    if (!mTemplates[type]) {
        // Initialize this template if it hasn't been initialized yet.
        std::unique_ptr<android::CameraMetadata> new_template =
            std::make_unique<android::CameraMetadata>();
        int res = initTemplate(type, new_template.get());
        if (res || !new_template) {
            ALOGE("%s:%d: Failed to generate template of type: %d",
                  __func__, mId, type);
            return NULL;
        }
        mTemplates[type] = std::move(new_template);
    }

    // The "locking" here only causes non-const methods to fail,
    // which is not a problem since the CameraMetadata being locked
    // is already const. Destructing automatically "unlocks".
    return mTemplates[type]->getAndLock();
}

int Camera::processCaptureRequest(camera3_capture_request_t *temp_request)
{
    int res;

    ALOGV("%s:%d: request=%p", __func__, mId, temp_request);
    ATRACE_CALL();

    if (temp_request == NULL) {
        ALOGE("%s:%d: NULL request recieved", __func__, mId);
        return -EINVAL;
    }

    // Make a persistent copy of request, since otherwise it won't live
    // past the end of this method.
    std::shared_ptr<CaptureRequest> request = std::make_shared<CaptureRequest>(temp_request);

    ALOGV("%s:%d: Request Frame:%d", __func__, mId,
            request->frame_number);

    // Null/Empty indicates use last settings
    if (request->settings.isEmpty() && !mSettingsSet) {
        ALOGE("%s:%d: NULL settings without previous set Frame:%d",
              __func__, mId, request->frame_number);
        return -EINVAL;
    }

    if (request->input_buffer != NULL) {
        ALOGV("%s:%d: Reprocessing input buffer %p", __func__, mId,
              request->input_buffer.get());
    } else {
        ALOGV("%s:%d: Capturing new frame.", __func__, mId);
    }

    if (!isValidRequest(*request)) {
        ALOGE("%s:%d: Invalid request.", __func__, mId);
        return -EINVAL;
    }
    // Valid settings have been provided (mSettingsSet is a misnomer;
    // all that matters is that a previous request with valid settings
    // has been passed to the device, not that they've been set).
    mSettingsSet = true;

    // Pre-process output buffers.
    if (request->output_buffers.size() <= 0) {
        ALOGE("%s:%d: Invalid number of output buffers: %d", __func__, mId,
              request->output_buffers.size());
        return -EINVAL;
    }
    for (auto& output_buffer : request->output_buffers) {
        res = preprocessCaptureBuffer(&output_buffer);
        if (res)
            return -ENODEV;
    }

    // Send the request off to the device for completion.
    enqueueRequest(request);

    // Request is now in flight. The device will call completeRequest
    // asynchronously when it is done filling buffers and metadata.
    // TODO(b/31653306): Track requests in flight to ensure not too many are
    // sent at a time, and so they can be dumped even if the device loses them.
    return 0;
}

void Camera::completeRequest(std::shared_ptr<CaptureRequest> request, int err)
{
    // TODO(b/31653306): make sure this is actually a request in flight,
    // and not a random new one or a cancelled one. If so, stop tracking.
    if (err) {
        ALOGE("%s:%d: Error completing request for frame %d.",
              __func__, mId, request->frame_number);
        completeRequestWithError(request);
        return;
    }

    // Notify the framework with the shutter time (extracted from the result).
    int64_t timestamp = 0;
    // TODO(b/31360070): The general metadata methods should be part of the
    // default_camera_hal namespace, not the v4l2_camera_hal namespace.
    int res = v4l2_camera_hal::SingleTagValue(
        request->settings, ANDROID_SENSOR_TIMESTAMP, &timestamp);
    if (res) {
        ALOGE("%s:%d: Request for frame %d is missing required metadata.",
              __func__, mId, request->frame_number);
        // TODO(b/31653322): Send RESULT error.
        // For now sending REQUEST error instead.
        completeRequestWithError(request);
        return;
    }
    notifyShutter(request->frame_number, timestamp);

    // TODO(b/31653322): Check all returned buffers for errors
    // (if any, send BUFFER error).

    sendResult(request);
}

int Camera::preprocessCaptureBuffer(camera3_stream_buffer_t *buffer)
{
    int res;
    // TODO(b/29334616): This probably should be non-blocking; part
    // of the asynchronous request processing.
    if (buffer->acquire_fence != -1) {
        res = sync_wait(buffer->acquire_fence, CAMERA_SYNC_TIMEOUT);
        if (res == -ETIME) {
            ALOGE("%s:%d: Timeout waiting on buffer acquire fence",
                    __func__, mId);
            return res;
        } else if (res) {
            ALOGE("%s:%d: Error waiting on buffer acquire fence: %s(%d)",
                    __func__, mId, strerror(-res), res);
            return res;
        }
    }

    // Acquire fence has been waited upon.
    buffer->acquire_fence = -1;
    // No release fence waiting unless the device sets it.
    buffer->release_fence = -1;

    buffer->status = CAMERA3_BUFFER_STATUS_OK;
    return 0;
}

void Camera::notifyShutter(uint32_t frame_number, uint64_t timestamp)
{
    camera3_notify_msg_t message;
    memset(&message, 0, sizeof(message));
    message.type = CAMERA3_MSG_SHUTTER;
    message.message.shutter.frame_number = frame_number;
    message.message.shutter.timestamp = timestamp;
    mCallbackOps->notify(mCallbackOps, &message);
}

void Camera::completeRequestWithError(std::shared_ptr<CaptureRequest> request)
{
    // Send an error notification.
    camera3_notify_msg_t message;
    memset(&message, 0, sizeof(message));
    message.type = CAMERA3_MSG_ERROR;
    message.message.error.frame_number = request->frame_number;
    message.message.error.error_stream = nullptr;
    message.message.error.error_code = CAMERA3_MSG_ERROR_REQUEST;
    mCallbackOps->notify(mCallbackOps, &message);

    // TODO(b/31856611): Ensure all the buffers indicate their error status.

    // Send the errored out result.
    sendResult(request);
}

void Camera::sendResult(std::shared_ptr<CaptureRequest> request) {
    // Fill in the result struct
    // (it only needs to live until the end of the framework callback).
    camera3_capture_result_t result {
        request->frame_number,
        request->settings.getAndLock(),
        request->output_buffers.size(),
        request->output_buffers.data(),
        request->input_buffer.get(),
        1  // Total result; only 1 part.
    };
    // Make the framework callback.
    mCallbackOps->process_capture_result(mCallbackOps, &result);
}

void Camera::dump(int fd)
{
    ALOGV("%s:%d: Dumping to fd %d", __func__, mId, fd);
    ATRACE_CALL();
    android::Mutex::Autolock al(mDeviceLock);

    dprintf(fd, "Camera ID: %d (Busy: %d)\n", mId, mBusy);

    // TODO: dump all settings

    dprintf(fd, "Number of streams: %d\n", mNumStreams);
    for (int i = 0; i < mNumStreams; i++) {
        dprintf(fd, "Stream %d/%d:\n", i, mNumStreams);
        mStreams[i]->dump(fd);
    }
}

const char* Camera::templateToString(int type)
{
    switch (type) {
    case CAMERA3_TEMPLATE_PREVIEW:
        return "CAMERA3_TEMPLATE_PREVIEW";
    case CAMERA3_TEMPLATE_STILL_CAPTURE:
        return "CAMERA3_TEMPLATE_STILL_CAPTURE";
    case CAMERA3_TEMPLATE_VIDEO_RECORD:
        return "CAMERA3_TEMPLATE_VIDEO_RECORD";
    case CAMERA3_TEMPLATE_VIDEO_SNAPSHOT:
        return "CAMERA3_TEMPLATE_VIDEO_SNAPSHOT";
    case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
        return "CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG";
    }
    // TODO: support vendor templates
    return "Invalid template type!";
}

extern "C" {
// Get handle to camera from device priv data
static Camera *camdev_to_camera(const camera3_device_t *dev)
{
    return reinterpret_cast<Camera*>(dev->priv);
}

static int initialize(const camera3_device_t *dev,
        const camera3_callback_ops_t *callback_ops)
{
    return camdev_to_camera(dev)->initialize(callback_ops);
}

static int configure_streams(const camera3_device_t *dev,
        camera3_stream_configuration_t *stream_list)
{
    return camdev_to_camera(dev)->configureStreams(stream_list);
}

static const camera_metadata_t *construct_default_request_settings(
        const camera3_device_t *dev, int type)
{
    return camdev_to_camera(dev)->constructDefaultRequestSettings(type);
}

static int process_capture_request(const camera3_device_t *dev,
        camera3_capture_request_t *request)
{
    return camdev_to_camera(dev)->processCaptureRequest(request);
}

static void dump(const camera3_device_t *dev, int fd)
{
    camdev_to_camera(dev)->dump(fd);
}

static int flush(const camera3_device_t*)
{
    // TODO(b/29937783)
    ALOGE("%s: unimplemented.", __func__);
    return -1;
}

} // extern "C"

const camera3_device_ops_t Camera::sOps = {
    .initialize = default_camera_hal::initialize,
    .configure_streams = default_camera_hal::configure_streams,
    .register_stream_buffers = nullptr,
    .construct_default_request_settings
        = default_camera_hal::construct_default_request_settings,
    .process_capture_request = default_camera_hal::process_capture_request,
    .get_metadata_vendor_tag_ops = nullptr,
    .dump = default_camera_hal::dump,
    .flush = default_camera_hal::flush,
    .reserved = {0},
};

}  // namespace default_camera_hal
