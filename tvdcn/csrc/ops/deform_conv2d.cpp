/*!
******************* BEGIN Caffe Copyright Notice and Disclaimer
*****************
*
* COPYRIGHT
*
* All contributions by the University of California:
* Copyright (c) 2014-2017 The Regents of the University of California (Regents)
* All rights reserved.
*
* All other contributions:
* Copyright (c) 2014-2017, the respective contributors
* All rights reserved.
*
* Caffe uses a shared copyright model: each contributor holds copyright over
* their contributions to Caffe. The project versioning records all such
* contribution and copyright details. If a contributor wants to further mark
* their specific copyright on a particular contribution, they should indicate
* their copyright solely in the commit message of the change when it is
* committed.
*
* LICENSE
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright notice,
* this list of conditions and the following disclaimer in the documentation
* and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
*AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
*IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
*FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
*DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
*SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
*OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
*OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* CONTRIBUTION AGREEMENT
*
* By contributing to the BVLC/caffe repository through pull-request, comment,
* or otherwise, the contributor releases their content to the
* license and copyright terms herein.
*
***************** END Caffe Copyright Notice and Disclaimer
*********************
*
* Copyright (c) 2018 Microsoft
* Licensed under The MIT License [see LICENSE for details]
* \file modulated_deformable_im2col.cuh
* \brief Function definitions of converting an image to
* column matrix based on kernel, padding, dilation, and offset.
* These functions are mainly used in deformable convolution operators.
* \ref: https://arxiv.org/abs/1703.06211
* \author Yuwen Xiong, Haozhi Qi, Jifeng Dai, Xizhou Zhu, Han Hu, Dazhi Cheng
*/

// modified from
// https://github.com/chengdazhi/Deformable-Convolution-V2-PyTorch/blob/mmdetection/mmdet/ops/dcn/src/deform_conv_cuda_kernel.cu

// modified from
// https://github.com/open-mmlab/mmdetection/blob/master/mmdet/ops/dcn/src/deform_conv_cuda.cpp

// modified from
// https://github.com/pytorch/vision/blob/master/torchvision/csrc/cuda/DeformConv_cuda.cpp

#include "utils/parallel_helpers.h"
#include "deform_conv2d_kernels.h"

namespace tvdcn {
    namespace ops {
        at::Tensor deform_conv2d_forward(
                const at::Tensor &input,
                const at::Tensor &weight,
                const at::Tensor &offset,
                const at::Tensor &mask,
                const at::Tensor &bias,
                const std::pair<int, int> &stride,
                const std::pair<int, int> &padding,
                const std::pair<int, int> &dilation,
                int groups,
                int offset_groups,
                int mask_groups,
                bool deformable,
                bool modulated) {
            at::Tensor input_c = input.contiguous();
            at::Tensor offset_c = offset.contiguous();
            at::Tensor mask_c = mask.contiguous();
            at::Tensor weight_c = weight.contiguous();
            at::Tensor bias_c = bias.contiguous();

            TORCH_CHECK(input_c.ndimension() == 4)
            TORCH_CHECK(!deformable || offset_c.ndimension() == 4)
            TORCH_CHECK(!modulated || mask_c.ndimension() == 4)
            TORCH_CHECK(weight_c.ndimension() == 4)
            TORCH_CHECK(input_c.is_contiguous())
            TORCH_CHECK(weight_c.is_contiguous())

            if (input_c.is_cuda())
                at::DeviceGuard guard(input_c.device());

            int batch_sz = input_c.size(0);
            int in_channels = input_c.size(1);
            int in_h = input_c.size(2);
            int in_w = input_c.size(3);

            int n_parallel_imgs = get_greatest_divisor_below_bound(batch_sz, kMaxParallelImgs);

            int out_channels = weight_c.size(0);
            int weight_h = weight_c.size(2);
            int weight_w = weight_c.size(3);

            int stride_h = stride.first;
            int stride_w = stride.second;

            int pad_h = padding.first;
            int pad_w = padding.second;

            int dilation_h = dilation.first;
            int dilation_w = dilation.second;

            int out_h = (in_h + 2 * pad_h - (dilation_h * (weight_h - 1) + 1)) / stride_h + 1;
            int out_w = (in_w + 2 * pad_w - (dilation_w * (weight_w - 1) + 1)) / stride_w + 1;

            TORCH_CHECK(
                    weight_h > 0 && weight_w > 0,
                    "weight_h: ",
                    weight_h,
                    " weight_w: ",
                    weight_w)
            TORCH_CHECK(
                    stride_h > 0 && stride_w > 0,
                    "stride_h: ",
                    stride_h,
                    " stride_w: ",
                    stride_w)
            TORCH_CHECK(pad_h >= 0 && pad_w >= 0, "pad_h: ", pad_h, " pad_w: ", pad_w)
            TORCH_CHECK(dilation_h > 0 && dilation_w > 0, "dilation_h: ", dilation_h, " dilation_w: ", dilation_w)

            TORCH_CHECK(weight_c.size(1) * groups == in_channels)
            TORCH_CHECK(weight_c.size(0) % groups == 0)
            TORCH_CHECK(!deformable || input_c.size(1) % offset_groups == 0)
            TORCH_CHECK(!modulated || input_c.size(1) % mask_groups == 0)

            TORCH_CHECK(
                    (!deformable || offset_c.size(1) == offset_groups * 2 * weight_h * weight_w),
                    "offset.shape[1] is not valid: got: ",
                    offset_c.size(1),
                    " expected: ",
                    offset_groups * 2 * weight_h * weight_w)
            TORCH_CHECK(
                    (offset_c.size(0) == input_c.size(0)), "invalid batch size of offset")
            TORCH_CHECK(
                    (!deformable || offset_c.size(2) == out_h && offset_c.size(3) == out_w),
                    "offset output dims: (",
                    offset_c.size(2),
                    ", ",
                    offset_c.size(3),
                    ") - ",
                    "computed output dims: (",
                    out_h,
                    ", ",
                    out_w,
                    ")")

            TORCH_CHECK(
                    (!modulated || mask_c.size(1) == mask_groups * weight_h * weight_w),
                    "mask.shape[1] is not valid: got: ",
                    mask_c.size(1),
                    " expected: ",
                    mask_groups * weight_h * weight_w)
            TORCH_CHECK(
                    (mask_c.size(0) == input_c.size(0)), "invalid batch size of mask")
            TORCH_CHECK(
                    (!modulated || (mask_c.size(2) == out_h && mask_c.size(3) == out_w)),
                    "mask output dims: (",
                    mask_c.size(2),
                    ", ",
                    mask_c.size(3),
                    ") - ",
                    "computed output dims: (",
                    out_h,
                    ", ",
                    out_w,
                    ")")

            TORCH_CHECK(
                    out_h > 0 && out_w > 0,
                    "Calculated output size too small - out_h: ",
                    out_h,
                    " out_w: ",
                    out_w)

            auto out = at::zeros({batch_sz, out_channels, out_h, out_w}, input_c.options());

            // Separate batches into blocks
            input_c = input_c.view(
                    {batch_sz / n_parallel_imgs, n_parallel_imgs, in_channels, in_h, in_w});
            if (deformable)
                offset_c = offset_c.view({batch_sz / n_parallel_imgs,
                                          n_parallel_imgs,
                                          offset_groups * 2 * weight_h * weight_w,
                                          out_h,
                                          out_w});
            if (modulated)
                mask_c = mask_c.view({batch_sz / n_parallel_imgs,
                                      n_parallel_imgs,
                                      mask_groups * weight_h * weight_w,
                                      out_h,
                                      out_w});
            out = out.view({batch_sz / n_parallel_imgs,
                            n_parallel_imgs,
                            out_channels,
                            out_h,
                            out_w});
            auto out_buf = at::zeros(
                    {batch_sz / n_parallel_imgs,
                     out_channels,
                     n_parallel_imgs,
                     out_h,
                     out_w},
                    out.options());

            // Separate channels into convolution groups
            out_buf = out_buf.view({out_buf.size(0),
                                    groups,
                                    out_buf.size(1) / groups,
                                    out_buf.size(2),
                                    out_buf.size(3),
                                    out_buf.size(4)});
            weight_c = weight_c.view({groups,
                                      weight_c.size(0) / groups,
                                      weight_c.size(1),
                                      weight_c.size(2),
                                      weight_c.size(3)});

            // Sample points and perform convolution
            auto columns = at::zeros(
                    {in_channels * weight_h * weight_w, n_parallel_imgs * out_h * out_w},
                    input_c.options());
            columns = columns.view(
                    {groups, columns.size(0) / groups, columns.size(1)});
            for (int b = 0; b < batch_sz / n_parallel_imgs; b++) {
                im2col(
                        input_c[b],
                        offset_c[b],
                        mask_c[b],
                        in_channels,
                        in_h,
                        in_w,
                        weight_h,
                        weight_w,
                        pad_h,
                        pad_w,
                        stride_h,
                        stride_w,
                        dilation_h,
                        dilation_w,
                        out_h,
                        out_w,
                        n_parallel_imgs,
                        offset_groups,
                        mask_groups,
                        deformable,
                        modulated,
                        columns);

                for (int g = 0; g < groups; g++) {
                    out_buf[b][g] = out_buf[b][g]
                            .flatten(1)
                            .addmm_(weight_c[g].flatten(1), columns[g])
                            .view_as(out_buf[b][g]);
                }
            }

            out_buf = out_buf.view({batch_sz / n_parallel_imgs,
                                    out_channels,
                                    n_parallel_imgs,
                                    out_h,
                                    out_w});
            out_buf.transpose_(1, 2);
            out.copy_(out_buf);
            out = out.view({batch_sz, out_channels, out_h, out_w});

            return out + bias_c.view({1, out_channels, 1, 1});
        }

        std::tuple<at::Tensor, at::Tensor, at::Tensor, at::Tensor, at::Tensor>
        deform_conv2d_backward(
                const at::Tensor &grad_out,
                const at::Tensor &input,
                const at::Tensor &weight,
                const at::Tensor &offset,
                const at::Tensor &mask,
                const at::Tensor &bias,
                const std::pair<int, int> &stride,
                const std::pair<int, int> &padding,
                const std::pair<int, int> &dilation,
                int groups,
                int offset_groups,
                int mask_groups,
                bool deformable,
                bool modulated) {
            at::Tensor grad_out_c = grad_out.contiguous();
            at::Tensor input_c = input.contiguous();
            at::Tensor weight_c = weight.contiguous();
            at::Tensor offset_c = offset.contiguous();
            at::Tensor mask_c = mask.contiguous();
            at::Tensor bias_c = bias.contiguous();

            if (input_c.is_cuda())
                at::DeviceGuard guard(input_c.device());

            int batch_sz = input_c.size(0);
            int in_channels = input_c.size(1);
            int in_h = input_c.size(2);
            int in_w = input_c.size(3);

            int n_parallel_imgs = get_greatest_divisor_below_bound(batch_sz, kMaxParallelImgs);

            int out_channels = weight_c.size(0);
            int weight_h = weight_c.size(2);
            int weight_w = weight_c.size(3);

            int stride_h = stride.first;
            int stride_w = stride.second;

            int pad_h = padding.first;
            int pad_w = padding.second;

            int dilation_h = dilation.first;
            int dilation_w = dilation.second;

            int out_h = (in_h + 2 * pad_h - (dilation_h * (weight_h - 1) + 1)) / stride_h + 1;
            int out_w = (in_w + 2 * pad_w - (dilation_w * (weight_w - 1) + 1)) / stride_w + 1;

            auto grad_input = at::zeros_like(input_c);
            auto grad_weight = at::zeros_like(weight_c);
            auto grad_offset = at::zeros_like(offset_c);
            auto grad_mask = at::zeros_like(mask_c);
            auto grad_bias = at::ones_like(bias_c);

            // Separate into blocks
            grad_input = grad_input.reshape({batch_sz / n_parallel_imgs,
                                             n_parallel_imgs,
                                             in_channels,
                                             in_h,
                                             in_w});
            input_c = input_c.reshape({batch_sz / n_parallel_imgs,
                                       n_parallel_imgs,
                                       in_channels,
                                       in_h,
                                       in_w});

            if (deformable) {
                grad_offset = grad_offset.reshape({batch_sz / n_parallel_imgs,
                                                   n_parallel_imgs,
                                                   offset_groups * 2 * weight_h * weight_w,
                                                   out_h,
                                                   out_w});
                offset_c = offset_c.reshape({batch_sz / n_parallel_imgs,
                                             n_parallel_imgs,
                                             offset_groups * 2 * weight_h * weight_w,
                                             out_h,
                                             out_w});
            }
            if (modulated) {
                grad_mask = grad_mask.reshape({batch_sz / n_parallel_imgs,
                                               n_parallel_imgs,
                                               mask_groups * weight_h * weight_w,
                                               out_h,
                                               out_w});
                mask_c = mask_c.reshape({batch_sz / n_parallel_imgs,
                                         n_parallel_imgs,
                                         mask_groups * weight_h * weight_w,
                                         out_h,
                                         out_w});
            }

            grad_out_c = grad_out_c
                    .reshape({batch_sz / n_parallel_imgs,
                              n_parallel_imgs,
                              groups,
                              out_channels / groups,
                              out_h,
                              out_w})
                    .permute({0, 2, 3, 1, 4, 5}).contiguous();

            grad_weight = grad_weight.reshape({groups,
                                               out_channels / groups,
                                               in_channels / groups,
                                               weight_h,
                                               weight_w});
            weight_c = weight_c.reshape({groups,
                                         out_channels / groups,
                                         in_channels / groups,
                                         weight_h,
                                         weight_w});

            auto columns = at::empty(
                    {in_channels * weight_h * weight_w, n_parallel_imgs * out_h * out_w},
                    input_c.options());
            columns = columns.view(
                    {groups, columns.size(0) / groups, columns.size(1)});

            for (int b = 0; b < batch_sz / n_parallel_imgs; b++) {
                columns.zero_();
                for (int g = 0; g < groups; g++) {
                    columns[g] = columns[g].addmm_(
                            weight_c[g].flatten(1).transpose(0, 1), grad_out_c[b][g].flatten(1));
                }

                auto grad_offset_b = grad_offset[b];
                deform_conv2d_compute_grad_offset(
                        columns,
                        input_c[b],
                        offset_c[b],
                        mask_c[b],
                        in_channels,
                        in_h,
                        in_w,
                        weight_h,
                        weight_w,
                        pad_h,
                        pad_w,
                        stride_h,
                        stride_w,
                        dilation_h,
                        dilation_w,
                        out_h,
                        out_w,
                        n_parallel_imgs,
                        offset_groups,
                        mask_groups,
                        deformable,
                        modulated,
                        grad_offset_b);

                auto grad_mask_b = grad_mask[b];
                deform_conv2d_compute_grad_mask(
                        columns,
                        input_c[b],
                        offset_c[b],
                        in_channels,
                        in_h,
                        in_w,
                        weight_h,
                        weight_w,
                        pad_h,
                        pad_w,
                        stride_h,
                        stride_w,
                        dilation_h,
                        dilation_w,
                        out_h,
                        out_w,
                        n_parallel_imgs,
                        offset_groups,
                        mask_groups,
                        deformable,
                        modulated,
                        grad_mask_b);

                auto grad_input_b = grad_input[b];
                col2im(
                        columns,
                        offset_c[b],
                        mask_c[b],
                        in_channels,
                        in_h,
                        in_w,
                        weight_h,
                        weight_w,
                        pad_h,
                        pad_w,
                        stride_h,
                        stride_w,
                        dilation_h,
                        dilation_w,
                        out_h,
                        out_w,
                        n_parallel_imgs,
                        offset_groups,
                        mask_groups,
                        deformable,
                        modulated,
                        grad_input_b);

                im2col(
                        input_c[b],
                        offset_c[b],
                        mask_c[b],
                        in_channels,
                        in_h,
                        in_w,
                        weight_h,
                        weight_w,
                        pad_h,
                        pad_w,
                        stride_h,
                        stride_w,
                        dilation_h,
                        dilation_w,
                        out_h,
                        out_w,
                        n_parallel_imgs,
                        offset_groups,
                        mask_groups,
                        deformable,
                        modulated,
                        columns);
                for (int g = 0; g < groups; g++) {
                    grad_weight[g] = grad_weight[g].flatten(1).addmm_(grad_out_c[b][g].flatten(1),
                                                                      columns[g].transpose(1, 0)).view_as(
                            grad_weight[g]);
                }
            }

            grad_input = grad_input.view({batch_sz, in_channels, in_h, in_w});
            grad_weight = grad_weight.view({out_channels, in_channels / groups, weight_h, weight_w});
            if (deformable)
                grad_offset = grad_offset.view(
                        {batch_sz, offset_groups * 2 * weight_h * weight_w, out_h, out_w});
            if (modulated)
                grad_mask = grad_mask.view(
                        {batch_sz, mask_groups * weight_h * weight_w, out_h, out_w});
            grad_bias *= grad_out_c.sum(at::IntArrayRef({0, 2, 3}));

            return std::make_tuple(grad_input, grad_weight, grad_offset, grad_mask, grad_bias);
        }
    }
}