/*
 * Copyright 2016 The Android Open Source Project
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

#include "v4l2_wrapper.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <mutex>

#include <nativehelper/ScopedFd.h>

#include "common.h"
#include "stream.h"
#include "stream_format.h"
#include "v4l2_gralloc.h"

namespace v4l2_camera_hal {

V4L2Wrapper* V4L2Wrapper::NewV4L2Wrapper(const std::string device_path) {
  HAL_LOG_ENTER();

  std::unique_ptr<V4L2Gralloc> gralloc(V4L2Gralloc::NewV4L2Gralloc());
  if (!gralloc) {
    HAL_LOGE("Failed to initialize gralloc helper.");
    return nullptr;
  }

  return new V4L2Wrapper(device_path, std::move(gralloc));
}

V4L2Wrapper::V4L2Wrapper(const std::string device_path,
                         std::unique_ptr<V4L2Gralloc> gralloc)
    : device_path_(std::move(device_path)),
      gralloc_(std::move(gralloc)),
      max_buffers_(0) {
  HAL_LOG_ENTER();
}

V4L2Wrapper::~V4L2Wrapper() { HAL_LOG_ENTER(); }

int V4L2Wrapper::Connect() {
  HAL_LOG_ENTER();
  std::lock_guard<std::mutex> lock(device_lock_);

  if (connected()) {
    HAL_LOGE("Camera device %s is already connected. Close it first",
             device_path_.c_str());
    return -EIO;
  }

  int fd = TEMP_FAILURE_RETRY(open(device_path_.c_str(), O_RDWR));
  if (fd < 0) {
    HAL_LOGE("failed to open %s (%s)", device_path_.c_str(), strerror(errno));
    return -errno;
  }
  device_fd_.reset(fd);

  // Check if this connection has the extended control query capability.
  v4l2_query_ext_ctrl query;
  query.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
  // Already holding the lock, so don't call IoctlLocked.
  int res = TEMP_FAILURE_RETRY(
      ioctl(device_fd_.get(), VIDIOC_QUERY_EXT_CTRL, &query));
  extended_query_supported_ = (res == 0);

  // TODO(b/29185945): confirm this is a supported device.
  // This is checked by the HAL, but the device at device_path_ may
  // not be the same one that was there when the HAL was loaded.
  // (Alternatively, better hotplugging support may make this unecessary
  // by disabling cameras that get disconnected and checking newly connected
  // cameras, so Connect() is never called on an unsupported camera)
  return 0;
}

void V4L2Wrapper::Disconnect() {
  HAL_LOG_ENTER();
  std::lock_guard<std::mutex> lock(device_lock_);

  device_fd_.reset();  // Includes close().
  format_.reset();
  max_buffers_ = 0;
  // Closing the device releases all queued buffers back to the user.
  gralloc_->unlockAllBuffers();
}

// Helper function. Should be used instead of ioctl throughout this class.
template <typename T>
int V4L2Wrapper::IoctlLocked(int request, T data) {
  HAL_LOG_ENTER();
  std::lock_guard<std::mutex> lock(device_lock_);

  if (!connected()) {
    HAL_LOGE("Device %s not connected.", device_path_.c_str());
    return -ENODEV;
  }
  return TEMP_FAILURE_RETRY(ioctl(device_fd_.get(), request, data));
}

int V4L2Wrapper::StreamOn() {
  HAL_LOG_ENTER();

  if (!format_) {
    HAL_LOGE("Stream format must be set before turning on stream.");
    return -EINVAL;
  }

  int32_t type = format_->type();
  if (IoctlLocked(VIDIOC_STREAMON, &type) < 0) {
    HAL_LOGE("STREAMON fails: %s", strerror(errno));
    return -ENODEV;
  }

  return 0;
}

int V4L2Wrapper::StreamOff() {
  HAL_LOG_ENTER();

  if (!format_) {
    HAL_LOGE("Stream format must be set to turn off stream.");
    return -ENODEV;
  }

  int32_t type = format_->type();
  int res = IoctlLocked(VIDIOC_STREAMOFF, &type);
  // Calling STREAMOFF releases all queued buffers back to the user.
  int gralloc_res = gralloc_->unlockAllBuffers();
  if (res < 0) {
    HAL_LOGE("STREAMOFF fails: %s", strerror(errno));
    return -ENODEV;
  }
  if (gralloc_res < 0) {
    HAL_LOGE("Failed to unlock all buffers after turning stream off.");
    return gralloc_res;
  }

  return 0;
}

int V4L2Wrapper::QueryControl(uint32_t control_id,
                              v4l2_query_ext_ctrl* result) {
  HAL_LOG_ENTER();
  int res;

  memset(result, 0, sizeof(*result));

  if (extended_query_supported_) {
    result->id = control_id;
    res = IoctlLocked(VIDIOC_QUERY_EXT_CTRL, result);
    // Assuming the operation was supported (not ENOTTY), no more to do.
    if (errno != ENOTTY) {
      if (res) {
        HAL_LOGE("QUERY_EXT_CTRL fails: %s", strerror(errno));
        return -ENODEV;
      }
      return 0;
    }
  }

  // Extended control querying not supported, fall back to basic control query.
  v4l2_queryctrl query;
  query.id = control_id;
  if (IoctlLocked(VIDIOC_QUERYCTRL, &query)) {
    HAL_LOGE("QUERYCTRL fails: %s", strerror(errno));
    return -ENODEV;
  }

  // Convert the basic result to the extended result.
  result->id = query.id;
  result->type = query.type;
  memcpy(result->name, query.name, sizeof(query.name));
  result->minimum = query.minimum;
  if (query.type == V4L2_CTRL_TYPE_BITMASK) {
    // According to the V4L2 documentation, when type is BITMASK,
    // max and default should be interpreted as __u32. Practically,
    // this means the conversion from 32 bit to 64 will pad with 0s not 1s.
    result->maximum = static_cast<uint32_t>(query.maximum);
    result->default_value = static_cast<uint32_t>(query.default_value);
  } else {
    result->maximum = query.maximum;
    result->default_value = query.default_value;
  }
  result->step = static_cast<uint32_t>(query.step);
  result->flags = query.flags;
  result->elems = 1;
  switch (result->type) {
    case V4L2_CTRL_TYPE_INTEGER64:
      result->elem_size = sizeof(int64_t);
      break;
    case V4L2_CTRL_TYPE_STRING:
      result->elem_size = result->maximum + 1;
      break;
    default:
      result->elem_size = sizeof(int32_t);
      break;
  }

  return 0;
}

int V4L2Wrapper::GetControl(uint32_t control_id, int32_t* value) {
  HAL_LOG_ENTER();

  v4l2_control control;
  control.id = control_id;
  if (IoctlLocked(VIDIOC_G_CTRL, &control) < 0) {
    HAL_LOGE("G_CTRL fails: %s", strerror(errno));
    return -ENODEV;
  }
  *value = control.value;
  return 0;
}

int V4L2Wrapper::SetControl(uint32_t control_id, int32_t desired,
                            int32_t* result) {
  HAL_LOG_ENTER();

  // TODO(b/29334616): When async, this may need to check if the stream
  // is on, and if so, lock it off while setting format. Need to look
  // into if V4L2 supports adjusting controls while the stream is on.

  v4l2_control control{control_id, desired};
  if (IoctlLocked(VIDIOC_S_CTRL, &control) < 0) {
    HAL_LOGE("S_CTRL fails: %s", strerror(errno));
    return -ENODEV;
  }
  // If the caller wants to know the result, pass it back.
  if (result != nullptr) {
    *result = control.value;
  }
  return 0;
}

int V4L2Wrapper::SetFormat(const default_camera_hal::Stream& stream,
                           uint32_t* result_max_buffers) {
  HAL_LOG_ENTER();

  // Should be checked earlier; sanity check.
  if (stream.isInputType()) {
    HAL_LOGE("Input streams not supported.");
    return -EINVAL;
  }

  StreamFormat desired_format(stream);
  if (format_ && desired_format == *format_) {
    HAL_LOGV("Already in correct format, skipping format setting.");
    return 0;
  }

  // Not in the correct format, set our format.
  v4l2_format new_format;
  desired_format.FillFormatRequest(&new_format);
  // TODO(b/29334616): When async, this will need to check if the stream
  // is on, and if so, lock it off while setting format.
  if (IoctlLocked(VIDIOC_S_FMT, &new_format) < 0) {
    HAL_LOGE("S_FMT failed: %s", strerror(errno));
    return -ENODEV;
  }

  // Check that the driver actually set to the requested values.
  if (desired_format != new_format) {
    HAL_LOGE("Device doesn't support desired stream configuration.");
    return -EINVAL;
  }

  // Keep track of our new format.
  format_.reset(new StreamFormat(new_format));

  // Format changed, setup new buffers.
  int res = SetupBuffers();
  if (res) {
    HAL_LOGE("Failed to set up buffers for new format.");
    return res;
  }
  *result_max_buffers = max_buffers_;
  return 0;
}

int V4L2Wrapper::SetupBuffers() {
  HAL_LOG_ENTER();

  if (!format_) {
    HAL_LOGE("Stream format must be set before setting up buffers.");
    return -ENODEV;
  }

  // "Request" a buffer (since we're using a userspace buffer, this just
  // tells V4L2 to switch into userspace buffer mode).
  v4l2_requestbuffers req_buffers;
  memset(&req_buffers, 0, sizeof(req_buffers));
  req_buffers.type = format_->type();
  req_buffers.memory = V4L2_MEMORY_USERPTR;
  req_buffers.count = 1;

  int res = IoctlLocked(VIDIOC_REQBUFS, &req_buffers);
  // Calling REQBUFS releases all queued buffers back to the user.
  int gralloc_res = gralloc_->unlockAllBuffers();
  if (res < 0) {
    HAL_LOGE("REQBUFS failed: %s", strerror(errno));
    return -ENODEV;
  }
  if (gralloc_res < 0) {
    HAL_LOGE("Failed to unlock all buffers when setting up new buffers.");
    return gralloc_res;
  }

  // V4L2 will set req_buffers.count to a number of buffers it can handle.
  max_buffers_ = req_buffers.count;
  // Sanity check.
  if (max_buffers_ < 1) {
    HAL_LOGE("REQBUFS claims it can't handle any buffers.");
    return -ENODEV;
  }
  return 0;
}

int V4L2Wrapper::EnqueueBuffer(const camera3_stream_buffer_t* camera_buffer) {
  HAL_LOG_ENTER();

  if (!format_) {
    HAL_LOGE("Stream format must be set before enqueuing buffers.");
    return -ENODEV;
  }

  // Set up a v4l2 buffer struct.
  v4l2_buffer device_buffer;
  memset(&device_buffer, 0, sizeof(device_buffer));
  device_buffer.type = format_->type();

  // Use QUERYBUF to ensure our buffer/device is in good shape.
  if (IoctlLocked(VIDIOC_QUERYBUF, &device_buffer) < 0) {
    HAL_LOGE("QUERYBUF fails: %s", strerror(errno));
    return -ENODEV;
  }

  // Configure the device buffer based on the stream buffer.
  device_buffer.memory = V4L2_MEMORY_USERPTR;
  // TODO(b/29334616): when this is async, actually limit the number
  // of buffers used to the known max, and set this according to the
  // queue length.
  device_buffer.index = 0;
  // Lock the buffer for writing.
  int res =
      gralloc_->lock(camera_buffer, format_->bytes_per_line(), &device_buffer);
  if (res) {
    HAL_LOGE("Gralloc failed to lock buffer.");
    return res;
  }
  if (IoctlLocked(VIDIOC_QBUF, &device_buffer) < 0) {
    HAL_LOGE("QBUF (%d) fails: %s", 0, strerror(errno));
    gralloc_->unlock(&device_buffer);
    return -ENODEV;
  }

  return 0;
}

int V4L2Wrapper::DequeueBuffer(v4l2_buffer* buffer) {
  HAL_LOG_ENTER();

  if (!format_) {
    HAL_LOGE("Stream format must be set before dequeueing buffers.");
    return -ENODEV;
  }

  memset(buffer, 0, sizeof(*buffer));
  buffer->type = format_->type();
  buffer->memory = V4L2_MEMORY_USERPTR;
  if (IoctlLocked(VIDIOC_DQBUF, buffer) < 0) {
    HAL_LOGE("DQBUF fails: %s", strerror(errno));
    return -ENODEV;
  }

  // Now that we're done painting the buffer, we can unlock it.
  int res = gralloc_->unlock(buffer);
  if (res) {
    HAL_LOGE("Gralloc failed to unlock buffer after dequeueing.");
    return res;
  }

  return 0;
}

}  // namespace v4l2_camera_hal
