#include <ATen/ATen.h>
#include "cpu_helpers.h"
#include "../utils/dispatch.h"

namespace tvdcn {
    namespace ops {
        namespace {
            template<typename scalar_t>
            static inline scalar_t sample(
                    const scalar_t *input,
                    const int height,
                    const int width,
                    const int y,
                    const int x) {
                return (0 <= y && y < height && 0 <= x && x < width) ? input[y * width + x] : static_cast<scalar_t>(0);
            }

            template<typename scalar_t>
            static scalar_t interpolate_sample(
                    const scalar_t *input,
                    const int height,
                    const int width,
                    const scalar_t y,
                    const scalar_t x) {
                if (y <= -1 || height <= y || x <= -1 || width <= x) {
                    return 0;
                }

                int y_l = floor(y);
                int y_h = y_l + 1;
                int x_l = floor(x);
                int x_h = x_l + 1;

                scalar_t dy_h = y - y_l;
                scalar_t dx_h = x - x_l;
                scalar_t dy_l = 1 - dy_h;
                scalar_t dx_l = 1 - dx_h;

                bool valid_y_l = y_l >= 0;
                bool valid_y_h = y_h < height;
                bool valid_x_l = x_l >= 0;
                bool valid_x_h = x_h < width;

                scalar_t val = 0;
                val += (valid_y_l && valid_x_l) ? dy_l * dx_l * input[y_l * width + x_l] : static_cast<scalar_t>(0);
                val += (valid_y_l && valid_x_h) ? dy_l * dx_h * input[y_l * width + x_h] : static_cast<scalar_t>(0);
                val += (valid_y_h && valid_x_l) ? dy_h * dx_l * input[y_h * width + x_l] : static_cast<scalar_t>(0);
                val += (valid_y_h && valid_x_h) ? dy_h * dx_h * input[y_h * width + x_h] : static_cast<scalar_t>(0);

                return val;
            }

            template<typename scalar_t>
            static inline void insert(
                    scalar_t *output,
                    const int height,
                    const int width,
                    const int y,
                    const int x,
                    const scalar_t val) {
                if (0 <= y && y < height && 0 <= x && x < width)
                    output[y * width + x] += val;
            }

            template<typename scalar_t>
            static void interpolate_insert(
                    scalar_t *output,
                    const int height,
                    const int width,
                    const scalar_t y,
                    const scalar_t x,
                    const scalar_t val) {
                int y_l = floor(y);
                int y_h = y_l + 1;
                int x_l = floor(x);
                int x_h = x_l + 1;

                scalar_t dy_h = y - y_l;
                scalar_t dx_h = x - x_l;
                scalar_t dy_l = 1 - dy_h;
                scalar_t dx_l = 1 - dx_h;

                bool valid_y_l = 0 <= y_l && y_l < height;
                bool valid_y_h = 0 <= y_h && y_h < height;
                bool valid_x_l = 0 <= x_l && x_l < width;
                bool valid_x_h = 0 <= x_h && x_h < width;

                if (valid_y_l && valid_x_l) output[y_l * width + x_l] += dy_l * dx_l * val;
                if (valid_y_l && valid_x_h) output[y_l * width + x_h] += dy_l * dx_h * val;
                if (valid_y_h && valid_x_l) output[y_h * width + x_l] += dy_h * dx_l * val;
                if (valid_y_h && valid_x_h) output[y_h * width + x_h] += dy_h * dx_h * val;
            }

            template<typename scalar_t>
            static scalar_t bilinear_coordinate_weight(
                    const scalar_t *input,
                    const int height,
                    const int width,
                    const scalar_t y,
                    const scalar_t x,
                    const int direction) {
                int y_l = floor(y);
                int y_h = y_l + 1;
                int x_l = floor(x);
                int x_h = x_l + 1;

                scalar_t dy_h = (direction == 0) ? static_cast<scalar_t>(1) : y - y_l;
                scalar_t dy_l = (direction == 0) ? static_cast<scalar_t>(-1) : 1 - dy_h;
                scalar_t dx_h = (direction == 1) ? static_cast<scalar_t>(1) : x - x_l;
                scalar_t dx_l = (direction == 1) ? static_cast<scalar_t>(-1) : 1 - dx_h;

                bool valid_y_l = 0 <= y_l && y_l < height;
                bool valid_y_h = 0 <= y_h && y_h < height;
                bool valid_x_l = 0 <= x_l && x_l < width;
                bool valid_x_h = 0 <= x_h && x_h < width;

                scalar_t val = 0;
                val += (valid_y_l && valid_x_l) ? dy_l * dx_l * input[y_l * width + x_l] : static_cast<scalar_t>(0);
                val += (valid_y_l && valid_x_h) ? dy_l * dx_h * input[y_l * width + x_h] : static_cast<scalar_t>(0);
                val += (valid_y_h && valid_x_l) ? dy_h * dx_l * input[y_h * width + x_l] : static_cast<scalar_t>(0);
                val += (valid_y_h && valid_x_h) ? dy_h * dx_h * input[y_h * width + x_h] : static_cast<scalar_t>(0);

                return val;
            }
        }

        template<bool deformable, bool modulated, typename scalar_t>
        static void im2col_kernel(
                const int n_kernels,
                const int c_per_offset_grp,
                const int c_per_mask_grp,
                const scalar_t *input,
                const scalar_t *offset,
                const scalar_t *mask,
                const int height,
                const int width,
                const int weight_h,
                const int weight_w,
                const int pad_h,
                const int pad_w,
                const int stride_h,
                const int stride_w,
                const int dilation_h,
                const int dilation_w,
                const int out_h,
                const int out_w,
                const int batch_sz,
                const int in_channels,
                const int n_offset_grps,
                const int n_mask_grps,
                scalar_t *columns) {
            CPU_1D_KERNEL_LOOP(index, n_kernels) {
                int out_x = index % out_w;
                int out_y = (index / out_w) % out_h;
                int out_b = (index / (out_w * out_h)) % batch_sz;
                int in_c = index / (out_w * out_h * batch_sz);
                int out_c = in_c * weight_h * weight_w;

                int offset_grp = in_c / c_per_offset_grp;
                int mask_grp = in_c / c_per_mask_grp;

                int columns_ptr = (out_c * (batch_sz * out_h * out_w) + out_b * (out_h * out_w) + out_y * out_w +
                                   out_x);
                int input_ptr = (out_b * (in_channels * height * width) + in_c * (height * width));
                int offset_ptr = (out_b * n_offset_grps + offset_grp) * 2 * weight_h * weight_w * out_h * out_w;
                int mask_ptr = (out_b * n_mask_grps + mask_grp) * weight_h * weight_w * out_h * out_w;

                for (int i = 0; i < weight_h; ++i) {
                    for (int j = 0; j < weight_w; ++j) {
                        int mask_idx = i * weight_w + j;
                        int offset_idx = 2 * mask_idx;

                        const scalar_t offset_h = deformable ?
                                                  offset[offset_ptr + offset_idx * (out_h * out_w) + out_y * out_w +
                                                         out_x]
                                                             : static_cast<scalar_t>(0);
                        const scalar_t offset_w = deformable ?
                                                  offset[offset_ptr + (offset_idx + 1) * (out_h * out_w) +
                                                         out_y * out_w +
                                                         out_x]
                                                             : static_cast<scalar_t>(0);
                        const scalar_t y = (out_y * stride_h - pad_h) + i * dilation_h + offset_h;
                        const scalar_t x = (out_x * stride_w - pad_w) + j * dilation_w + offset_w;
                        const scalar_t val = deformable ?
                                             interpolate_sample(input + input_ptr, height, width, y, x)
                                                        : sample(input + input_ptr, height, width,
                                                                 static_cast<int>(y),
                                                                 static_cast<int>(x));

                        const scalar_t mask_val = modulated ?
                                                  mask[mask_ptr + mask_idx * (out_h * out_w) + out_y * out_w +
                                                       out_x]
                                                            : static_cast<scalar_t>(1);

                        columns[columns_ptr] = val * mask_val;
                        columns_ptr += batch_sz * out_h * out_w;
                    }
                }
            }
        }

        void im2col_cpu(
                const at::Tensor &input,
                const at::Tensor &offset,
                const at::Tensor &mask,
                const int in_channels,
                const int height,
                const int width,
                const int weight_h,
                const int weight_w,
                const int pad_h,
                const int pad_w,
                const int stride_h,
                const int stride_w,
                const int dilation_h,
                const int dilation_w,
                const int out_h,
                const int out_w,
                const int batch_sz,
                const int n_offset_grps,
                const int n_mask_grps,
                const bool deformable,
                const bool modulated,
                at::Tensor &columns) {
            const int n_kernels = in_channels * out_h * out_w * batch_sz;
            const int c_per_offset_grp = deformable ? in_channels / n_offset_grps : 1;
            const int c_per_mask_grp = modulated ? in_channels / n_mask_grps : 1;

            AT_DISPATCH_FLOATING_TYPES_AND_HALF(
                    input.scalar_type(), "im2col_cpu", ([&] {
                TVDCN_DISPATCH_CONDITION2(deformable, modulated, ([&] {
                    im2col_kernel<deformable, modulated>(
                            n_kernels,
                            c_per_offset_grp,
                            c_per_mask_grp,
                            input.data_ptr<scalar_t>(),
                            offset.data_ptr<scalar_t>(),
                            mask.data_ptr<scalar_t>(),
                            height,
                            width,
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
                            batch_sz,
                            in_channels,
                            n_offset_grps,
                            n_mask_grps,
                            columns.data_ptr<scalar_t>());
                }));
            }));
        }

        template<bool deformable, bool modulated, typename scalar_t>
        static void col2im_kernel(
                const int n_kernels,
                const int c_per_offset_grp,
                const int c_per_mask_grp,
                const scalar_t *columns,
                const scalar_t *offset,
                const scalar_t *mask,
                const int in_channels,
                const int height,
                const int width,
                const int weight_h,
                const int weight_w,
                const int pad_h,
                const int pad_w,
                const int stride_h,
                const int stride_w,
                const int dilation_h,
                const int dilation_w,
                const int out_h,
                const int out_w,
                const int batch_sz,
                const int n_offset_grps,
                const int n_mask_grps,
                scalar_t *grad_input) {
            CPU_1D_KERNEL_LOOP(index, n_kernels) {
                int out_x = index % out_w;
                int out_y = (index / out_w) % out_h;
                int b = (index / (out_w * out_h)) % batch_sz;
                int j = (index / (out_w * out_h * batch_sz)) % weight_w;
                int i = (index / (out_w * out_h * batch_sz * weight_w)) % weight_h;
                int c = index / (out_w * out_h * batch_sz * weight_w * weight_h);

                int offset_grp = c / c_per_offset_grp;
                int mask_grp = c / c_per_mask_grp;

                int mask_idx = i * weight_w + j;
                int offset_idx = 2 * mask_idx;

                int offset_ptr = (b * n_offset_grps + offset_grp) * 2 * weight_h * weight_w * out_h * out_w;
                int mask_ptr = (b * n_mask_grps + mask_grp) * weight_h * weight_w * out_h * out_w;

                int offset_h_ptr = (offset_idx * out_h + out_y) * out_w + out_x;
                int offset_w_ptr = ((offset_idx + 1) * out_h + out_y) * out_w + out_x;
                const scalar_t offset_h = deformable ?
                                          offset[offset_ptr + offset_h_ptr] : static_cast<scalar_t>(0);
                const scalar_t offset_w = deformable ?
                                          offset[offset_ptr + offset_w_ptr] : static_cast<scalar_t>(0);
                const scalar_t y = (out_y * stride_h - pad_h) + i * dilation_h + offset_h;
                const scalar_t x = (out_x * stride_w - pad_w) + j * dilation_w + offset_w;

                const scalar_t mask_val = modulated ?
                                          mask[mask_ptr + (mask_idx * out_h + out_y) * out_w + out_x]
                                                    : static_cast<scalar_t>(1);

                const scalar_t val = columns[index] * mask_val;

                int grad_input_ptr = (b * in_channels + c) * height * width;
                if (deformable)
                    interpolate_insert(grad_input + grad_input_ptr, height, width, y, x, val);
                else
                    insert(grad_input + grad_input_ptr, height, width, y, x, val);
            }
        }

        void col2im_cpu(
                const at::Tensor &columns,
                const at::Tensor &offset,
                const at::Tensor &mask,
                const int in_channels,
                const int height,
                const int width,
                const int weight_h,
                const int weight_w,
                const int pad_h,
                const int pad_w,
                const int stride_h,
                const int stride_w,
                const int dilation_h,
                const int dilation_w,
                const int out_h,
                const int out_w,
                const int batch_sz,
                const int n_offset_grps,
                const int n_mask_grps,
                const bool deformable,
                const bool modulated,
                at::Tensor &grad_input) {
            const int n_kernels = in_channels * weight_h * weight_w * out_h * out_w * batch_sz;
            const int c_per_offset_grp = deformable ? in_channels / n_offset_grps : 1;
            const int c_per_mask_grp = modulated ? in_channels / n_mask_grps : 1;

            AT_DISPATCH_FLOATING_TYPES_AND_HALF(
                    columns.scalar_type(), "col2im_cpu", ([&] {
                TVDCN_DISPATCH_CONDITION2(deformable, modulated, ([&] {
                    col2im_kernel<deformable, modulated>(
                            n_kernels,
                            c_per_offset_grp,
                            c_per_mask_grp,
                            columns.data_ptr<scalar_t>(),
                            offset.data_ptr<scalar_t>(),
                            mask.data_ptr<scalar_t>(),
                            in_channels,
                            height,
                            width,
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
                            batch_sz,
                            n_offset_grps,
                            n_mask_grps,
                            grad_input.data_ptr<scalar_t>());
                }));
            }));
        }

        template<bool modulated, typename scalar_t>
        static void deform_conv2d_compute_grad_offset_kernel(
                const int n_kernels,
                const int n_offset_kernels,
                const int offset_channels,
                const int c_per_offset_grp,
                const int c_per_mask_grp,
                const scalar_t *columns,
                const scalar_t *input,
                const scalar_t *offset,
                const scalar_t *mask,
                const int in_channels,
                const int height,
                const int width,
                const int weight_h,
                const int weight_w,
                const int pad_h,
                const int pad_w,
                const int stride_h,
                const int stride_w,
                const int dilation_h,
                const int dilation_w,
                const int out_h,
                const int out_w,
                const int batch_sz,
                const int n_offset_grps,
                const int n_mask_grps,
                scalar_t *grad_offset) {
            CPU_1D_KERNEL_LOOP(index, n_offset_kernels) {
                scalar_t grad_offset_val = 0;

                int w = index % out_w;
                int h = (index / out_w) % out_h;
                int c = (index / (out_w * out_h)) % offset_channels;
                int b = index / (out_w * out_h * offset_channels);

                int offset_grp = c / (2 * weight_h * weight_w);

                int col_ptr = offset_grp * c_per_offset_grp * weight_h * weight_w * batch_sz * out_h * out_w;
                int input_ptr = (b * n_offset_grps + offset_grp) * c_per_offset_grp * height * width;
                int offset_ptr = (b * n_offset_grps + offset_grp) * 2 * weight_h * weight_w * out_h * out_w;

                int offset_c = c - offset_grp * 2 * weight_h * weight_w;
                int direction = offset_c % 2;

                const int c_bound = c_per_offset_grp * weight_h * weight_w;
                const int col_step = weight_h * weight_w;
                for (int col_c = (offset_c / 2); col_c < c_bound; col_c += col_step) {
                    int col_pos = (((col_c * batch_sz + b) * out_h) + h) * out_w + w;
                    int in_c = (col_ptr + col_pos) * in_channels / n_kernels;

                    int mask_grp = in_c / c_per_mask_grp;
                    int mask_ptr = (b * n_mask_grps + mask_grp) * weight_h * weight_w * out_h * out_w;

                    int out_x = col_pos % out_w;
                    int out_y = (col_pos / out_w) % out_h;
                    int j = (col_pos / (out_w * out_h * batch_sz)) % weight_w;
                    int i = (col_pos / (out_w * out_h * batch_sz * weight_w)) % weight_h;

                    int mask_idx = i * weight_w + j;
                    int offset_idx = 2 * mask_idx;

                    const scalar_t offset_h = offset[offset_ptr + (offset_idx * out_h + out_y) * out_w + out_x];
                    const scalar_t offset_w = offset[offset_ptr + ((offset_idx + 1) * out_h + out_y) * out_w +
                                                     out_x];
                    const scalar_t y = (out_y * stride_h - pad_h) + i * dilation_h + offset_h;
                    const scalar_t x = (out_x * stride_w - pad_w) + j * dilation_w + offset_w;
                    const scalar_t weight = bilinear_coordinate_weight(input + input_ptr, height, width, y, x,
                                                                       direction);

                    const scalar_t mask_val = modulated ?
                                              mask[mask_ptr + mask_idx * (out_h * out_w) + out_y * out_w + out_x]
                                                        : static_cast<scalar_t>(1);

                    grad_offset_val += columns[col_ptr + col_pos] * weight * mask_val;
                    input_ptr += height * width;
                }

                grad_offset[index] = grad_offset_val;
            }
        }

        void deform_conv2d_compute_grad_offset_cpu(
                const at::Tensor &columns,
                const at::Tensor &input,
                const at::Tensor &offset,
                const at::Tensor &mask,
                const int in_channels,
                const int height,
                const int width,
                const int weight_h,
                const int weight_w,
                const int pad_h,
                const int pad_w,
                const int stride_h,
                const int stride_w,
                const int dilation_h,
                const int dilation_w,
                const int out_h,
                const int out_w,
                const int batch_sz,
                const int n_offset_grps,
                const int n_mask_grps,
                const bool deformable,
                const bool modulated,
                at::Tensor &grad_offset) {
            if (!deformable) return;
            const int n_kernels = out_h * out_w * weight_h * weight_w * in_channels * batch_sz;
            const int n_offset_kernels = out_h * out_w * 2 * weight_h * weight_w * n_offset_grps * batch_sz;
            const int offset_channels = 2 * weight_h * weight_w * n_offset_grps;
            const int c_per_offset_grp = deformable ? in_channels / n_offset_grps : 1;
            const int c_per_mask_grp = modulated ? in_channels / n_mask_grps : 1;

            AT_DISPATCH_FLOATING_TYPES_AND_HALF(
                    columns.scalar_type(), "deform_conv2d_compute_grad_offset_cpu", ([&] {
                TVDCN_DISPATCH_CONDITION(modulated, ([&] {
                    deform_conv2d_compute_grad_offset_kernel<modulated>(
                            n_kernels,
                            n_offset_kernels,
                            offset_channels,
                            c_per_offset_grp,
                            c_per_mask_grp,
                            columns.data_ptr<scalar_t>(),
                            input.data_ptr<scalar_t>(),
                            offset.data_ptr<scalar_t>(),
                            mask.data_ptr<scalar_t>(),
                            in_channels,
                            height,
                            width,
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
                            batch_sz,
                            n_offset_grps,
                            n_mask_grps,
                            grad_offset.data_ptr<scalar_t>());
                }));
            }));
        }

        template<bool deformable, typename scalar_t>
        static void deform_conv2d_compute_grad_mask_kernel(
                const int n_kernels,
                const int n_mask_kernels,
                const int mask_channels,
                const int c_per_offset_grp,
                const int c_per_mask_grp,
                const scalar_t *columns,
                const scalar_t *input,
                const scalar_t *offset,
                const int in_channels,
                const int height,
                const int width,
                const int weight_h,
                const int weight_w,
                const int pad_h,
                const int pad_w,
                const int stride_h,
                const int stride_w,
                const int dilation_h,
                const int dilation_w,
                const int out_h,
                const int out_w,
                const int batch_sz,
                const int n_offset_grps,
                const int n_mask_grps,
                scalar_t *grad_mask) {
            CPU_1D_KERNEL_LOOP(index, n_mask_kernels) {
                scalar_t grad_mask_val = 0;

                int w = index % out_w;
                int h = (index / out_w) % out_h;
                int c = (index / (out_w * out_h)) % mask_channels;
                int b = index / (out_w * out_h * mask_channels);

                int mask_grp = c / (weight_h * weight_w);

                int col_ptr = mask_grp * c_per_mask_grp * weight_h * weight_w * batch_sz * out_h * out_w;
                int input_ptr = (b * n_mask_grps + mask_grp) * c_per_mask_grp * height * width;
                int mask_ptr = (b * n_mask_grps + mask_grp) * weight_h * weight_w * out_h * out_w;

                int mask_c = c - mask_grp * weight_h * weight_w;

                const int c_bound = c_per_mask_grp * weight_h * weight_w;
                const int col_step = weight_h * weight_w;
                for (int col_c = mask_c; col_c < c_bound; col_c += col_step) {
                    int col_pos = (((col_c * batch_sz + b) * out_h) + h) * out_w + w;
                    int in_c = (col_ptr + col_pos) * in_channels / n_kernels;

                    int offset_grp = in_c / c_per_offset_grp;
                    int offset_ptr = (b * n_offset_grps + offset_grp) * 2 * weight_h * weight_w * out_h * out_w;

                    int out_x = col_pos % out_w;
                    int out_y = (col_pos / out_w) % out_h;
                    int j = (col_pos / (out_w * out_h * batch_sz)) % weight_w;
                    int i = (col_pos / (out_w * out_h * batch_sz * weight_w)) % weight_h;

                    int mask_idx = i * weight_w + j;
                    int offset_idx = 2 * mask_idx;

                    const scalar_t offset_h = deformable ?
                                              offset[offset_ptr + (offset_idx * out_h + out_y) * out_w + out_x]
                                                         : static_cast<scalar_t>(0);
                    const scalar_t offset_w = deformable ?
                                              offset[offset_ptr + ((offset_idx + 1) * out_h + out_y) * out_w +
                                                     out_x]
                                                         : static_cast<scalar_t>(0);
                    const scalar_t y = (out_y * stride_h - pad_h) + i * dilation_h + offset_h;
                    const scalar_t x = (out_x * stride_w - pad_w) + j * dilation_w + offset_w;

                    const scalar_t val = deformable ?
                                         interpolate_sample(input + input_ptr, height, width, y, x)
                                                    : sample(input + input_ptr, height, width,
                                                             static_cast<int>(y),
                                                             static_cast<int>(x));

                    grad_mask_val += columns[col_ptr + col_pos] * val;
                    input_ptr += height * width;
                }

                grad_mask[mask_ptr + (mask_c * out_h + h) * out_w + w] = grad_mask_val;
            }
        }

        void deform_conv2d_compute_grad_mask_cpu(
                const at::Tensor &columns,
                const at::Tensor &input,
                const at::Tensor &offset,
                const int in_channels,
                const int height,
                const int width,
                const int weight_h,
                const int weight_w,
                const int pad_h,
                const int pad_w,
                const int stride_h,
                const int stride_w,
                const int dilation_h,
                const int dilation_w,
                const int out_h,
                const int out_w,
                const int batch_sz,
                const int n_offset_grps,
                const int n_mask_grps,
                const bool deformable,
                const bool modulated,
                at::Tensor &grad_mask) {
            if (!modulated) return;
            const int n_kernels = out_h * out_w * weight_h * weight_w * in_channels * batch_sz;
            const int n_mask_kernels = out_h * out_w * weight_h * weight_w * n_mask_grps * batch_sz;
            const int mask_channels = weight_h * weight_w * n_mask_grps;
            const int c_per_offset_grp = deformable ? in_channels / n_offset_grps : 1;
            const int c_per_mask_grp = modulated ? in_channels / n_mask_grps : 1;

            AT_DISPATCH_FLOATING_TYPES_AND_HALF(
                    columns.scalar_type(), "deform_conv2d_compute_grad_mask_cpu", ([&] {
                TVDCN_DISPATCH_CONDITION(deformable, ([&] {
                    deform_conv2d_compute_grad_mask_kernel<deformable>(
                            n_kernels,
                            n_mask_kernels,
                            mask_channels,
                            c_per_offset_grp,
                            c_per_mask_grp,
                            columns.data_ptr<scalar_t>(),
                            input.data_ptr<scalar_t>(),
                            offset.data_ptr<scalar_t>(),
                            in_channels,
                            height,
                            width,
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
                            batch_sz,
                            n_offset_grps,
                            n_mask_grps,
                            grad_mask.data_ptr<scalar_t>());
                }));
            }));
        }
    }
}