// Copyright 2018 Xiaomi, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MACE_KERNELS_RESIZE_BILINEAR_H_
#define MACE_KERNELS_RESIZE_BILINEAR_H_

#include <algorithm>
#include <memory>
#include <vector>

#include "mace/core/future.h"
#include "mace/core/tensor.h"
#include "mace/kernels/kernel.h"
#include "mace/utils/quantize.h"

namespace mace {
namespace kernels {

struct CachedInterpolation {
  index_t lower;  // Lower source index used in the interpolation
  index_t upper;  // Upper source index used in the interpolation
  // 1-D linear iterpolation scale (see:
  // https://en.wikipedia.org/wiki/Bilinear_interpolation)
  float lerp;
};

inline float CalculateResizeScale(index_t in_size,
                                  index_t out_size,
                                  bool align_corners) {
  return (align_corners && out_size > 1)
         ? (in_size - 1) / static_cast<float>(out_size - 1)
         : in_size / static_cast<float>(out_size);
}

inline void ComputeInterpolationWeights(
    const index_t out_size,
    const index_t in_size,
    const float scale,
    CachedInterpolation *interpolation) {
  interpolation[out_size].lower = 0;
  interpolation[out_size].upper = 0;
  for (index_t i = out_size - 1; i >= 0; --i) {
    const float in = i * scale;
    interpolation[i].lower = static_cast<index_t>(in);
    interpolation[i].upper = std::min(interpolation[i].lower + 1, in_size - 1);
    interpolation[i].lerp = in - interpolation[i].lower;
  }
}

template <typename T>
inline T ComputeLerp(const T top_left,
                     const T top_right,
                     const T bottom_left,
                     const T bottom_right,
                     const float x_lerp,
                     const float y_lerp);

template <>
inline float ComputeLerp<float>(const float top_left,
                                const float top_right,
                                const float bottom_left,
                                const float bottom_right,
                                const float x_lerp,
                                const float y_lerp) {
  const float top = top_left + (top_right - top_left) * x_lerp;
  const float bottom = bottom_left + (bottom_right - bottom_left) * x_lerp;
  return top + (bottom - top) * y_lerp;
}

template <>
inline uint8_t ComputeLerp<uint8_t>(const uint8_t top_left,
                                    const uint8_t top_right,
                                    const uint8_t bottom_left,
                                    const uint8_t bottom_right,
                                    const float x_lerp,
                                    const float y_lerp) {
  const float top = top_left + (top_right - top_left) * x_lerp;
  const float bottom = bottom_left + (bottom_right - bottom_left) * x_lerp;
  return Saturate<uint8_t>(roundf(top + (bottom - top) * y_lerp));
}

template <typename T>
inline void ResizeImageNCHW(const T *images,
                            const index_t batch_size,
                            const index_t in_height,
                            const index_t in_width,
                            const index_t out_height,
                            const index_t out_width,
                            const index_t channels,
                            const std::vector<CachedInterpolation> &xs_vec,
                            const std::vector<CachedInterpolation> &ys,
                            T *output) {
  const CachedInterpolation *xs = xs_vec.data();

#pragma omp parallel for collapse(2)
  for (index_t b = 0; b < batch_size; ++b) {
    for (index_t c = 0; c < channels; ++c) {
      const T
          *channel_input_ptr =
          images + (b * channels + c) * in_height * in_width;
      T *channel_output_ptr =
          output + (b * channels + c) * out_height * out_width;
      for (index_t y = 0; y < out_height; ++y) {
        const T *y_lower_input_ptr =
            channel_input_ptr + ys[y].lower * in_width;
        const T *y_upper_input_ptr =
            channel_input_ptr + ys[y].upper * in_width;
        const float ys_lerp = ys[y].lerp;

        for (index_t x = 0; x < out_width; ++x) {
          const float xs_lerp = xs[x].lerp;
          const T top_left = y_lower_input_ptr[xs[x].lower];
          const T top_right = y_lower_input_ptr[xs[x].upper];
          const T bottom_left = y_upper_input_ptr[xs[x].lower];
          const T bottom_right = y_upper_input_ptr[xs[x].upper];
          channel_output_ptr[y * out_width + x] =
              ComputeLerp(top_left, top_right, bottom_left,
                          bottom_right, xs_lerp, ys_lerp);
        }
      }
    }
  }
}

template <typename T>
inline void ResizeImageNHWC(const T *images,
                            const index_t batch_size,
                            const index_t in_height,
                            const index_t in_width,
                            const index_t out_height,
                            const index_t out_width,
                            const index_t channels,
                            const std::vector<CachedInterpolation> &xs_vec,
                            const std::vector<CachedInterpolation> &ys,
                            T *output) {
  const CachedInterpolation *xs = xs_vec.data();

  for (index_t b = 0; b < batch_size; ++b) {
    const T *input_base = images + b * channels * in_height * in_width;
    T *output_base = output + b * channels * out_height * out_width;
#pragma omp parallel for
    for (index_t y = 0; y < out_height; ++y) {
      const T
          *y_lower_input_ptr = input_base + ys[y].lower * in_width * channels;
      const T
          *y_upper_input_ptr = input_base + ys[y].upper * in_width * channels;
      const float ys_lerp = ys[y].lerp;

      for (index_t x = 0; x < out_width; ++x) {
        const float xs_lerp = xs[x].lerp;
        const T *top_left = y_lower_input_ptr + xs[x].lower * channels;
        const T *top_right = y_lower_input_ptr + xs[x].upper * channels;
        const T *bottom_left = y_upper_input_ptr + xs[x].lower * channels;
        const T *bottom_right = y_upper_input_ptr + xs[x].upper * channels;

        T *output_ptr = output_base + (y * out_width + x) * channels;
        for (index_t c = 0; c < channels; ++c) {
          output_ptr[c] =
              ComputeLerp(top_left[c], top_right[c], bottom_left[c],
                          bottom_right[c], xs_lerp, ys_lerp);
        }
      }
    }
  }
}

template<DeviceType D, typename T>
struct ResizeBilinearFunctor : OpKernel {
  ResizeBilinearFunctor(OpKernelContext *context,
                        const std::vector<index_t> &size,
                        bool align_corners)
      : OpKernel(context), align_corners_(align_corners) {
    MACE_CHECK(size.size() == 2);
    out_height_ = size[0];
    out_width_ = size[1];
  }

  MaceStatus operator()(const Tensor *input,
                        Tensor *output,
                        StatsFuture *future) {
    MACE_UNUSED(future);
    const index_t batch = input->dim(0);
    const index_t channels = input->dim(1);
    const index_t in_height = input->dim(2);
    const index_t in_width = input->dim(3);

    index_t out_height = out_height_;
    index_t out_width = out_width_;
    MACE_CHECK(out_height > 0 && out_width > 0);
    std::vector<index_t> out_shape{batch, channels, out_height, out_width};
    MACE_RETURN_IF_ERROR(output->Resize(out_shape));

    Tensor::MappingGuard input_mapper(input);
    Tensor::MappingGuard output_mapper(output);
    const T *input_data = input->data<T>();
    T *output_data = output->mutable_data<T>();

    if (out_height == in_height && out_width == in_width) {
      std::copy(input_data,
                input_data + batch * channels * in_height * in_width,
                output_data);
      return MACE_SUCCESS;
    }

    float height_scale =
        CalculateResizeScale(in_height, out_height, align_corners_);
    float width_scale =
        CalculateResizeScale(in_width, out_width, align_corners_);

    std::vector<CachedInterpolation> ys(out_height + 1);
    std::vector<CachedInterpolation> xs(out_width + 1);

    // Compute the cached interpolation weights on the x and y dimensions.
    ComputeInterpolationWeights(out_height, in_height, height_scale, ys.data());
    ComputeInterpolationWeights(out_width, in_width, width_scale, xs.data());

    ResizeImageNCHW(input_data,
                    batch,
                    in_height,
                    in_width,
                    out_height,
                    out_width,
                    channels,
                    xs,
                    ys,
                    output_data);

    return MACE_SUCCESS;
  }

  bool align_corners_;
  index_t out_height_;
  index_t out_width_;
};

template<DeviceType D>
struct ResizeBilinearFunctor<D, uint8_t> : OpKernel {
  ResizeBilinearFunctor(OpKernelContext *context,
                        const std::vector<index_t> &size,
                        bool align_corners)
      : OpKernel(context), align_corners_(align_corners) {
    MACE_CHECK(size.size() == 2);
    out_height_ = size[0];
    out_width_ = size[1];
  }

  MaceStatus operator()(const Tensor *input,
                        Tensor *output,
                        StatsFuture *future) {
    MACE_UNUSED(future);
    const index_t batch = input->dim(0);
    const index_t in_height = input->dim(1);
    const index_t in_width = input->dim(2);
    const index_t channels = input->dim(3);

    index_t out_height = out_height_;
    index_t out_width = out_width_;
    MACE_CHECK(out_height > 0 && out_width > 0);
    std::vector<index_t> out_shape{batch, out_height, out_width, channels};
    MACE_RETURN_IF_ERROR(output->Resize(out_shape));

    Tensor::MappingGuard input_mapper(input);
    Tensor::MappingGuard output_mapper(output);
    const uint8_t *input_data = input->data<uint8_t>();
    uint8_t *output_data = output->mutable_data<uint8_t>();

    if (out_height == in_height && out_width == in_width) {
      std::copy(input_data,
                input_data + batch * in_height * in_width * channels ,
                output_data);
      return MACE_SUCCESS;
    }

    float height_scale =
        CalculateResizeScale(in_height, out_height, align_corners_);
    float width_scale =
        CalculateResizeScale(in_width, out_width, align_corners_);

    std::vector<CachedInterpolation> ys(out_height + 1);
    std::vector<CachedInterpolation> xs(out_width + 1);

    // Compute the cached interpolation weights on the x and y dimensions.
    ComputeInterpolationWeights(out_height, in_height, height_scale, ys.data());
    ComputeInterpolationWeights(out_width, in_width, width_scale, xs.data());

    ResizeImageNHWC(input_data,
                    batch,
                    in_height,
                    in_width,
                    out_height,
                    out_width,
                    channels,
                    xs,
                    ys,
                    output_data);

    return MACE_SUCCESS;
  }

  bool align_corners_;
  index_t out_height_;
  index_t out_width_;
};

#ifdef MACE_ENABLE_OPENCL
class OpenCLResizeBilinearKernel {
 public:
  virtual MaceStatus Compute(
      OpKernelContext *context,
      const Tensor *input,
      Tensor *output,
      StatsFuture *future) = 0;
  MACE_VIRTUAL_EMPTY_DESTRUCTOR(OpenCLResizeBilinearKernel);
};
template<typename T>
struct ResizeBilinearFunctor<DeviceType::GPU, T>
    : OpKernel {
  ResizeBilinearFunctor(OpKernelContext *context,
                        const std::vector<index_t> &size,
                        bool align_corners);

  MaceStatus operator()(const Tensor *input,
                        Tensor *output,
                        StatsFuture *future);

  std::unique_ptr<OpenCLResizeBilinearKernel> kernel_;
};
#endif  // MACE_ENABLE_OPENCL

}  // namespace kernels
}  // namespace mace

#endif  // MACE_KERNELS_RESIZE_BILINEAR_H_
