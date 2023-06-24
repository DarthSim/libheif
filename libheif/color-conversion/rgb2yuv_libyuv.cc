/*
 * HEIF codec.
 * Copyright (c) 2023, Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cassert>
#include <memory>
#include <vector>
#include "rgb2yuv_libyuv.h"

#ifdef HAVE_LIBYUV

#include <libyuv.h>
#include "libheif/nclx.h"
#include "libheif/common_utils.h"

typedef int (*RGBToY)(const uint8_t*, int, uint8_t*, int, int, int);
typedef int (*RGBToYuv)(const uint8_t*, int,
    uint8_t*, int, uint8_t*, int, uint8_t*, int, int, int);

#endif

std::vector<ColorStateWithCost>
Op_Any_RGB_to_YCbCr_420_400_Libyuv::state_after_conversion(
    const ColorState& input_state, const ColorState& target_state,
    const heif_color_conversion_options& options) const
{
#ifdef HAVE_LIBYUV
  // this Op only implements the libyuv algorithm

  // Note: no input alpha channel required. It will be filled up with 0xFF.

  if (input_state.colorspace != heif_colorspace_RGB ||
      (
          input_state.chroma != heif_chroma_interleaved_RGB &&
          input_state.chroma != heif_chroma_interleaved_RGBA)) {
    return {};
  }

  if (input_state.bits_per_pixel != 8 || target_state.bits_per_pixel != 8) {
    return {};
  }

  if (target_state.chroma != heif_chroma_420 &&
      target_state.chroma != heif_chroma_monochrome) {
    return {};
  }

  bool full_range = target_state.nclx_profile.get_full_range_flag();

  const uint16_t matrix_coefficients = target_state.nclx_profile.get_matrix_coefficients();
  if (matrix_coefficients != heif_matrix_coefficients_ITU_R_BT_470_6_System_B_G &&
      matrix_coefficients != heif_matrix_coefficients_ITU_R_BT_601_6) {
    return {};
  }

  if (!full_range && target_state.chroma == heif_chroma_monochrome) {
    return {};
  }

  std::vector<ColorStateWithCost> states;

  ColorState output_state;

  output_state.colorspace = heif_colorspace_YCbCr;
  output_state.chroma = target_state.chroma;
  output_state.has_alpha = target_state.has_alpha;
  output_state.bits_per_pixel = 8;
  output_state.nclx_profile = target_state.nclx_profile;
  states.push_back({output_state, SpeedCosts_OptimizedSoftware});

  return states;
#else
  return {};
#endif
}

std::shared_ptr<HeifPixelImage>
Op_Any_RGB_to_YCbCr_420_400_Libyuv::convert_colorspace(
    const std::shared_ptr<const HeifPixelImage>& input,
    const ColorState& input_state,
    const ColorState& target_state,
    const heif_color_conversion_options& options) const
{
#ifdef HAVE_LIBYUV
  int width = input->get_width();
  int height = input->get_height();

  auto outimg = std::make_shared<HeifPixelImage>();

  heif_chroma input_chroma = input->get_chroma_format();
  heif_chroma output_chroma = target_state.chroma;

  assert(input_chroma == heif_chroma_interleaved_RGB ||
         input_chroma == heif_chroma_interleaved_RGBA);

  assert(output_chroma == heif_chroma_420 ||
         output_chroma == heif_chroma_monochrome);

  int output_bits = target_state.bits_per_pixel;

  assert(output_bits == 8);

  outimg->create(width, height, heif_colorspace_YCbCr, output_chroma);

  bool has_alpha = input->get_chroma_format() == heif_chroma_interleaved_RGBA;
  bool want_alpha = target_state.has_alpha;

  if (!outimg->add_plane(heif_channel_Y, width, height, output_bits)) {
    return nullptr;
  }

  if (output_chroma != heif_chroma_monochrome) {
    uint8_t chromaSubH = chroma_h_subsampling(output_chroma);
    uint8_t chromaSubV = chroma_v_subsampling(output_chroma);

    int chroma_width = (width + chromaSubH - 1) / chromaSubH;
    int chroma_height = (height + chromaSubV - 1) / chromaSubV;

    if (!outimg->add_plane(heif_channel_Cb, chroma_width, chroma_height, output_bits) ||
        !outimg->add_plane(heif_channel_Cr, chroma_width, chroma_height, output_bits)) {
      return nullptr;
    }
  }

  if (want_alpha) {
    if (!outimg->add_plane(heif_channel_Alpha, width, height, output_bits)) {
      return nullptr;
    }
  }

  const uint8_t* in_a = nullptr;
  int in_stride = 0;
  int in_a_stride = 0;

  const uint8_t* in_p = input->get_plane(heif_channel_interleaved, &in_stride);
  if (has_alpha) {
    in_a = &in_p[3];
    in_a_stride = in_stride;
  }

  int out_cb_stride = 0, out_cr_stride = 0, out_y_stride = 0;
  uint8_t* out_cb, *out_cr;

  uint8_t* out_y = outimg->get_plane(heif_channel_Y, &out_y_stride);
  if (output_chroma != heif_chroma_monochrome) {
    out_cb = outimg->get_plane(heif_channel_Cb, &out_cb_stride);
    out_cr = outimg->get_plane(heif_channel_Cr, &out_cr_stride);
  }

  bool full_range = target_state.nclx_profile.get_full_range_flag();
  assert(full_range || output_chroma != heif_chroma_monochrome);

  int yuv_err = 0;

  if (output_chroma == heif_chroma_monochrome) {
    RGBToY convert = nullptr;

    switch (input_chroma) {
    case heif_chroma_interleaved_RGB:
      convert = libyuv::RAWToJ400;
      break;
    case heif_chroma_interleaved_RGBA:
      convert = libyuv::ABGRToJ400;
      break;
    default:
      return nullptr;
    }

    yuv_err = convert(in_p, in_stride, out_y, out_y_stride,
                     input->get_width(), input->get_height());
  } else {
    RGBToYuv convert = nullptr;

    switch (input_chroma) {
    case heif_chroma_interleaved_RGB:
      convert = full_range ? libyuv::RAWToJ420 : libyuv::RAWToI420;
      break;
    case heif_chroma_interleaved_RGBA:
      convert = full_range ? libyuv::ABGRToJ420 : libyuv::ABGRToI420;
      break;
    default:
      return nullptr;
    }

    yuv_err = convert(in_p, in_stride, out_y, out_y_stride, out_cb, out_cb_stride,
                     out_cr, out_cr_stride, input->get_width(), input->get_height());
  }

  if (yuv_err) {
    return nullptr;
  }

  if (want_alpha) {
    int out_a_stride;
    uint8_t* out_a = outimg->get_plane(heif_channel_Alpha, &out_a_stride);
    int rgb_step = has_alpha ? 4 : 3;

    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        const uint8_t* in = has_alpha ? &in_a[y * in_a_stride + x * rgb_step] : nullptr;
        out_a[y * out_a_stride + x] = has_alpha ? in[0] : 255;
      }
    }
  }

  return outimg;
#else
  return nullptr;
#endif
}
